/*
 * Copyright (C) 2026 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 *
 * OV5640 JPEG capture with a memory dump instead of USB.
 *
 * This is the no-USB sibling of video_usbout_uvc. The camera pipeline and the
 * capture/validation logic are identical, but there is no USB device stack:
 * each captured JPEG is dumped straight from the DTCM capture buffer over the
 * console (framed hex, plus its address/size for a debugger memory dump). That
 * lets the HE core run the aggressive scaled-HFRC 38.4 MHz profile that broke
 * USB high-speed enumeration in video_usbout_uvc — with USB out of the picture
 * the PLL is no longer required, so the low-clock capture can be exercised and
 * inspected in isolation.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/cache.h>
#include <zephyr/console/console.h>
#include <soc_common.h>
#include <se_service.h>
#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(video_memdump, LOG_LEVEL_INF);

/* Declared capture geometry (full-resolution OV5640 JPEG). */
#define FRAME_WIDTH  2592U
#define FRAME_HEIGHT 1944U

/*
 * JPEG capture buffer size, maximised for this board. The buffer is carved
 * from the video pool in the NON_SECURE0 region. This value is bounded by
 * CONFIG_VIDEO_BUFFER_POOL_SZ_MAX (board .conf), which must be this plus
 * ~1.5 KB for k_heap block headers / alignment overhead. Keep both in sync.
 * See the .overlay for the DTCM memory budget.
 */
#define JPEG_CAPTURE_MAX_BYTES (672U * 1024U)

/*
 * Sanity floor for a captured full-resolution JPEG. A real 2592x1944 frame is
 * far larger than this; anything smaller indicates a truncated/garbled capture
 * (e.g. find_jpeg_size latched onto an early false EOI) and is dropped.
 */
#define JPEG_MIN_VALID_BYTES (8U * 1024U)

/*
 * Alignment of the capture buffer in DTCM. The CPI/LPCAM driver only requires
 * 8-byte alignment for the AXI frame address, but on enqueue it does a
 * sys_cache_data_flush_and_invd_range() over the whole buffer. Over-aligning
 * the start to 128 bytes (and JPEG_CAPTURE_MAX_BYTES is a multiple of 128, so
 * the end lands on a boundary too) keeps that cache maintenance from touching
 * neighbouring heap metadata in the NON_SECURE0 region.
 */
#define CAPTURE_BUF_ALIGN 128U

/*
 * Upper bound on how long a single snapshot capture may take. A real capture
 * completes within a few sensor frame periods; the generous 2 s ceiling only
 * trips when the CPI driver dropped the frame as corrupt and never delivered a
 * buffer. Without it, video_dequeue(K_FOREVER) would block forever.
 */
#define CAPTURE_TIMEOUT K_MSEC(2000)

/*
 * OV5640 JPEG VFIFO overflow status register. A non-zero value means the
 * sensor's internal JPEG FIFO overflowed while emitting the frame: it then
 * drops entropy-coded bytes mid/late in the image yet still appends a valid
 * EOI marker. Logged per frame so the low-clock capture can be checked for
 * sensor-side JFIFO overflow.
 */
#define OV5640_JFIFO_OVERFLOW_REG 0x4417U

static const struct i2c_dt_spec ov5640_i2c =
	I2C_DT_SPEC_GET(DT_NODELABEL(ov5640));

static void log_jfifo_overflow(void)
{
	uint8_t addr_buf[2] = {
		(uint8_t)(OV5640_JFIFO_OVERFLOW_REG >> 8),
		(uint8_t)(OV5640_JFIFO_OVERFLOW_REG & 0xFF),
	};
	uint8_t val = 0;
	int ret;

	ret = i2c_write_read_dt(&ov5640_i2c, addr_buf, sizeof(addr_buf),
				&val, sizeof(val));
	if (ret) {
		LOG_WRN("OV5640 JFIFO overflow read failed: %d", ret);
		return;
	}

	if (val != 0) {
		LOG_WRN("OV5640 JFIFO OVERFLOW: reg 0x4417 = 0x%02x "
			"(sensor dropped JPEG entropy bytes this frame)", val);
	} else {
		LOG_INF("OV5640 JFIFO ok: reg 0x4417 = 0x%02x", val);
	}
}

