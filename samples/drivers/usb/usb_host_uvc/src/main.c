/*
 * Copyright (c) 2026 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB Host UVC Capture Sample
 *
 * Brings up the DWC3 controller in host mode and captures video from a USB
 * Video Class (UVC) device that streams over a bulk endpoint, such as an
 * Alif board running the video_uvc sample, or over an isochronous one, such
 * as a webcam:
 *
 *   1. Wait for a device to be connected and enumerate it
 *   2. Parse the configuration descriptor for the VideoStreaming interface,
 *      its formats and frame sizes, and its alternate settings
 *   3. SET_CONFIGURATION and, for bulk, configure the bulk IN endpoint
 *   4. Negotiate a format and frame size with Probe/Commit
 *   5. Isochronous: select the alternate setting that carries the
 *      negotiated payload size and configure its endpoint
 *   6. Keep transfers queued, reassemble frames from the UVC payloads,
 *      validate each frame and log statistics once per second
 *   7. With display.overlay and display.conf, show each valid frame on the
 *      DK display (src/display.c) from a thread of its own
 *
 * Build:
 *   west build -b alif_e8_dk/ae822fa0e5597xx0/rtss_hp \
 *     alif/samples/drivers/usb/usb_host_uvc \
 *     -- -DDTC_OVERLAY_FILE=boards/alif_usb_host.overlay
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbh.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/drivers/usb/uhc_dwc3.h>
#include <zephyr/sys/byteorder.h>

#include "display.h"
#include "uvc.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usb_host_uvc, LOG_LEVEL_INF);

#define UHC_NODE DT_NODELABEL(zephyr_uhc0)

#if !DT_NODE_EXISTS(UHC_NODE)
#error "zephyr_uhc0 node not found. Check overlay."
#endif

static const struct device *uhc_dev = DEVICE_DT_GET(UHC_NODE);

USBH_CONTROLLER_DEFINE(sample_uhs_ctx, DEVICE_DT_GET(UHC_NODE));

/* Bulk IN transfers kept in flight, each receiving into its own chunk */
#define NUM_CHUNKS	4
#define CHUNK_SIZE	16384

/*
 * Isochronous IN transfers kept in flight, one per service interval. On
 * High Speed the 32 cover 4 ms. Each buffer holds the most a High-Speed
 * endpoint carries per microframe (3 x 1024 bytes) and is aligned so it
 * never crosses a 64 KiB boundary.
 */
#define NUM_ISO_XFERS	32
#define ISO_BUF_SIZE	4096

/* Wait for a completion before checking that the device is still there */
#define REAP_TIMEOUT_MS	1000

/* A second frame buffer lets the display show one frame while the next is
 * received.
 */
#define NUM_FRAME_BUFS	(IS_ENABLED(CONFIG_DISPLAY) ? 2 : 1)

/*
 * The USB DMA cannot reach TCM, so the chunks live in SRAM0 (placed by
 * uhc_dma.ld). Aligning each chunk to its size keeps it inside one 64 KiB
 * window, as the controller requires. The frame buffer is only written by
 * the CPU but is too large for TCM.
 */
#define SRAM0_SECTION __attribute__((section(".sram0.bss.uhc_dma.uvc")))

static uint8_t chunks[NUM_CHUNKS][CHUNK_SIZE] __aligned(CHUNK_SIZE)
	SRAM0_SECTION;
static uint8_t iso_bufs[NUM_ISO_XFERS][ISO_BUF_SIZE] __aligned(ISO_BUF_SIZE)
	SRAM0_SECTION;
static uint8_t frame_bufs[NUM_FRAME_BUFS][CONFIG_SAMPLE_UVC_FRAME_BUFFER_SIZE]
	__aligned(64) SRAM0_SECTION;
static int frame_buf_idx;

static uint8_t cfg_desc[UHC_DWC3_CTRL_MAX_LEN];
static struct uvc_device uvc;
static struct uvc_stream stream;

/* What the negotiated format lets us check about each frame */
static struct {
	bool mjpeg;
	size_t expected_size;	/* Uncompressed frames: exact size, else 0 */
} check;

