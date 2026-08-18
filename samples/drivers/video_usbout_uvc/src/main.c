/*
 * Copyright (C) 2026 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 *
 * OV5640 JPEG capture exposed as a USB Video Class (UVC) webcam.
 *
 * On boot the camera pipeline is initialized and the board enumerates as a
 * standard MJPEG webcam. Whenever the host opens the stream the sample
 * captures fresh JPEG frames from the sensor and pushes them out the UVC bulk
 * endpoint, so the host can grab as many frames as it likes without resetting
 * the board (unlike the previous USB mass-storage snapshot approach).
 */

#include <sample_usbd.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/cache.h>
#include <soc_common.h>
#include <se_service.h>
#include <string.h>
#include <zephyr/console/console.h>

#include "uvc.h"

#ifdef CONFIG_VIDEO_USBOUT_INTERACTIVE_CONFIG
#include "cam_config.h"
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(video_usbout, LOG_LEVEL_INF);

/*
 * JPEG capture buffer size, maximised for this board. The buffer is carved
 * from the video pool in the NON_SECURE0 region. This value is bounded by
 * CONFIG_VIDEO_BUFFER_POOL_SZ_MAX (board .conf), which must be this plus
 * ~1.5 KB for k_heap block headers / alignment overhead. Keep both in sync,
 * and in sync with UVC_MAX_FRAME_SIZE (uvc.h). See the .overlay for the DTCM
 * memory budget.
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
 * the neighbouring USB buffers / heap metadata in the NON_SECURE0 region.
 */
#define CAPTURE_BUF_ALIGN 128U

/*
 * Diagnostic: capture a single frame, then stream that same static buffer to
 * the host over and over without re-capturing. If the host still sees
 * intermittent corruption with this set, the fault is in the USB/UVC path;
 * if the static frame is always clean, the per-frame capture is to blame.
 * Set to 0 for normal live capture.
 */
#define UVC_DEBUG_STATIC_FRAME 0

/*
 * Diagnostic: stream the last captured frame to the host continuously and only
 * re-capture when the user asks for it (press Enter on the console). Because
 * the streamed bytes are frozen between manual captures, this pins down where
 * corruption enters: if the host keeps seeing corruption while nothing is being
 * re-captured, it is in the USB/UVC path (or the host); if only freshly
 * captured frames are corrupt, it is in the capture path. Mutually exclusive
 * with UVC_DEBUG_STATIC_FRAME; set to 0 for normal live capture.
 */
#define UVC_DEBUG_MANUAL_CAPTURE 0

/*
 * Upper bound on how long a single snapshot capture may take. A real capture
 * completes within a few sensor frame periods (a couple hundred ms); the
 * generous 2 s ceiling only trips when the CPI driver dropped the frame as
 * corrupt (a FIFO overrun latched in the same interrupt as end-of-frame) and
 * therefore never delivered a buffer. Without it, video_dequeue(K_FOREVER)
 * would block forever on such a frame and the stream would stall.
 */
#define CAPTURE_TIMEOUT K_MSEC(2000)

