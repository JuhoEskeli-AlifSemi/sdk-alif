/*
 * Copyright (C) 2026 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 *
 * OV5640 JPEG capture exposed as a file on USB mass storage.
 *
 * On boot the camera pipeline is initialized, one snapshot is captured,
 * written to a FAT filesystem on a RAM disk and exposed over USB MSC so
 * the host PC sees it as a removable drive containing capture.jpg.
 */

#include <sample_usbd.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/video/ov5640-video-controls.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <soc_common.h>
#include <se_service.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(video_usbout, LOG_LEVEL_INF);

/*
 * JPEG capture buffer size. Keep in sync with CONFIG_VIDEO_BUFFER_POOL_SZ_MAX
 * in the board .conf — that value must be this plus ~1 KB for k_heap block
 * headers / alignment overhead.
 */
#define JPEG_CAPTURE_MAX_BYTES (420U * 1024U)

#define CAPTURE_FILE_PATH "/RAM:/capture.jpg"

USBD_DEFINE_MSC_LUN(ram, "RAM", "Alif", "VideoCapture", "0.01");

static FATFS fat_fs;
static struct fs_mount_t fs_mnt = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/RAM:",
};

/*
 * Scan a captured buffer for the JPEG EOI marker and report the compressed
 * size. Parses past the SOS (Start of Scan) marker first so an EOI inside
 * an EXIF thumbnail is not mistakenly reported. JPEG byte-stuffing
 * guarantees 0xFF 0xD9 cannot appear as a false positive within the
 * entropy-coded data.
 */
static int find_jpeg_size(const uint8_t *bytes, size_t bsize)
{
	int scan_from = 2;
	int pos = 2;

	if (bytes[0] != 0xFF || bytes[1] != 0xD8) {
		LOG_ERR("No JPEG SOI marker (got %02x %02x)",
			bytes[0], bytes[1]);
		return -EIO;
	}

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
			((uint16_t)bytes[pos + 2] << 8) | bytes[pos + 3];
		pos += 2 + seg_len;
	}

	for (int j = scan_from; j < (int)bsize - 1; j++) {
		if (bytes[j] == 0xFF && bytes[j + 1] == 0xD9) {
			return j + 2;
		}
	}

	LOG_ERR("JPEG EOI not found in %zu byte buffer", bsize);
	return -EIO;
}

static int capture_one_jpeg(const struct device *video,
			    struct video_buffer *buf, size_t bsize)
{
	struct video_buffer *vbuf;
	int jpeg_size;
	int ret;

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
	LOG_INF("Capture started");

	/*
	 * The CPI driver stops capture on the 2nd VSYNC (end-of-frame) and
	 * moves the buffer to fifo_out. Block on video_dequeue until that
	 * happens, then scan the buffer for the JPEG EOI marker.
	 */
	ret = video_dequeue(video, VIDEO_EP_OUT, &vbuf, K_FOREVER);
	if (ret) {
		LOG_ERR("Unable to dequeue video buf: %d", ret);
		return ret;
	}
	LOG_INF("Frame captured, scanning for JPEG EOI...");

	jpeg_size = find_jpeg_size(vbuf->buffer, bsize);
	if (jpeg_size < 0) {
		return jpeg_size;
	}

	vbuf->bytesused = jpeg_size;
	LOG_INF("JPEG EOI at offset %d (%d KB), timestamp %u ms",
		jpeg_size, jpeg_size / 1024, vbuf->timestamp);

	video_flush(video, VIDEO_EP_OUT, false);
	ret = video_stream_stop(video);
	if (ret) {
		LOG_ERR("Unable to stop capture: %d", ret);
	}
	return 0;
}

static int write_capture_to_file(const struct video_buffer *vbuf)
{
	struct fs_file_t file;
	ssize_t written;
	int ret;

	fs_file_t_init(&file);
	ret = fs_open(&file, CAPTURE_FILE_PATH, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		LOG_ERR("Failed to open %s: %d", CAPTURE_FILE_PATH, ret);
		return ret;
	}

	written = fs_write(&file, vbuf->buffer, vbuf->bytesused);
	if (written < 0) {
		LOG_ERR("Failed to write %s: %d",
			CAPTURE_FILE_PATH, (int)written);
		fs_close(&file);
		return (int)written;
	}

	fs_close(&file);

	if ((size_t)written < vbuf->bytesused) {
		LOG_ERR("Short write to %s: %d of %u bytes (disk full?)",
			CAPTURE_FILE_PATH, (int)written, vbuf->bytesused);
		return -ENOSPC;
	}

	LOG_INF("Wrote %d bytes to %s", (int)written, CAPTURE_FILE_PATH);
	return 0;
}

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

	{
		uint8_t chip_rev = 0;
		int rev_ret = video_get_ctrl(video, VIDEO_OV5640_CID_CHIP_REVISION,
					     &chip_rev);

		if (rev_ret) {
			LOG_WRN("Unable to read OV5640 chip revision: %d", rev_ret);
		} else {
			const char *proc;

			switch (chip_rev >> 4) {
			case 0xA:
				proc = "FSI";
				break;
			case 0xB:
				proc = "BSI";
				break;
			default:
				proc = "unknown";
				break;
			}
			LOG_INF("- OV5640 reg 0x302A = 0x%02x "
				"(process %s, revision %u)",
				chip_rev, proc, chip_rev & 0x0F);
		}
	}

	if (video_get_caps(video, VIDEO_EP_OUT, &caps)) {
		LOG_ERR("Unable to retrieve video capabilities");
		return -1;
	}

	while (caps.format_caps[i].pixelformat) {
		const struct video_format_cap *fcap = &caps.format_caps[i];

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
		LOG_ERR("Failed to set video format: %d", ret);
		return -1;
	}
	LOG_INF("- format: JPEG %ux%u", fmt.width, fmt.height);

	bsize = MIN((size_t)JPEG_CAPTURE_MAX_BYTES,
		    (size_t)fmt.width * fmt.height);

	buf = video_buffer_alloc(bsize, K_NO_WAIT);
	if (buf == NULL) {
		LOG_ERR("Unable to alloc video buffer");
		return -1;
	}
	LOG_INF("- capture buffer: %zu bytes at 0x%08x",
		bsize, (uint32_t)buf->buffer);

	ret = fs_mount(&fs_mnt);
	if (ret) {
		LOG_ERR("Failed to mount FAT filesystem: %d", ret);
		return -1;
	}
	LOG_INF("FAT filesystem mounted on %s", fs_mnt.mnt_point);

	ret = capture_one_jpeg(video, buf, bsize);
	if (ret) {
		return ret;
	}

	ret = write_capture_to_file(buf);
	if (ret) {
		return ret;
	}

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
	LOG_INF("USB mass storage enabled — %s available on host.",
		CAPTURE_FILE_PATH);

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

	gpio_pin_configure_dt(&cam_enbuf, GPIO_OUTPUT_ACTIVE);

	return 0;
}

SYS_INIT(app_set_parameters, PRE_KERNEL_1, 46);