static struct {
	uint32_t frames;
	uint32_t bad_frames;
	uint64_t bytes;
	uint32_t timeouts;
	uint32_t lost;		/* Isochronous transfers that lost their data */
	/* Totals across the window, logged once per second */
	uint32_t win_frames;
	uint64_t win_bytes;
	uint32_t win_start;
} stats;

#if CONFIG_SAMPLE_UVC_TRACE_XFERS > 0
static struct {
	uint16_t len;
	uint8_t head[4];
} trace[CONFIG_SAMPLE_UVC_TRACE_XFERS];
static int trace_count;

/* Record a transfer; log them all once the trace is full */
static void trace_xfer(const uint8_t *buf, size_t len, size_t xfer_len)
{
	uint32_t offset = 0;

	if (trace_count >= ARRAY_SIZE(trace)) {
		return;
	}

	trace[trace_count].len = len;
	memcpy(trace[trace_count].head, buf, MIN(len, 4));
	if (++trace_count < ARRAY_SIZE(trace)) {
		return;
	}

	/* Offset is counted from the end of the last short transfer */
	for (int i = 0; i < trace_count; i++) {
		const uint8_t *h = trace[i].head;

		LOG_INF("xfer %3d: off %6u len %5u%s %02x %02x %02x %02x", i,
			offset, trace[i].len,
			trace[i].len < xfer_len ? " short" : "      ",
			h[0], h[1], h[2], h[3]);
		offset = (trace[i].len < xfer_len) ? 0 : offset + trace[i].len;
	}
}
#else
static inline void trace_xfer(const uint8_t *buf, size_t len, size_t xfer_len)
{
}
#endif

static bool port_connected(void)
{
	volatile uint32_t *portsc =
		(volatile uint32_t *)(DT_REG_ADDR(UHC_NODE) + 0x420);

	return (*portsc & BIT(0)) != 0;
}

static int usb_host_event_cb(const struct device *dev,
			     const struct uhc_event *const event)
{
	switch (event->type) {
	case UHC_EVT_DEV_CONNECTED_FS:
		LOG_INF(">>> USB device connected (Full Speed)");
		break;
	case UHC_EVT_DEV_CONNECTED_HS:
		LOG_INF(">>> USB device connected (High Speed)");
		break;
	case UHC_EVT_DEV_REMOVED:
		LOG_INF(">>> USB device disconnected");
		break;
	default:
		break;
	}

	return 0;
}

#if defined(CONFIG_DISPLAY)
/*
 * Frames are drawn by a thread of their own, so that the stream keeps being
 * received meanwhile: an isochronous device does not wait for the host. A
 * frame that completes while the previous one is still being drawn is not
 * shown.
 */
static K_SEM_DEFINE(display_sem, 0, 1);
static atomic_t display_busy;
static const uint8_t *display_buf;
static size_t display_len;

static void display_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&display_sem, K_FOREVER);
		uvc_display_frame(display_buf, display_len);
		atomic_clear(&display_busy);
	}
}

K_THREAD_DEFINE(display_tid, 2048, display_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(5), 0, 0);

static void show_frame(const uint8_t *frame, size_t len)
{
	if (!atomic_cas(&display_busy, 0, 1)) {
		return;
	}

	display_buf = frame;
	display_len = len;

	/* Receive the next frames into the other buffer meanwhile */
	frame_buf_idx = (frame_buf_idx + 1) % NUM_FRAME_BUFS;
	uvc_stream_set_buffer(&stream, frame_bufs[frame_buf_idx],
			      sizeof(frame_bufs[0]));

	k_sem_give(&display_sem);
}

/* Let the frame being drawn finish before the display format changes */
static void display_wait_idle(void)
{
	while (atomic_get(&display_busy)) {
		k_sleep(K_MSEC(1));
	}
}
#else
static void show_frame(const uint8_t *frame, size_t len)
{
}

static void display_wait_idle(void)
{
}
#endif

/* JPEG: SOI first; EOI last, allowing for a few bytes of zero padding */
static bool jpeg_ok(const uint8_t *frame, size_t len)
{
	size_t end = len;

	if (len < 4 || frame[0] != 0xFF || frame[1] != 0xD8) {
		return false;
	}

	while (end > 2 && len - end < 16 && frame[end - 1] == 0x00) {
		end--;
	}

	return frame[end - 2] == 0xFF && frame[end - 1] == 0xD9;
}