/*
 * OV5640 JPEG VFIFO overflow status register. A non-zero value means the
 * sensor's internal JPEG FIFO overflowed while emitting the frame: it then
 * drops entropy-coded bytes mid/late in the image yet still appends a valid
 * EOI marker. The result is a JPEG that decodes correctly until the bottom
 * rows run out of bits (mjpeg_decode_dc: bad vlc / overread at the last MCU
 * row). find_jpeg_size() cannot catch this because the EOI is genuine, so the
 * register is logged per frame to confirm whether the corruption tracks sensor
 * JFIFO overflow (in which case the fix is sensor-side: more JPEG compression
 * or a faster pixel clock to drain the FIFO).
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
 * The capture buffer is zero-filled before every capture (see
 * capture_one_jpeg) and the sensor is configured to emit no dummy padding
 * after the image, so the populated region of the buffer is exactly one JPEG
 * ending in the EOI marker (0xFF 0xD9) followed by zero padding. The true end
 * of frame is therefore the last non-zero byte, which for a complete JPEG is
 * the second EOI byte.
 *
 * Scanning backwards from the end of the buffer (rather than forwards for the
 * first 0xFF 0xD9 after the Start-Of-Scan) is deliberate: a single bus or
 * sensor-FIFO glitch can flip a byte-stuffed 0xFF 0x00 pair into 0xFF 0xD9
 * inside the entropy-coded data. A forward "first EOI" scan latches onto that
 * false marker and reports a size short of the real frame, so the host gets a
 * JPEG whose last rows are missing and the decoder overreads at the bottom of
 * the image (mjpeg_decode_dc: bad vlc / overread, error y=<last MCU row>).
 * Anchoring on the real end of data avoids that, and an EOI inside an EXIF
 * thumbnail (which precedes the main image) is likewise never mistaken for the
 * end.
 *
 * Returns the JPEG length on success, or -EIO if the capture does not start
 * with SOI or does not end in EOI (truncated / overflowed / garbled frame), in
 * which case the caller drops it and re-captures.
 */
