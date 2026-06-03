/*
 * Copyright (C) 2026 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal video capture sample.
 * Captures a few frames and prints buffer addresses for memory dump.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/video/video_alif.h>
#include <zephyr/drivers/gpio.h>
#include <soc_common.h>
#include <se_service.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(video_capture, LOG_LEVEL_INF);

#define ISP_ENABLED DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(isp))

#if ISP_ENABLED
#include <zephyr/drivers/video/isp-vsi.h>
#define OUTPUT_FORMAT VIDEO_PIX_FMT_RGB888_PLANAR_PRIVATE
#endif

#define VIDEO_BUFFER_COUNT CONFIG_VIDEO_BUFFER_POOL_NUM_MAX

#define PIPELINE_FORMAT VIDEO_PIX_FMT_Y10P
#define N_FRAMES 3

static int fourcc_to_pitch(uint32_t fourcc, uint32_t width)
{
	switch (fourcc) {
	case VIDEO_PIX_FMT_RGB888_PLANAR_PRIVATE:
		return width * 3;
	case VIDEO_PIX_FMT_RGB565:
	case VIDEO_PIX_FMT_Y10P:
	case VIDEO_PIX_FMT_YUYV:
		return width << 1;
	case VIDEO_PIX_FMT_BGGR8:
	case VIDEO_PIX_FMT_GBRG8:
	case VIDEO_PIX_FMT_GRBG8:
	case VIDEO_PIX_FMT_RGGB8:
	case VIDEO_PIX_FMT_GREY:
	default:
		return width;
	}
}

int main(void)
{
	struct video_buffer *buffers[VIDEO_BUFFER_COUNT], *vbuf;
	struct video_format fmt = {0};
	struct video_caps caps;
	const struct device *video;
	enum video_endpoint_id ep;
	size_t bsize;
	int i = 0;
	int ret;

#if ISP_ENABLED
	video = DEVICE_DT_GET_ONE(vsi_isp_pico);
#else
	video = DEVICE_DT_GET_ONE(alif_cam);
#endif

	if (!device_is_ready(video)) {
		LOG_ERR("%s: device not ready.", video->name);
		return -1;
	}
	LOG_INF("Device: %s", video->name);

	/* Get capabilities from the pipeline input side */
#if ISP_ENABLED
	ep = VIDEO_EP_IN;
#else
	ep = VIDEO_EP_OUT;
#endif

	ret = video_get_caps(video, ep, &caps);
	if (ret) {
		LOG_ERR("Unable to retrieve video capabilities");
		return -1;
	}

	/* Find Y10P format in caps */
	while (caps.format_caps[i].pixelformat) {
		const struct video_format_cap *fcap = &caps.format_caps[i];

		LOG_INF("Cap: %c%c%c%c %ux%u",
			(char)fcap->pixelformat,
			(char)(fcap->pixelformat >> 8),
			(char)(fcap->pixelformat >> 16),
			(char)(fcap->pixelformat >> 24),
			fcap->width_min, fcap->height_min);

		if (fcap->pixelformat == PIPELINE_FORMAT) {
			fmt.pixelformat = PIPELINE_FORMAT;
			fmt.width = fcap->width_min;
			fmt.height = fcap->height_min;
		}
		i++;
	}

	if (fmt.pixelformat == 0) {
		LOG_ERR("Desired pixel format not supported");
		return -1;
	}

	fmt.pitch = fourcc_to_pitch(fmt.pixelformat, fmt.width);

	ret = video_set_format(video, ep, &fmt);
	if (ret) {
		LOG_ERR("Failed to set video format: %d", ret);
		return -1;
	}

	LOG_INF("Pipeline format: %c%c%c%c %ux%u",
		(char)fmt.pixelformat,
		(char)(fmt.pixelformat >> 8),
		(char)(fmt.pixelformat >> 16),
		(char)(fmt.pixelformat >> 24),
		fmt.width, fmt.height);

#if ISP_ENABLED
	/* Set ISP output format */
	fmt.pixelformat = OUTPUT_FORMAT;
	fmt.width = 480;
	fmt.height = 480;
	fmt.pitch = fourcc_to_pitch(fmt.pixelformat, fmt.width);

	ret = video_set_format(video, VIDEO_EP_OUT, &fmt);
	if (ret) {
		LOG_ERR("Failed to set ISP output format: %d", ret);
		return -1;
	}

	LOG_INF("Output format: %c%c%c%c %ux%u",
		(char)fmt.pixelformat,
		(char)(fmt.pixelformat >> 8),
		(char)(fmt.pixelformat >> 16),
		(char)(fmt.pixelformat >> 24),
		fmt.width, fmt.height);