static void frame_done(const uint8_t *frame, size_t len, bool err)
{
	bool bad = err;

	if (!bad && check.mjpeg) {
		bad = !jpeg_ok(frame, len);
	} else if (!bad && check.expected_size != 0) {
		bad = (len != check.expected_size);
	}

	if (stats.frames == 0) {
		LOG_INF("First frame: %zu bytes%s, starts %02x %02x %02x %02x",
			len, bad ? " (bad)" : "", len > 0 ? frame[0] : 0,
			len > 1 ? frame[1] : 0, len > 2 ? frame[2] : 0,
			len > 3 ? frame[3] : 0);
	}

	if (bad && stats.bad_frames < 10) {
		LOG_WRN("Bad frame #%u: %zu bytes (expected %zu)%s", stats.frames,
			len, check.expected_size,
			err ? " (error flagged in stream)" : "");
	}

	if (!bad) {
		show_frame(frame, len);
	}

	stats.frames++;
	stats.bad_frames += bad ? 1 : 0;
	stats.bytes += len;
	stats.win_frames++;
	stats.win_bytes += len;
}

static void log_stats(void)
{
	uint32_t now = k_uptime_get_32();
	uint32_t ms = now - stats.win_start;

	if (ms < 1000) {
		return;
	}

	uint32_t fps_x10 = stats.win_frames * 10000U / ms;
	uint32_t kbps = (uint32_t)(stats.win_bytes / ms); /* bytes/ms = KB/s */

	LOG_INF("%u.%u fps, %u KB/s, avg %u B/frame | frames %u bad %u "
		"| hdr err %u, stream err %u, overflow %u, timeouts %u, lost %u",
		fps_x10 / 10, fps_x10 % 10, kbps,
		stats.win_frames ? (uint32_t)(stats.win_bytes / stats.win_frames) : 0,
		stats.frames, stats.bad_frames, stream.bad_headers,
		stream.err_payloads, stream.overflows, stats.timeouts,
		stats.lost);

	stats.win_frames = 0;
	stats.win_bytes = 0;
	stats.win_start = now;
}

static void list_formats(void)
{
	char name[5];

	LOG_INF("UVC %x.%02x, VideoStreaming interface %u, %s EP 0x%02x mps %u",
		uvc.bcd_uvc >> 8, uvc.bcd_uvc & 0xFF, uvc.vs_iface,
		uvc.ep_type == USB_EP_TYPE_BULK ? "bulk" : "isochronous",
		uvc.ep, uvc.ep_mps);

	for (int i = 0; i < uvc.num_alts; i++) {
		LOG_INF("  Alt %u: %u bytes per interval, bInterval %u",
			uvc.alts[i].alt, uvc_alt_payload(&uvc.alts[i]),
			uvc.alts[i].interval);
	}

	for (int i = 0; i < uvc.num_formats; i++) {
		const struct uvc_format *fmt = &uvc.formats[i];

		LOG_INF("  Format %u: %s%s", fmt->index,
			uvc_format_name(fmt, name),
			fmt->bpp ? "" : " (compressed)");

		for (int j = 0; j < uvc.num_frames; j++) {
			const struct uvc_frame *frm = &uvc.frames[j];

			if (frm->format_index != fmt->index) {
				continue;
			}
			LOG_INF("    Frame %u: %ux%u, default %u.%u fps",
				frm->index, frm->width, frm->height,
				frm->default_interval ?
					10000000U / frm->default_interval : 0,
				frm->default_interval ?
					(100000000U / frm->default_interval) % 10 : 0);
		}
	}
}

/* Pick the format and frame selected in Kconfig (0 = default) */
static const struct uvc_frame *select_frame(const struct uvc_format **fmt_out)
{
	const struct uvc_format *fmt;
	uint8_t frame_index;

	if (CONFIG_SAMPLE_UVC_FORMAT_INDEX == 0) {
		fmt = &uvc.formats[0];
	} else {
		fmt = uvc_find_format(&uvc, CONFIG_SAMPLE_UVC_FORMAT_INDEX);
	}

	if (fmt == NULL) {
		LOG_ERR("Format %u not found", CONFIG_SAMPLE_UVC_FORMAT_INDEX);
		return NULL;
	}

	frame_index = CONFIG_SAMPLE_UVC_FRAME_INDEX ?
		      CONFIG_SAMPLE_UVC_FRAME_INDEX : fmt->default_frame;

	*fmt_out = fmt;

	return uvc_find_frame(&uvc, fmt->index, frame_index);
}