static int find_jpeg_size(const uint8_t *bytes, size_t bsize)
{
	size_t end = bsize;

	if (bsize < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) {
		LOG_ERR("No JPEG SOI marker (got %02x %02x)",
			bytes[0], bytes[1]);
		return -EIO;
	}

	/* Walk back over the zero padding to the last populated byte. */
	while (end > 0 && bytes[end - 1] == 0x00) {
		end--;
	}

	/*
	 * A complete frame ends in EOI. If the last populated byte is not the
	 * second EOI byte, the capture was truncated (the CPI dropped the tail
	 * burst, so the sensor's EOI never landed) and is unusable.
	 */
	if (end < 4 || bytes[end - 1] != 0xD9 || bytes[end - 2] != 0xFF) {
		LOG_ERR("JPEG does not end in EOI (truncated/garbled capture)");
		return -EIO;
	}

	/*
	 * No zero padding at all means the JPEG was at least as large as the
	 * capture buffer, i.e. it overran JPEG_CAPTURE_MAX_BYTES (and the bytes
	 * past the end of the buffer were written into neighbouring memory).
	 * Reject it: such a frame is corrupt and must not be streamed.
	 */
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
 * snapshot mode (one frame per stream_start/stop), so each frame re-arms the
 * pipeline. On success buf->bytesused holds the compressed JPEG size.
 */
static int capture_one_jpeg(const struct device *video,
			    struct video_buffer *buf, size_t bsize)
{
	struct video_buffer *vbuf;
	int jpeg_size;
	int ret;

	/*
	 * Clear the buffer before every capture. The CPI driver stops on the
	 * 2nd VSYNC and occasionally drops the final AXI burst, truncating the
	 * JPEG tail. Since the buffer is reused frame to frame, a stale 0xFF
	 * 0xD9 from a previous capture would otherwise sit past the truncation
	 * point and fool find_jpeg_size() into reporting a false end. Zeroing
	 * first guarantees a truncated frame has no EOI, so it is cleanly
	 * rejected below and re-captured rather than streamed corrupted.
	 * (The driver flushes this buffer to RAM on enqueue, so the zeros land
	 * before the camera DMA overwrites them.)
	 */
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

	/*
	 * The CPI driver stops capture on the 2nd VSYNC (end-of-frame) and
	 * moves the buffer to fifo_out. Block on video_dequeue until that
	 * happens, then scan the buffer for the JPEG EOI marker.
	 */
	ret = video_dequeue(video, VIDEO_EP_OUT, &vbuf, CAPTURE_TIMEOUT);
	if (ret == -EAGAIN) {
		/*
		 * No buffer arrived within CAPTURE_TIMEOUT. The CPI driver drops
		 * frames it flags as corrupt (a FIFO overrun latched in the same
		 * interrupt as end-of-frame) instead of delivering them, so the
		 * buffer is still sitting in the driver's IN-FIFO. Stop the
		 * stream and reclaim it: stream_stop zeroes curr_vid_buf so the
		 * flush below can move the buffer to the OUT-FIFO, and draining it
		 * leaves the caller free to re-enqueue the same buffer and retry.
		 */
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

	/*
	 * The CPI driver only does cache maintenance on enqueue (before DMA),
	 * not after the frame lands, so invalidate here to make sure the CPU
	 * reads the freshly DMA'd JPEG rather than stale cache lines.
	 */
	sys_cache_data_invd_range(vbuf->buffer, bsize);

	/*
	 * Log the sensor's JPEG FIFO overflow status for this frame. If this
	 * fires in step with the host-side tail corruption, the sensor (not the
	 * USB path or find_jpeg_size) is dropping entropy bytes and the fix
	 * belongs on the sensor side (higher JPEG compression / faster PCLK).
	 */
	log_jfifo_overflow();

	jpeg_size = find_jpeg_size(vbuf->buffer, bsize);
	if (jpeg_size < 0 || jpeg_size < (int)JPEG_MIN_VALID_BYTES) {
		/* Truncated/garbled capture: drop it, the caller re-captures. */
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

#if UVC_DEBUG_MANUAL_CAPTURE
/*
 * Manual-capture trigger. A tiny background thread blocks on console line
 * input; each Enter sets manual_capture_request, which the streaming loop in
 * main() polls between sends and consumes by grabbing exactly one fresh frame.
 * The thread touches neither the capture buffer nor the video device, so
 * capture and USB transmit stay serialised in the main thread with no buffer
 * race against the repeat-send.
 */
#define MANUAL_KEY_STACK_SIZE 1536
#define MANUAL_KEY_PRIORITY   7

static K_THREAD_STACK_DEFINE(manual_key_stack, MANUAL_KEY_STACK_SIZE);
static struct k_thread manual_key_thread;
static volatile bool manual_capture_request;

static void manual_key_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

#ifndef CONFIG_VIDEO_USBOUT_INTERACTIVE_CONFIG
	/*
	 * The interactive register menu already initialises console line input
	 * before this thread is started; only initialise it here when that menu
	 * is compiled out.
	 */
	console_getline_init();
#endif

	while (1) {
		/* Blocks until the user presses Enter; the text is ignored. */
		(void)console_getline();
		manual_capture_request = true;
	}
}
#endif /* UVC_DEBUG_MANUAL_CAPTURE */

int main(void)
{
	struct video_format fmt = { 0 };
	struct video_caps caps;
	const struct device *video;
	struct video_buffer *buf;
	struct usbd_context *sample_usbd;
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
			fmt.width = UVC_FRAME_WIDTH;
			fmt.height = UVC_FRAME_HEIGHT;
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

#ifdef CONFIG_VIDEO_USBOUT_INTERACTIVE_CONFIG
	cam_interactive_config();
#endif

	sample_usbd = sample_usbd_init_device(NULL);
	if (sample_usbd == NULL) {
		LOG_ERR("Failed to initialize USB device");
		return -1;
	}

	ret = usbd_enable(sample_usbd);
	if (ret) {
		LOG_ERR("Failed to enable USB: %d", ret);
		return -1;
	}
	LOG_INF("UVC webcam enabled — waiting for host to open the stream.");

#if UVC_DEBUG_STATIC_FRAME
	/* Capture one frame up front and stream it repeatedly (see above). */
	do {
		ret = capture_one_jpeg(video, buf, bsize);
	} while (ret);
	LOG_INF("DEBUG: streaming static %u-byte frame repeatedly",
		buf->bytesused);

	while (1) {
		ret = uvc_wait_for_stream(-1);
		if (ret) {
			continue;
		}

		ret = uvc_send_frame(buf->buffer, buf->bytesused);
		if (ret) {
			LOG_INF("Frame send aborted (%d), stream likely closed",
				ret);
		}
	}
#elif UVC_DEBUG_MANUAL_CAPTURE
	/*
	 * Manual-capture diagnostic: grab one frame up front, then stream that
	 * same buffer to the host over and over. The background key thread sets
	 * manual_capture_request when the user presses Enter; the loop consumes
	 * it by capturing exactly one fresh frame into the same buffer before
	 * resuming the repeat-send. Between captures the streamed bytes never
	 * change, so corruption the host still sees is downstream of capture
	 * (USB/UVC path or host), while corruption that appears only on freshly
	 * captured frames points back at the capture path.
	 */
	do {
		ret = capture_one_jpeg(video, buf, bsize);
	} while (ret);
	LOG_INF("Initial capture: JPEG at 0x%08x, size=%u bytes "
		"(dump 0x%08x..0x%08x)",
		(uint32_t)buf->buffer, buf->bytesused,
		(uint32_t)buf->buffer,
		(uint32_t)buf->buffer + buf->bytesused);
	LOG_HEXDUMP_INF(&buf->buffer[buf->bytesused - 48], 48,
			"last 48 JPEG bytes");

	k_thread_create(&manual_key_thread, manual_key_stack,
			K_THREAD_STACK_SIZEOF(manual_key_stack),
			manual_key_entry, NULL, NULL, NULL,
			MANUAL_KEY_PRIORITY, 0, K_NO_WAIT);

	LOG_INF("DEBUG: manual-capture mode — streaming the last frame; "
		"press Enter on the console to capture a fresh one.");

	while (1) {
		ret = uvc_wait_for_stream(-1);
		if (ret) {
			continue;
		}

		if (manual_capture_request) {
			manual_capture_request = false;

			ret = capture_one_jpeg(video, buf, bsize);
			if (ret) {
				LOG_WRN("Manual capture failed (%d); keeping "
					"the previous frame", ret);
			} else {
				LOG_INF("Manual capture: JPEG at 0x%08x, "
					"size=%u bytes (dump 0x%08x..0x%08x)",
					(uint32_t)buf->buffer,
					buf->bytesused,
					(uint32_t)buf->buffer,
					(uint32_t)buf->buffer +
						buf->bytesused);
				LOG_HEXDUMP_INF(
					&buf->buffer[buf->bytesused - 48],
					48, "last 48 JPEG bytes");
			}
		}

		ret = uvc_send_frame(buf->buffer, buf->bytesused);
		if (ret) {
			LOG_INF("Frame send aborted (%d), stream likely closed",
				ret);
		}
	}
#else
	while (1) {
		/* Block until the host commits/opens the video stream. */
		ret = uvc_wait_for_stream(-1);
		if (ret) {
			continue;
		}

		ret = capture_one_jpeg(video, buf, bsize);
		if (ret) {
			/* Truncated/garbled frame was dropped; re-capture. */
			LOG_WRN("Dropped bad capture (%d), retrying", ret);
			continue;
		}

		/*
		 * DEBUG: dump the JPEG tail as it sits in the capture buffer,
		 * before any USB transfer, so it can be compared byte-for-byte
		 * against the frame the host receives. The last two bytes must
		 * be FF D9 (EOI). If this tail is already corrupt, the fault is
		 * in capture; if it is clean but the host's frame is not, the
		 * fault is in the USB path.
		 */
		LOG_INF("JPEG at 0x%08x, size=%u bytes (dump 0x%08x..0x%08x)",
			(uint32_t)buf->buffer, buf->bytesused,
			(uint32_t)buf->buffer,
			(uint32_t)buf->buffer + buf->bytesused);
		LOG_HEXDUMP_INF(&buf->buffer[buf->bytesused - 48], 48,
				"last 48 JPEG bytes");

		ret = uvc_send_frame(buf->buffer, buf->bytesused);
		if (ret) {
			LOG_INF("Frame send aborted (%d), stream likely closed",
				ret);
		}
	}
#endif

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
		//GPIO_DT_SPEC_GET(DT_NODELABEL(cam_enbuf), enable_gpios); for rebased version

	gpio_pin_configure_dt(&cam_enbuf, GPIO_OUTPUT_ACTIVE);

	return 0;
}

SYS_INIT(app_set_parameters, PRE_KERNEL_1, 46);
