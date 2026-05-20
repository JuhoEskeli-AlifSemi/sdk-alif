/*
 * Copyright (C) 2026 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Video capture to USB mass storage sample.
 *
 * Captures a frame from the camera and writes it to a FAT filesystem
 * on a RAM disk exposed as USB mass storage. The host PC can then
 * read the captured image file directly without needing a debugger.
 */

#include <sample_usbd.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/cache.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <soc_common.h>
#include <se_service.h>

#include <zephyr/drivers/video/video_alif.h>

#ifdef CONFIG_DT_HAS_HIMAX_HM0360_ENABLED
#include <zephyr/drivers/video/hm0360-video-controls.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(video_usbout, LOG_LEVEL_INF);

#define N_VID_BUFF 1

#define ISP_ENABLED DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(isp))

#ifdef CONFIG_DT_HAS_HIMAX_HM0360_ENABLED
#define PIPELINE_FORMAT	VIDEO_PIX_FMT_BGGR8
#elif CONFIG_DT_HAS_OVTI_OV5640_ENABLED
#define PIPELINE_FORMAT	VIDEO_PIX_FMT_JPEG
#else
#define PIPELINE_FORMAT	VIDEO_PIX_FMT_Y10P
#endif

#if ISP_ENABLED
#define OUTPUT_FORMAT VIDEO_PIX_FMT_RGB888_PLANAR_PRIVATE
#endif

/* Define USB MSC LUN for the RAM disk */
USBD_DEFINE_MSC_LUN(ram, "RAM", "Alif", "VideoCapture", "0.01");

static FATFS fat_fs;
static struct fs_mount_t fs_mnt = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/RAM:",
};

static int fourcc_to_pitch(uint32_t fourcc, uint32_t width)
{
	switch (fourcc) {
	case VIDEO_PIX_FMT_RGB888_PLANAR_PRIVATE:
	case VIDEO_PIX_FMT_NV24:
	case VIDEO_PIX_FMT_NV42:
		return width * 3;
	case VIDEO_PIX_FMT_RGB565:
	case VIDEO_PIX_FMT_Y10P:
	case VIDEO_PIX_FMT_BGGR10:
	case VIDEO_PIX_FMT_GBRG10:
	case VIDEO_PIX_FMT_GRBG10:
	case VIDEO_PIX_FMT_RGGB10:
	case VIDEO_PIX_FMT_YUYV:
	case VIDEO_PIX_FMT_YVYU:
	case VIDEO_PIX_FMT_VYUY:
	case VIDEO_PIX_FMT_UYVY:
	case VIDEO_PIX_FMT_NV16:
	case VIDEO_PIX_FMT_NV61:
	case VIDEO_PIX_FMT_YUV422P:
		return width << 1;
	case VIDEO_PIX_FMT_NV12:
	case VIDEO_PIX_FMT_NV21:
	case VIDEO_PIX_FMT_YUV420:
	case VIDEO_PIX_FMT_YVU420:
		return (width * 3) >> 1;
	case VIDEO_PIX_FMT_JPEG:
		/* For JPEG, use full uncompressed size as max buffer */
		return width * 2;
	case VIDEO_PIX_FMT_BGGR8:
	case VIDEO_PIX_FMT_GBRG8:
	case VIDEO_PIX_FMT_GRBG8:
	case VIDEO_PIX_FMT_RGGB8:
	case VIDEO_PIX_FMT_GREY:
	default:
		return width;
	}
}

static const char *fourcc_str(uint32_t fourcc, char buf[5])
{
	buf[0] = (char)(fourcc);
	buf[1] = (char)(fourcc >> 8);
	buf[2] = (char)(fourcc >> 16);
	buf[3] = (char)(fourcc >> 24);
	buf[4] = '\0';
	return buf;
}

static int write_capture_to_file(struct video_buffer *vbuf, int index,
				 const char *ext)
{
	struct fs_file_t file;
	char path[32];
	ssize_t written;
	int ret;

	snprintf(path, sizeof(path), "/RAM:/cap_%d.%s", index, ext);

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		LOG_ERR("Failed to open %s: %d", path, ret);
		return ret;
	}

	written = fs_write(&file, vbuf->buffer, vbuf->bytesused);
	if (written < 0) {
		LOG_ERR("Failed to write %s: %d", path, (int)written);
		fs_close(&file);
		return (int)written;
	}

	fs_close(&file);
	LOG_INF("Wrote %d bytes to %s", (int)written, path);
	return 0;
}