static int read_config_descriptor(size_t *len)
{
	uint8_t hdr[9];
	uint16_t total;
	int ret;

	ret = uhc_dwc3_control_transfer(uhc_dev, 0x80, USB_SREQ_GET_DESCRIPTOR,
					USB_DESC_CONFIGURATION << 8, 0,
					sizeof(hdr), hdr);
	if (ret < (int)sizeof(hdr)) {
		return ret < 0 ? ret : -EIO;
	}

	total = sys_get_le16(&hdr[2]);
	if (total > sizeof(cfg_desc)) {
		LOG_WRN("Config descriptor is %u bytes, reading the first %zu",
			total, sizeof(cfg_desc));
		total = sizeof(cfg_desc);
	}

	ret = uhc_dwc3_control_transfer(uhc_dev, 0x80, USB_SREQ_GET_DESCRIPTOR,
					USB_DESC_CONFIGURATION << 8, 0,
					total, cfg_desc);
	if (ret < 0) {
		return ret;
	}

	*len = ret;

	return 0;
}

/*
 * Size of each bulk IN transfer: whole packets, at most one chunk. A payload
 * that fills dwMaxPayloadTransferSize ends without a short packet, so the
 * transfers must then split it exactly, or one would run on into the header
 * of the next payload.
 */
static size_t xfer_size(uint32_t max_payload)
{
	size_t step = ROUND_UP(uvc.ep_mps, UHC_DWC3_BUF_ALIGN);
	size_t len = MIN(CHUNK_SIZE, ROUND_UP(max_payload, step));

	if (max_payload % uvc.ep_mps == 0) {
		while (len > step && max_payload % len != 0) {
			len -= step;
		}
	}

	return len;
}

/*
 * Select the isochronous alternate setting that carries the negotiated
 * payloads, configure its endpoint and select it, which starts the stream.
 * Returns the transfer size.
 */
static int start_isoch(uint32_t max_payload)
{
	const struct uvc_alt *alt = uvc_select_alt(&uvc, max_payload);
	uint32_t len;
	int ret;

	if (alt == NULL) {
		LOG_ERR("No alternate setting carries %u byte payloads",
			max_payload);
		return -ENOTSUP;
	}

	len = uvc_alt_payload(alt);
	LOG_INF("Alternate setting %u: %u bytes per interval, bInterval %u",
		alt->alt, len, alt->interval);

	if (len > ISO_BUF_SIZE) {
		return -ENOTSUP;
	}

	ret = uhc_dwc3_configure_isoch_in(uhc_dev, uvc.ep, alt->mps,
					  alt->interval);
	if (ret) {
		LOG_ERR("Configuring EP 0x%02x failed: %d", uvc.ep, ret);
		return ret;
	}

	ret = uhc_dwc3_control_transfer(uhc_dev, 0x01, USB_SREQ_SET_INTERFACE,
					alt->alt, uvc.vs_iface, 0, NULL);
	if (ret < 0) {
		LOG_ERR("SET_INTERFACE %u alt %u failed: %d", uvc.vs_iface,
			alt->alt, ret);
		return ret;
	}

	return len;
}