#endif

	/* Allocate and enqueue video buffers */
	bsize = fmt.pitch * fmt.height;
	LOG_INF("Buffer size: %u (pitch=%u height=%u)", bsize, fmt.pitch, fmt.height);

	for (i = 0; i < ARRAY_SIZE(buffers); i++) {
		buffers[i] = video_buffer_alloc(bsize, K_NO_WAIT);
		if (buffers[i] == NULL) {
			LOG_ERR("Unable to alloc video buffer");
			return -1;
		}

		LOG_INF("Buffer[%d]: addr=0x%08x size=%u",
			i, (uint32_t)buffers[i]->buffer, bsize);

		video_enqueue(video, VIDEO_EP_OUT, buffers[i]);
	}

	/* Start streaming */
	ret = video_stream_start(video);
	if (ret) {
		LOG_ERR("Unable to start capture: %d", ret);
		return -1;
	}

	LOG_INF("Capture started - waiting for %d frames", N_FRAMES);

	/* Capture frames */
	for (i = 0; i < N_FRAMES; i++) {
		ret = video_dequeue(video, VIDEO_EP_OUT, &vbuf, K_FOREVER);
		if (ret) {
			LOG_ERR("Unable to dequeue video buf: %d", ret);
			break;
		}

		LOG_INF("Frame %d: addr=0x%08x bytesused=%u timestamp=%u ms",
			i, (uint32_t)vbuf->buffer, vbuf->bytesused, vbuf->timestamp);

		/* Print dump command for debugger */
		LOG_INF("  dump binary memory \"/tmp/frame_%d.bin\" 0x%08x 0x%08x",
			i, (uint32_t)vbuf->buffer,
			(uint32_t)vbuf->buffer + vbuf->bytesused - 1);

		/* Quick sanity: check first 16 bytes */
		uint8_t *p = vbuf->buffer;

		LOG_INF("  first 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x",
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
			p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);

		/* Re-enqueue for next capture */
		ret = video_enqueue(video, VIDEO_EP_OUT, vbuf);
		if (ret) {
			LOG_ERR("Unable to requeue video buf: %d", ret);
			break;
		}

		ret = video_stream_start(video);
		if (ret && ret != -EBUSY) {
			LOG_ERR("Unable to restart capture: %d", ret);
			break;
		}
	}

	LOG_INF("Capture complete");

	video_flush(video, VIDEO_EP_OUT, false);
	video_stream_stop(video);

	return 0;
}

/*
 * System configuration - based on the working img_class pattern.
 * Zero-initialized run_profile_t is critical for correct PHY config.
 */
static int app_set_parameters(void)
{
#if (DT_NODE_HAS_STATUS(DT_NODELABEL(cam), okay))
	int ret;
	run_profile_t runp = {0};

#if (DT_NODE_HAS_STATUS(DT_NODELABEL(camera_select), okay))
	const struct gpio_dt_spec sel =
		GPIO_DT_SPEC_GET(DT_NODELABEL(camera_select), select_gpios);

	gpio_pin_configure_dt(&sel, GPIO_OUTPUT);
	gpio_pin_set_dt(&sel, 1);
#endif

	runp.power_domains = PD_SYST_MASK | PD_SSE700_AON_MASK | PD_DBSS_MASK;
	runp.dcdc_voltage  = 825;
	runp.dcdc_mode     = DCDC_MODE_PWM;
	runp.aon_clk_src   = CLK_SRC_LFXO;
	runp.run_clk_src   = CLK_SRC_PLL;
	runp.vdd_ioflex_3V3 = IOFLEX_LEVEL_1V8;
#if defined(CONFIG_RTSS_HP)
	runp.cpu_clk_freq  = CLOCK_FREQUENCY_400MHZ;
#else
	runp.cpu_clk_freq  = CLOCK_FREQUENCY_160MHZ;
#endif

	runp.memory_blocks = MRAM_MASK;
#if DT_NODE_EXISTS(DT_NODELABEL(sram0))
	runp.memory_blocks |= SRAM0_MASK;
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(sram1))
	runp.memory_blocks |= SRAM1_MASK;
#endif

	runp.phy_pwr_gating |= MIPI_TX_DPHY_MASK | MIPI_RX_DPHY_MASK |
		MIPI_PLL_DPHY_MASK | LDO_PHY_MASK;
	runp.ip_clock_gating = CAMERA_MASK | MIPI_CSI_MASK | MIPI_DSI_MASK;

	ret = se_service_set_run_cfg(&runp);
	__ASSERT(ret == 0, "SE: set_run_cfg failed = %d", ret);

	/*
	 * CPI Pixel clock - Generate XVCLK.
	 * Used by ARX3A0 & OV5675 sensors.
	 */
#if defined(CONFIG_VIDEO_MIPI_CSI2_DW)
	sys_write32(0x140001, CLKCTRL_PER_MST_CAMERA_PIXCLK_CTRL);
#endif
#endif /* DT_NODE_HAS_STATUS(DT_NODELABEL(cam), okay) */

	return 0;
}

SYS_INIT(app_set_parameters, PRE_KERNEL_1, 46);
