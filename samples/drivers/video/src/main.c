/*
 * Copyright (C) 2026 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>

#include <zephyr/drivers/video.h>
#include <soc_common.h>
#include <se_service.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/cache.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <cmsis_core.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(video_app, LOG_LEVEL_INF);

/*
 * Latency probe GPIO (P0_3 on E1C SK).
 *
 * Three scope events on P0_3:
 *   1. Rising edge  — very early boot  (SYS_INIT PRE_KERNEL_1, called from
 *                      app_set_parameters before main() runs)
 *   2. Falling edge — capture started  (just before video_stream_start)
 *   3. Rising edge  — frame received   (video_dequeue returned)
 *
 * No LOG calls inside latency_probe_init — logging is not yet available at
 * the PRE_KERNEL_1 init level.
 */
static const struct gpio_dt_spec latency_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), latency_gpios);

static inline void latency_probe_init(void)
{
	gpio_pin_configure_dt(&latency_pin, GPIO_OUTPUT_ACTIVE);
}

static inline void latency_probe_set(int val)
{
	gpio_pin_set_dt(&latency_pin, val);
}

/*
 * Capture trigger button on LPGPIO 0 (P15_0). The "wakeup_pins" node also
 * doubles as the PM wakeup spec — see the PM section below.
 */
static const struct gpio_dt_spec capture_button =
	GPIO_DT_SPEC_GET_BY_IDX(DT_NODELABEL(wakeup_pins), lpgpios, 0);

static K_SEM_DEFINE(button_wait_sem, 0, 1);
static struct gpio_callback button_cb_data;

static void button_callback(const struct device *dev, struct gpio_callback *cb,
			    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (!k_sem_count_get(&button_wait_sem)) {
		k_sem_give(&button_wait_sem);
	}
}

static int configure_capture_button(void)
{
	int ret;

	if (!gpio_is_ready_dt(&capture_button)) {
		LOG_ERR("Capture button GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&capture_button,
				    GPIO_INPUT | capture_button.dt_flags);
	if (ret) {
		LOG_ERR("Failed to configure capture button: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&capture_button,
					      GPIO_INT_EDGE_FALLING);
	if (ret) {
		LOG_ERR("Failed to configure capture button interrupt: %d", ret);
		return ret;
	}

	gpio_init_callback(&button_cb_data, button_callback,
			   BIT(capture_button.pin));
	ret = gpio_add_callback(capture_button.port, &button_cb_data);
	if (ret) {
		LOG_ERR("Failed to add capture button callback: %d", ret);
		return ret;
	}

	LOG_INF("Capture button configured (LPGPIO%d)", capture_button.pin);
	return 0;
}

static void wait_for_capture_button(void)
{
	LOG_INF("Press the button to capture another picture...");
	k_sem_reset(&button_wait_sem);
	k_sem_take(&button_wait_sem, K_FOREVER);
	LOG_INF("Button pressed, starting next capture");
}

/*
 * Power management: STOP-mode SUSPEND_TO_RAM with LPGPIO wakeup.
 *
 * Pattern adapted from alif/samples/simple_pm — the sleep window is the
 * k_sem_take in wait_for_capture_button(). When the policy lock is released
 * (app_allow_sleep), the idle thread can pick PM_STATE_SUSPEND_TO_RAM; the
 * LPGPIO falling edge fires EWIC, the PM notifier restores the run profile
 * before devices resume, and the button callback gives the semaphore.
 */
#define RET_A1 SRAM4_1_MASK
#define RET_A2 SRAM5_1_MASK
#define RET_B  (SRAM4_2_MASK | SRAM5_2_MASK)
#define RET_C  (SRAM4_3_MASK | SRAM5_3_MASK)
#define RET_D  (SRAM4_4_MASK | SRAM5_4_MASK)
#define RET_E  (SRAM5_5_MASK)
#define APP_RET_MEM_BLOCKS (RET_A1 | RET_A2 | RET_B | RET_C | RET_D | RET_E)
#define SERAM_MEMORY_BLOCKS_IN_USE \
	(SERAM_1_MASK | SERAM_2_MASK | SERAM_3_MASK | SERAM_4_MASK)

static volatile int run_profile_error;

static int app_set_parameters(void); /* forward decl: re-run on S2RAM resume */

static int set_off_profile_stop(void)
{
	off_profile_t offp;
	int ret;

	offp.power_domains = PD_VBAT_AON_MASK;
#if (CONFIG_FLASH_BASE_ADDRESS == 0)
	offp.memory_blocks = 0;
#else
	offp.memory_blocks = MRAM_MASK;
#endif
	offp.memory_blocks |= SERAM_MEMORY_BLOCKS_IN_USE | APP_RET_MEM_BLOCKS;
	offp.dcdc_voltage = 775;
	offp.ip_clock_gating = 0;
	offp.phy_pwr_gating = 0;
	offp.dcdc_mode = DCDC_MODE_OFF;
	offp.aon_clk_src = CLK_SRC_LFXO;
	offp.stby_clk_src = CLK_SRC_HFRC;
	offp.stby_clk_freq = SCALED_FREQ_RC_STDBY_0_075_MHZ;
	offp.ewic_cfg = EWIC_VBAT_GPIO;
	offp.wakeup_events = WE_LPGPIO0;
	offp.vtor_address = SCB->VTOR;
	offp.vtor_address_ns = SCB->VTOR;

	ret = se_service_set_off_cfg(&offp);
	if (ret) {
		LOG_ERR("SE: set_off_cfg failed = %d", ret);
	}
	return ret;
}

static void pm_notify_state_entry(enum pm_state const state)
{
	ARG_UNUSED(state);
}

static void pm_notify_pre_device_resume(enum pm_state const state)
{
	if (state == PM_STATE_SUSPEND_TO_RAM) {
		run_profile_error = app_set_parameters();
	}
}

static struct pm_notifier app_pm_notifier = {
	.state_entry = pm_notify_state_entry,
	.pre_device_resume = pm_notify_pre_device_resume,
};

static void app_disable_sleep(void)
{
	pm_policy_state_lock_get(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
}

static void app_allow_sleep(void)
{
	pm_policy_state_lock_put(PM_STATE_SOFT_OFF, PM_ALL_SUBSTATES);
	pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);
}

static int app_pre_kernel_init(void)
{
	pm_notifier_register(&app_pm_notifier);
	app_disable_sleep();
	return 0;
}
SYS_INIT(app_pre_kernel_init, PRE_KERNEL_1, 47);

/*
 * Idle-timer hooks for the Cortex-M systick LPM driver. Measures actual
 * sleep duration via the RTC so the kernel tick can be advanced correctly
 * on resume.
 */
static uint32_t idle_timer_pre_idle;
static const struct device *idle_timer =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_cortex_m_idle_timer));