/* Enumerate the device and get it streaming; returns the transfer size */
static int setup_stream(void)
{
	const struct uvc_format *fmt;
	const struct uvc_frame *frame;
	struct usb_device_descriptor desc;
	struct uvc_probe probe;
	uint32_t max_payload;
	uint32_t max_frame;
	size_t cfg_len;
	char name[5];
	int xfer_len;
	int ret;

	ret = uhc_dwc3_enumerate_device(uhc_dev, &desc);
	if (ret) {
		LOG_ERR("Enumeration failed: %d", ret);
		return ret;
	}

	LOG_INF("Device: VID=0x%04x PID=0x%04x",
		sys_le16_to_cpu(desc.idVendor), sys_le16_to_cpu(desc.idProduct));

	ret = read_config_descriptor(&cfg_len);
	if (ret) {
		LOG_ERR("Reading the config descriptor failed: %d", ret);
		return ret;
	}

	ret = uvc_parse_config(cfg_desc, cfg_len, &uvc);
	if (ret == -ENODEV) {
		LOG_ERR("Not a USB Video Class device");
		return ret;
	} else if (ret) {
		LOG_ERR("No usable VideoStreaming interface found");
		return ret;
	}

	list_formats();

	if (uvc.ep_type != USB_EP_TYPE_BULK && uvc.ep_type != USB_EP_TYPE_ISO) {
		LOG_ERR("Streaming endpoint type %u is not supported",
			uvc.ep_type);
		return -ENOTSUP;
	}

	frame = select_frame(&fmt);
	if (frame == NULL) {
		LOG_ERR("Frame %u not found", CONFIG_SAMPLE_UVC_FRAME_INDEX);
		return -EINVAL;
	}

	ret = uhc_dwc3_control_transfer(uhc_dev, 0x00,
					USB_SREQ_SET_CONFIGURATION,
					uvc.cfg_val, 0, 0, NULL);
	if (ret < 0) {
		LOG_ERR("SET_CONFIGURATION failed: %d", ret);
		return ret;
	}

	if (uvc.ep_type == USB_EP_TYPE_BULK) {
		ret = uhc_dwc3_configure_bulk_in(uhc_dev, uvc.ep, uvc.ep_mps);
		if (ret) {
			LOG_ERR("Configuring EP 0x%02x failed: %d", uvc.ep,
				ret);
			return ret;
		}
	}

	ret = uvc_negotiate(uhc_dev, &uvc, frame, &probe);
	if (ret) {
		return ret;
	}

	/* Go with the format and frame the device settled on */
	if (probe.bFormatIndex != fmt->index ||
	    probe.bFrameIndex != frame->index) {
		fmt = uvc_find_format(&uvc, probe.bFormatIndex);
		frame = uvc_find_frame(&uvc, probe.bFormatIndex,
				       probe.bFrameIndex);
		if (fmt == NULL || frame == NULL) {
			LOG_ERR("Device committed an unknown format/frame");
			return -EINVAL;
		}
	}

	max_payload = sys_le32_to_cpu(probe.dwMaxPayloadTransferSize);
	max_frame = sys_le32_to_cpu(probe.dwMaxVideoFrameSize);

	LOG_INF("Streaming format %u (%s) frame %u: %ux%u, interval %u00 ns",
		fmt->index, uvc_format_name(fmt, name), frame->index,
		frame->width, frame->height,
		sys_le32_to_cpu(probe.dwFrameInterval));
	LOG_INF("  dwMaxVideoFrameSize %u, dwMaxPayloadTransferSize %u",
		max_frame, max_payload);

	if (max_payload == 0) {
		LOG_ERR("Device reported a zero payload size");
		return -EINVAL;
	}

	if (max_frame > sizeof(frame_bufs[0])) {
		LOG_ERR("Frames of up to %u bytes do not fit the %zu byte "
			"buffer, raise CONFIG_SAMPLE_UVC_FRAME_BUFFER_SIZE",
			max_frame, sizeof(frame_bufs[0]));
		return -ENOMEM;
	}

	check.mjpeg = (fmt->subtype == UVC_VS_FORMAT_MJPEG);
	check.expected_size = (fmt->subtype == UVC_VS_FORMAT_UNCOMPRESSED) ?
		(size_t)frame->width * frame->height * fmt->bpp / 8 : 0;

	display_wait_idle();
	uvc_display_set_format(fmt, frame->width, frame->height);

	uvc_stream_init(&stream, max_payload, frame_bufs[frame_buf_idx],
			sizeof(frame_bufs[0]), frame_done);
	memset(&stats, 0, sizeof(stats));
	stats.win_start = k_uptime_get_32();

	if (uvc.ep_type == USB_EP_TYPE_ISO) {
		xfer_len = start_isoch(max_payload);
	} else {
		xfer_len = xfer_size(max_payload);
	}

	return xfer_len;
}