int main(void)
{
	struct video_buffer *buffers[N_VID_BUFF], *vbuf;
	struct video_format fmt = { 0 };
	struct video_caps caps;
	const struct device *video;
	enum video_endpoint_id ep;
	char fcc[5];
	size_t bsize;
	int i;
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

	if (IS_ENABLED(ISP_ENABLED)) {
		ep = VIDEO_EP_IN;
	} else {
		ep = VIDEO_EP_OUT;
	}

	/* Get capabilities and select format */
	if (video_get_caps(video, ep, &caps)) {
		LOG_ERR("Unable to retrieve video capabilities");
		return -1;
	}

	i = 0;
	while (caps.format_caps[i].pixelformat) {
		const struct video_format_cap *fcap = &caps.format_caps[i];

		LOG_INF("  %s %ux%u - %ux%u",
			fourcc_str(fcap->pixelformat, fcc),
			fcap->width_min, fcap->height_min,
			fcap->width_max, fcap->height_max);

		if (fcap->pixelformat == PIPELINE_FORMAT) {
			fmt.pixelformat = PIPELINE_FORMAT;
			if (IS_ENABLED(CONFIG_DT_HAS_HIMAX_HM0360_ENABLED)) {
				fmt.width = 320;
				fmt.height = 240;
			} else if (IS_ENABLED(CONFIG_DT_HAS_OVTI_OV5640_ENABLED)) {
				fmt.width = 160;
				fmt.height = 120;
			} else {
				fmt.width = fcap->width_min;
				fmt.height = fcap->height_min;
			}
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

#if ISP_ENABLED
	fmt.pixelformat = OUTPUT_FORMAT;
	fmt.width = 480;
	fmt.height = 480;
	fmt.pitch = fourcc_to_pitch(fmt.pixelformat, fmt.width);

	ret = video_set_format(video, VIDEO_EP_OUT, &fmt);
	if (ret) {
		LOG_ERR("Failed to set ISP output format: %d", ret);
		return -1;
	}
#endif

	bsize = fmt.pitch * fmt.height;
	LOG_INF("Format: %s %ux%u, pitch %u, buffer %u bytes",
		fourcc_str(fmt.pixelformat, fcc),
		fmt.width, fmt.height, fmt.pitch, bsize);

	/* Allocate video buffers and enqueue */
	for (i = 0; i < ARRAY_SIZE(buffers); i++) {
		buffers[i] = video_buffer_aligned_alloc(bsize, 8, K_NO_WAIT);
		if (buffers[i] == NULL) {
			LOG_ERR("Unable to alloc video buffer");
			return -1;
		}
		memset(buffers[i]->buffer, 0, bsize);
		video_enqueue(video, VIDEO_EP_OUT, buffers[i]);
	}

	/*
	 * Delay needed for some sensors (e.g. mt9m114) to stabilize
	 * after configuration.
	 */
	k_msleep(7000);

#ifdef CONFIG_DT_HAS_HIMAX_HM0360_ENABLED
	uint32_t num_frames = 1;

	ret = video_set_ctrl(video, VIDEO_CID_SNAPSHOT_CAPTURE, &num_frames);
	if (ret) {
		LOG_INF("Snapshot mode not supported");
	}
#endif

	/* Start capture */
	ret = video_stream_start(video);
	if (ret) {
		LOG_ERR("Unable to start capture: %d", ret);
		return -1;
	}
	LOG_INF("Capture started, waiting for frame...");

#ifdef CONFIG_DT_HAS_OVTI_OV5640_ENABLED
	/*
	 * JPEG capture: CPI is configured for worst-case frame size.
	 * Poll the buffer for JPEG EOI marker (0xFF 0xD9), then
	 * stop the CPI via flush+stop. The JPEG compressed size is
	 * determined by the EOI position.
	 */
	{
		uint8_t *buf = buffers[0]->buffer;
		int jpeg_size = -1;
		int poll_count = 0;
		const int max_polls = 500; /* 5 seconds max */

		LOG_INF("Polling buffer for JPEG EOI marker...");

		while (jpeg_size < 0 && poll_count < max_polls) {
			k_msleep(10);
			poll_count++;

			/* Invalidate cache to see DMA-written data */
			sys_cache_data_invd_range(buf, bsize);

			if (poll_count % 50 == 0) {
				LOG_INF("Poll %d: first bytes: %02x %02x %02x %02x",
					poll_count, buf[0], buf[1], buf[2], buf[3]);
			}

			/* Scan for EOI marker (0xFF 0xD9) */
			for (int j = 0; j < (int)bsize - 1; j++) {
				if (buf[j] == 0xFF && buf[j + 1] == 0xD9) {
					jpeg_size = j + 2;
					break;
				}
			}
		}

		/* Stop CPI: flush cancel moves buffer to out-fifo, then stop */
		LOG_INF("Stopping CPI capture...");
		video_flush(video, VIDEO_EP_OUT, true);
		video_stream_stop(video);

		if (jpeg_size > 0) {
			LOG_INF("JPEG EOI found at offset %d", jpeg_size);
			buffers[0]->bytesused = jpeg_size;
			vbuf = buffers[0];
		} else {
			LOG_ERR("JPEG EOI not found after %d polls", poll_count);
			LOG_INF("First 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x "
				"%02x %02x %02x %02x %02x %02x %02x %02x",
				buf[0], buf[1], buf[2], buf[3],
				buf[4], buf[5], buf[6], buf[7],
				buf[8], buf[9], buf[10], buf[11],
				buf[12], buf[13], buf[14], buf[15]);
			return -1;
		}
	}
#else
	/* Dequeue one frame */
	ret = video_dequeue(video, VIDEO_EP_OUT, &vbuf, K_FOREVER);
	if (ret) {
		LOG_ERR("Unable to dequeue video buf: %d", ret);
		return -1;
	}
	LOG_INF("Got frame! size: %u bytes, timestamp: %u ms",
		vbuf->bytesused, vbuf->timestamp);

	/* Stop capture */
	video_flush(video, VIDEO_EP_OUT, false);
	ret = video_stream_stop(video);
	if (ret) {
		LOG_ERR("Unable to stop capture: %d", ret);
	}
#endif

	/* Mount FAT filesystem on RAM disk */
	ret = fs_mount(&fs_mnt);
	if (ret) {
		LOG_ERR("Failed to mount FAT filesystem: %d", ret);
		return -1;
	}
	LOG_INF("FAT filesystem mounted on %s", fs_mnt.mnt_point);

	/* Write captured frame to file */
	const char *file_ext = (fmt.pixelformat == VIDEO_PIX_FMT_JPEG) ? "jpg" : "bin";

	ret = write_capture_to_file(vbuf, 0, file_ext);
	if (ret) {
		LOG_ERR("Failed to write capture file");
		return -1;
	}

	/* Enable USB mass storage - host will see the RAM disk */
	struct usbd_context *sample_usbd;

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

	LOG_INF("USB mass storage enabled. Connect USB to read captured image.");
	LOG_INF("The file 'cap_0.%s' contains the %s %ux%u image.",
		file_ext, fourcc_str(fmt.pixelformat, fcc), fmt.width, fmt.height);

	return 0;
}

static int app_set_parameters(void)
{
	run_profile_t runp = { 0 };
	int ret;

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

	runp.phy_pwr_gating |= MIPI_TX_DPHY_MASK | MIPI_RX_DPHY_MASK |
		MIPI_PLL_DPHY_MASK | LDO_PHY_MASK | USB_PHY_MASK;
	runp.ip_clock_gating = CAMERA_MASK | MIPI_CSI_MASK | MIPI_DSI_MASK | USB_MASK;

	ret = se_service_set_run_cfg(&runp);
	__ASSERT(ret == 0, "SE: set_run_cfg failed = %d", ret);

#if (DT_NODE_HAS_STATUS(DT_NODELABEL(lpcam), okay))

	sys_write32(0x080001, M55HE_CFG_HE_CAMERA_PIXCLK);

#if CONFIG_DT_HAS_OVTI_OV5640_ENABLED
	const struct gpio_dt_spec cam_enbuf =
		GPIO_DT_SPEC_GET(DT_NODELABEL(cam_enbuf), enbuf_gpios);

	gpio_pin_configure_dt(&cam_enbuf, GPIO_OUTPUT_ACTIVE);
#endif
#endif
	return 0;
}

SYS_INIT(app_set_parameters, PRE_KERNEL_1, 46);