void z_cms_lptim_hook_on_lpm_entry(uint64_t max_lpm_time_us)
{
	counter_get_value(idle_timer, &idle_timer_pre_idle);

	struct counter_alarm_cfg cfg = {
		.callback = NULL,
		.ticks = counter_us_to_ticks(idle_timer, max_lpm_time_us) +
			 idle_timer_pre_idle,
		.user_data = NULL,
		.flags = COUNTER_ALARM_CFG_ABSOLUTE,
	};
	counter_set_channel_alarm(idle_timer, 0, &cfg);
}

uint64_t z_cms_lptim_hook_on_lpm_exit(void)
{
	uint32_t idle_timer_post, idle_timer_diff;

	counter_get_value(idle_timer, &idle_timer_post);
	if (idle_timer_pre_idle > idle_timer_post) {
		idle_timer_diff =
			(counter_get_top_value(idle_timer) - idle_timer_pre_idle) +
			idle_timer_post + 1;
	} else {
		idle_timer_diff = idle_timer_post - idle_timer_pre_idle;
	}

	return (uint64_t)counter_ticks_to_us(idle_timer, idle_timer_diff);
}

/*
 * OV5640 JPEG snapshot capture.
 *
 * The OV5640 is configured for full-resolution (2592x1944) JPEG output via
 * the parallel CPI / LPCAM path. We allocate one buffer bounded at
 * JPEG_CAPTURE_MAX_BYTES (kept in sync with CONFIG_VIDEO_BUFFER_POOL_SZ_MAX
 * in the board .conf — that value must be this plus ~1 KB for k_heap
 * block headers / alignment overhead) and scan the dequeued buffer for the
 * JPEG EOI marker to determine the actual compressed size.
 */
#define JPEG_CAPTURE_MAX_BYTES (420U * 1024U)