/* Receive until the device goes away or the stream fails */
static void run_isoch(size_t xfer_len)
{
	uint8_t *buf;
	int ret;

	for (int i = 0; i < NUM_ISO_XFERS; i++) {
		ret = uhc_dwc3_isoch_in_submit(uhc_dev, iso_bufs[i], xfer_len);
		if (ret) {
			LOG_ERR("Submitting transfer %d failed: %d", i, ret);
			return;
		}
	}

	while (true) {
		ret = uhc_dwc3_isoch_in_reap(uhc_dev, &buf, REAP_TIMEOUT_MS);
		if (ret == -EAGAIN) {
			/* A streaming isochronous endpoint completes a
			 * transfer every interval, also with no data.
			 */
			stats.timeouts++;
			if (!port_connected()) {
				LOG_INF("Device disconnected");
			} else {
				LOG_ERR("Isochronous stream stopped");
			}
			return;
		}

		if (ret == -ENODATA || ret == -EIO) {
			stats.lost++;
			uvc_stream_lost(&stream);
		} else if (ret >= 0) {
			trace_xfer(buf, ret, xfer_len);
			uvc_stream_feed(&stream, buf, ret, true);
		} else {
			LOG_ERR("Isochronous IN failed: %d", ret);
			return;
		}

		ret = uhc_dwc3_isoch_in_submit(uhc_dev, buf, xfer_len);
		if (ret) {
			LOG_ERR("Resubmitting transfer failed: %d", ret);
			return;
		}

		log_stats();
	}
}

static void run_bulk(size_t xfer_len)
{
	uint8_t *buf;
	int ret;

	for (int i = 0; i < NUM_CHUNKS; i++) {
		ret = uhc_dwc3_bulk_in_submit(uhc_dev, chunks[i], xfer_len);
		if (ret) {
			LOG_ERR("Submitting transfer %d failed: %d", i, ret);
			return;
		}
	}

	while (true) {
		ret = uhc_dwc3_bulk_in_reap(uhc_dev, &buf, REAP_TIMEOUT_MS);
		if (ret == -EAGAIN) {
			stats.timeouts++;
			if (!port_connected()) {
				LOG_INF("Device disconnected");
				return;
			}
			LOG_WRN("No video data for %u ms", REAP_TIMEOUT_MS);
			continue;
		} else if (ret < 0) {
			LOG_ERR("Bulk IN failed: %d", ret);
			return;
		}

		trace_xfer(buf, ret, xfer_len);
		uvc_stream_feed(&stream, buf, ret, (size_t)ret < xfer_len);

		ret = uhc_dwc3_bulk_in_submit(uhc_dev, buf, xfer_len);
		if (ret) {
			LOG_ERR("Resubmitting transfer failed: %d", ret);
			return;
		}

		log_stats();
	}
}

int main(void)
{
	int ret;

	LOG_INF("USB Host UVC Capture Sample");
	LOG_INF("===========================");

	if (!device_is_ready(uhc_dev)) {
		LOG_ERR("USB host controller device not ready");
		return -ENODEV;
	}

	ret = uhc_init(uhc_dev, usb_host_event_cb, &sample_uhs_ctx);
	if (ret) {
		LOG_ERR("Failed to initialize UHC: %d", ret);
		return ret;
	}

	ret = uhc_enable(uhc_dev);
	if (ret) {
		LOG_ERR("Failed to enable UHC: %d", ret);
		return ret;
	}

	/* Without a working display the sample still captures and checks */
	if (uvc_display_init() != 0) {
		LOG_WRN("Continuing without the display");
	}

	LOG_INF("Waiting for a USB video device...");

	while (true) {
		k_sleep(K_SECONDS(1));

		if (!port_connected()) {
			continue;
		}

		LOG_INF("Device detected, enumerating...");
		k_sleep(K_MSEC(500)); /* Let the device settle */

		ret = setup_stream();
		if (ret < 0) {
			LOG_INF("Retrying in 5 seconds...");
			k_sleep(K_SECONDS(5));
			continue;
		}

		/* Returns when the device is unplugged or the stream fails;
		 * a device that is still attached is enumerated again.
		 */
		if (uvc.ep_type == USB_EP_TYPE_ISO) {
			run_isoch(ret);
		} else {
			run_bulk(ret);
		}
		log_stats();
	}

	return 0;
}