/*
 * Determine the compressed size of a captured JPEG frame.
 *
 * The capture buffer is zero-filled before every capture and the sensor emits
 * no dummy padding after the image, so the populated region is exactly one
 * JPEG ending in EOI (0xFF 0xD9) followed by zero padding. Scanning backwards
 * from the end (rather than forwards for the first EOI) avoids latching onto a
 * false 0xFF 0xD9 produced by a bit glitch inside the entropy-coded data.
 *
 * Returns the JPEG length, or -EIO if the buffer does not start with SOI or
 * does not end in EOI (truncated / overflowed / garbled frame).
 */
static int find_jpeg_size(const uint8_t *bytes, size_t bsize)
{
	size_t end = bsize;

	if (bsize < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) {
		LOG_ERR("No JPEG SOI marker (got %02x %02x)",
			bytes[0], bytes[1]);
		return -EIO;
	}

	while (end > 0 && bytes[end - 1] == 0x00) {
		end--;
	}

	if (end < 4 || bytes[end - 1] != 0xD9 || bytes[end - 2] != 0xFF) {
		LOG_ERR("JPEG does not end in EOI (truncated/garbled capture)");
		return -EIO;
	}

	if (end >= bsize) {
		LOG_ERR("JPEG filled the whole %zu byte capture buffer "
			"(overflow) - increase JPEG_CAPTURE_MAX_BYTES or "
			"raise the sensor JPEG compression (QS).", bsize);
		return -EIO;
	}

	return (int)end;
}

/*
 * Capture one JPEG frame into buf. The OV5640 JPEG path runs the CPI in
 * snapshot mode (one frame per stream_start/stop). On success buf->bytesused
 * holds the compressed JPEG size.
 */
static int capture_one_jpeg(const struct device *video,
			    struct video_buffer *buf, size_t bsize)
{
	struct video_buffer *vbuf;
	int jpeg_size;
	int ret;

	/* Zero first so a truncated frame has no stale EOI to fool find_jpeg_size. */
	memset(buf->buffer, 0, bsize);

	ret = video_enqueue(video, VIDEO_EP_OUT, buf);
	if (ret) {
		LOG_ERR("Unable to enqueue buf: %d", ret);
		return ret;
	}

	ret = video_stream_start(video);
	if (ret) {
		LOG_ERR("Unable to start capture: %d", ret);
		return ret;
	}

	ret = video_dequeue(video, VIDEO_EP_OUT, &vbuf, CAPTURE_TIMEOUT);
	if (ret == -EAGAIN) {
		LOG_WRN("Capture timed out; driver dropped the frame, reclaiming");
		log_jfifo_overflow();
		video_stream_stop(video);
		video_flush(video, VIDEO_EP_OUT, false);
		while (video_dequeue(video, VIDEO_EP_OUT, &vbuf, K_NO_WAIT) == 0) {
			/* Drain the reclaimed buffer so it can be re-enqueued. */
		}
		return -EAGAIN;
	}
	if (ret) {
		LOG_ERR("Unable to dequeue video buf: %d", ret);
		return ret;
	}

	/* CPI does cache maintenance only on enqueue; invalidate so the CPU
	 * reads the freshly DMA'd JPEG rather than stale cache lines. */
	sys_cache_data_invd_range(vbuf->buffer, bsize);

	log_jfifo_overflow();

	jpeg_size = find_jpeg_size(vbuf->buffer, bsize);
	if (jpeg_size < 0 || jpeg_size < (int)JPEG_MIN_VALID_BYTES) {
		video_flush(video, VIDEO_EP_OUT, false);
		video_stream_stop(video);
		return (jpeg_size < 0) ? jpeg_size : -EIO;
	}

	vbuf->bytesused = jpeg_size;

	video_flush(video, VIDEO_EP_OUT, false);
	ret = video_stream_stop(video);
	if (ret) {
		LOG_ERR("Unable to stop capture: %d", ret);
	}
	return 0;
}

/*
 * Dump the captured JPEG from memory over the console. The frame is framed
 * with markers and its size so a host script can extract it from the serial
 * log, e.g.:
 *
 *   python3 - <<'EOF'
 *   import re,sys
 *   t=open('capture.log').read()
 *   m=re.search(r'==JPEG_BEGIN size=(\d+)==\n(.*?)\n==JPEG_END==', t, re.S)
 *   open('frame.jpg','wb').write(bytes.fromhex(re.sub(r'\s','',m.group(2))))
 *   EOF
 */