int main(void)
{
	struct video_buffer *vbuf;
	struct video_format fmt = { 0 };
	struct video_caps caps;
	const struct device *video;
	struct video_buffer *buf;
	size_t bsize;
	int ret;
	int i = 0;

	video = DEVICE_DT_GET_ONE(alif_cam);

	if (!device_is_ready(video)) {
		LOG_ERR("%s: device not ready.", video->name);
		return -1;
	}
	LOG_INF("- Device name: %s", video->name);

	if (video_get_caps(video, VIDEO_EP_OUT, &caps)) {
		LOG_ERR("Unable to retrieve video capabilities");
		return -1;
	}

	LOG_INF("- Capabilities:");
	while (caps.format_caps[i].pixelformat) {
		const struct video_format_cap *fcap = &caps.format_caps[i];

		LOG_INF("  %c%c%c%c width (min, max, step)[%u; %u; %u] "
			"height (min, max, step)[%u; %u; %u]",
			(char)fcap->pixelformat,
			(char)(fcap->pixelformat >> 8),
			(char)(fcap->pixelformat >> 16),
			(char)(fcap->pixelformat >> 24),
			fcap->width_min, fcap->width_max, fcap->width_step,
			fcap->height_min, fcap->height_max, fcap->height_step);

		if (fcap->pixelformat == VIDEO_PIX_FMT_JPEG) {
			fmt.pixelformat = VIDEO_PIX_FMT_JPEG;
			fmt.width = 2592;
			fmt.height = 1944;
		}
		i++;
	}

	if (fmt.pixelformat == 0) {
		LOG_ERR("JPEG pixel format not advertised by sensor.");
		return -1;
	}

	fmt.pitch = fmt.width;

	ret = video_set_format(video, VIDEO_EP_OUT, &fmt);
	if (ret) {
		LOG_ERR("Failed to set video format. ret - %d", ret);
		return -1;
	}

	LOG_INF("- format: %c%c%c%c %ux%u",
		(char)fmt.pixelformat,
		(char)(fmt.pixelformat >> 8),
		(char)(fmt.pixelformat >> 16),
		(char)(fmt.pixelformat >> 24),
		fmt.width, fmt.height);

	bsize = MIN((size_t)JPEG_CAPTURE_MAX_BYTES,
		    (size_t)fmt.width * fmt.height);

	LOG_INF("Width - %d, Pitch - %d, Height - %d, Buff size - %zu",
		fmt.width, fmt.pitch, fmt.height, bsize);

	buf = video_buffer_alloc(bsize, K_NO_WAIT);
	if (buf == NULL) {
		LOG_ERR("Unable to alloc video buffer");
		return -1;
	}

	LOG_INF("- addr - 0x%x, size - %zu, bytesused - %d, resolution - %ux%u",
		(uint32_t)buf->buffer, bsize, buf->bytesused, fmt.width, fmt.height);
	LOG_INF("capture buffer: dump binary memory "
		"\"/home/$USER/capture.bin\" 0x%08x 0x%08x -r",
		(uint32_t)buf->buffer, (uint32_t)buf->buffer + bsize - 1);

	ret = configure_capture_button();
	if (ret) {
		return ret;
	}

	/*
	 * Stage the SE off profile (STOP-mode, LPGPIO wakeup). Without this the
	 * idle thread cannot enter SUSPEND_TO_RAM even after sleep is allowed.
	 */
	ret = set_off_profile_stop();
	if (ret) {
		LOG_ERR("Failed to set off profile: %d", ret);
		return ret;
	}

	/*
	 * Capture loop: take a picture on each iteration. After the first
	 * capture, the board enters SUSPEND_TO_RAM until a button press wakes
	 * it.
	 */
	bool first_capture = true;

	while (1) {
		if (!first_capture) {
			if (run_profile_error) {
				LOG_ERR("run profile restore failed: %d",
					run_profile_error);
				return run_profile_error;
			}

			app_allow_sleep();
			wait_for_capture_button();
			app_disable_sleep();
		}
		first_capture = false;

		ret = video_enqueue(video, VIDEO_EP_OUT, buf);
		if (ret) {
			LOG_ERR("Unable to enqueue buf: %d", ret);
			return -1;
		}

		latency_probe_set(0); /* LOW: capture in progress */
		ret = video_stream_start(video);
		if (ret) {
			latency_probe_set(1); /* restore HIGH on error */
			LOG_ERR("Unable to start capture. ret - %d", ret);
			return -1;
		}

		LOG_INF("Capture started");

		/*
		 * JPEG capture: the CPI driver stops capture on the 2nd VSYNC
		 * (end-of-frame) and moves the buffer to fifo_out. We block on
		 * video_dequeue until that happens, then scan the buffer once
		 * for the JPEG EOI marker to determine the actual compressed
		 * size.
		 *
		 * To avoid false-matching an EOI inside an EXIF thumbnail, we
		 * parse past the SOS (Start of Scan) marker and only scan the
		 * entropy-coded data. JPEG byte-stuffing guarantees 0xFF 0xD9
		 * cannot appear as a false positive within compressed data.
		 */
		ret = video_dequeue(video, VIDEO_EP_OUT, &vbuf, K_FOREVER);
		latency_probe_set(1); /* HIGH: frame received */
		if (ret) {
			LOG_ERR("Unable to dequeue video buf: %d", ret);
			return -1;
		}
		LOG_INF("Frame captured, scanning for JPEG EOI...");

		{
			uint8_t *bytes = vbuf->buffer;
			int jpeg_size = -1;
			int scan_from = 2;

			if (bytes[0] != 0xFF || bytes[1] != 0xD8) {
				LOG_ERR("No JPEG SOI marker (got %02x %02x)",
					bytes[0], bytes[1]);
				return -1;
			}

			/* Skip marker segments until SOS (0xFF 0xDA). */
			{
				int pos = 2;

				while (pos < (int)bsize - 3) {
					if (bytes[pos] != 0xFF) {
						break;
					}
					uint8_t marker = bytes[pos + 1];

					if (marker == 0xDA) {
						uint16_t seg_len =
							((uint16_t)bytes[pos + 2] << 8) |
							bytes[pos + 3];
						scan_from = pos + 2 + seg_len;
						break;
					}
					if (marker == 0x00 || marker == 0x01 ||
					    (marker >= 0xD0 && marker <= 0xD9)) {
						pos += 2;
						continue;
					}
					uint16_t seg_len =
						((uint16_t)bytes[pos + 2] << 8) |
						bytes[pos + 3];
					pos += 2 + seg_len;
				}
			}

			for (int j = scan_from; j < (int)bsize - 1; j++) {
				if (bytes[j] == 0xFF && bytes[j + 1] == 0xD9) {
					jpeg_size = j + 2;
					break;
				}
			}

			if (jpeg_size > 0) {
				LOG_INF("JPEG EOI at offset %d (%d KB)",
					jpeg_size, jpeg_size / 1024);
				vbuf->bytesused = jpeg_size;
			} else {
				LOG_ERR("JPEG EOI not found in %zu byte buffer",
					bsize);
				LOG_INF("First 16 bytes: %02x %02x %02x %02x "
					"%02x %02x %02x %02x %02x %02x %02x "
					"%02x %02x %02x %02x %02x",
					bytes[0], bytes[1], bytes[2], bytes[3],
					bytes[4], bytes[5], bytes[6], bytes[7],
					bytes[8], bytes[9], bytes[10], bytes[11],
					bytes[12], bytes[13], bytes[14], bytes[15]);
				return -1;
			}
		}

		LOG_INF("Got frame! size: %u bytes, timestamp: %u ms",
			vbuf->bytesused, vbuf->timestamp);

		video_flush(video, VIDEO_EP_OUT, false);
		ret = video_stream_stop(video);
		latency_probe_set(0); /* LOW: capture sequence done */
		if (ret) {
			LOG_ERR("Unable to stop capture: %d", ret);
		}
	}

	return 0;
}

static int app_set_parameters(void)
{
	run_profile_t runp = { 0 };
	int ret;

	/*
	 * Latency probe event 1: boot reference. Pin goes HIGH here at
	 * PRE_KERNEL_1 — the earliest rising edge the scope will see, well
	 * before main() is entered.
	 */
	latency_probe_init();

	runp.power_domains  = PD_SYST_MASK | PD_SSE700_AON_MASK | PD_DBSS_MASK;
	runp.dcdc_voltage   = 825;
	runp.dcdc_mode      = DCDC_MODE_PWM;
	runp.aon_clk_src    = CLK_SRC_LFXO;
	runp.run_clk_src    = CLK_SRC_PLL;
	runp.vdd_ioflex_3V3 = IOFLEX_LEVEL_1V8;
	runp.cpu_clk_freq   = CLOCK_FREQUENCY_160MHZ;

	runp.memory_blocks = MRAM_MASK;
#if DT_NODE_EXISTS(DT_NODELABEL(sram0))
	runp.memory_blocks |= SRAM0_MASK;
#endif

	runp.phy_pwr_gating  = MIPI_TX_DPHY_MASK | MIPI_RX_DPHY_MASK |
			       MIPI_PLL_DPHY_MASK | LDO_PHY_MASK | USB_PHY_MASK;
	runp.ip_clock_gating = CAMERA_MASK | MIPI_CSI_MASK | MIPI_DSI_MASK |
			       USB_MASK;

	ret = se_service_set_run_cfg(&runp);
	if (ret) {
		__ASSERT(false, "SE: set_run_cfg failed = %d", ret);
		return ret;
	}

	/* LPCAM pixel clock and OV5640 enable-buffer GPIO. */
	sys_write32(0x080001, M55HE_CFG_HE_CAMERA_PIXCLK);

	const struct gpio_dt_spec cam_enbuf =
		GPIO_DT_SPEC_GET(DT_NODELABEL(cam_enbuf), enbuf_gpios);

	gpio_pin_configure_dt(&cam_enbuf, GPIO_OUTPUT_ACTIVE);

	return 0;
}

SYS_INIT(app_set_parameters, PRE_KERNEL_1, 46);