static void dump_jpeg_hex(const uint8_t *data, size_t len)
{
	printk("==JPEG_BEGIN size=%zu==\n", len);
	for (size_t i = 0; i < len; i++) {
		printk("%02x", data[i]);
		if ((i & 0x3FU) == 0x3FU) {
			printk("\n");
		}
	}
	printk("\n==JPEG_END==\n");
}

int main(void)
{
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

	while (caps.format_caps[i].pixelformat) {
		const struct video_format_cap *fcap = &caps.format_caps[i];

		if (fcap->pixelformat == VIDEO_PIX_FMT_JPEG) {
			fmt.pixelformat = VIDEO_PIX_FMT_JPEG;
			fmt.width = FRAME_WIDTH;
			fmt.height = FRAME_HEIGHT;
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
		LOG_ERR("Failed to set video format: %d", ret);
		return -1;
	}
	LOG_INF("- format: JPEG %ux%u", fmt.width, fmt.height);

	bsize = MIN((size_t)JPEG_CAPTURE_MAX_BYTES,
		    (size_t)fmt.width * fmt.height);

	buf = video_buffer_aligned_alloc(bsize, CAPTURE_BUF_ALIGN, K_NO_WAIT);
	if (buf == NULL) {
		LOG_ERR("Unable to alloc video buffer");
		return -1;
	}
	LOG_INF("- capture buffer: %zu bytes at 0x%08x",
		bsize, (uint32_t)buf->buffer);

	console_getline_init();

	LOG_INF("Memory-dump capture ready. Press Enter to capture a frame.");

	while (1) {
		/* Wait for the user to trigger a capture (text is ignored). */
		(void)console_getline();

		ret = capture_one_jpeg(video, buf, bsize);
		if (ret) {
			LOG_WRN("Dropped bad capture (%d), press Enter to retry",
				ret);
			continue;
		}

		LOG_INF("JPEG at 0x%08x, size=%u bytes (dump 0x%08x..0x%08x)",
			(uint32_t)buf->buffer, buf->bytesused,
			(uint32_t)buf->buffer,
			(uint32_t)buf->buffer + buf->bytesused);
		LOG_HEXDUMP_INF(&buf->buffer[buf->bytesused - 48], 48,
				"last 48 JPEG bytes");

#ifdef CONFIG_VIDEO_MEMDUMP_HEX_CONSOLE
		dump_jpeg_hex(buf->buffer, buf->bytesused);
#endif
	}

	return 0;
}

static int app_set_parameters(void)
{
	run_profile_t runp = { 0 };
	int ret;

	runp.power_domains  = PD_SYST_MASK | PD_SSE700_AON_MASK | PD_DBSS_MASK;
	runp.dcdc_voltage   = 825;
	runp.dcdc_mode      = DCDC_MODE_PWM;
	runp.aon_clk_src    = CLK_SRC_LFXO;
	runp.run_clk_src    = CLK_SRC_HFRC;
	runp.vdd_ioflex_3V3 = IOFLEX_LEVEL_1V8;
	/* Scaled-HFRC 38.4 MHz. Capture works at the unscaled 76.8 MHz; 38.4 MHz
	 * starves the CPI input FIFO (int_st=0x10 INFIFO overrun) because the
	 * sensor's internal 24 MHz XCLK keeps the pixel clock fixed while the
	 * AXI/DTCM drain runs at half speed. cpu_clk_freq stays at the 76.8
	 * selection while scaled_clk_freq tunes the same physical HFRC down to
	 * 38.4. Keep coherent with CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=38400000
	 * (prj.conf) or the SE rejects the profile. */
	runp.cpu_clk_freq    = CLOCK_FREQUENCY_76_8_RC_MHZ;
	//runp.scaled_clk_freq = SCALED_FREQ_RC_ACTIVE_38_4_MHZ;
    runp.scaled_clk_freq = SCALED_FREQ_RC_ACTIVE_76_8_MHZ;

	runp.memory_blocks = MRAM_MASK;
#if DT_NODE_EXISTS(DT_NODELABEL(sram0))
	runp.memory_blocks |= SRAM0_MASK;
#endif

	/* Camera path only; USB PHY/clock are intentionally left out. */
	runp.phy_pwr_gating  = MIPI_TX_DPHY_MASK | MIPI_RX_DPHY_MASK |
			       MIPI_PLL_DPHY_MASK | LDO_PHY_MASK;
	runp.ip_clock_gating = CAMERA_MASK | MIPI_CSI_MASK | MIPI_DSI_MASK;

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
