/*
 * Copyright (c) 2026 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shows each captured frame centred on the DK display. The CDC200 layer is
 * RGB565, so RGB565 ("RGBP") frames are copied as they are and YUY2 frames
 * are converted; frames larger than the panel are shrunk by an integer
 * factor (nearest neighbour).
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/cdc200.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dsi/dsi_dw.h>
#include <zephyr/sys/byteorder.h>

#include "display.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(uvc_display, LOG_LEVEL_INF);

#define DISPLAY_NODE	DT_CHOSEN(zephyr_display)
#define DSI_NODE	DT_ALIAS(mipi_dsi)
#define PANEL_NODE	DT_ALIAS(panel)

/* Widest panel row the line buffer holds */
#define MAX_PANEL_WIDTH	800

static const struct device *const display_dev = DEVICE_DT_GET(DISPLAY_NODE);

static uint16_t panel_w;
static uint16_t panel_h;
static bool ready;

/* One output row, converted and/or shrunk */
static uint16_t line[MAX_PANEL_WIDTH];

/* Where and how the current format is drawn */
static struct {
	bool ok;
	bool yuy2;
	uint16_t width;		/* Source frame */
	uint16_t height;
	uint16_t step;		/* Shrink factor */
	uint16_t out_w;		/* On the panel */
	uint16_t out_h;
	uint16_t x;
	uint16_t y;
} view;

#if DT_NODE_HAS_PROP(DSI_NODE, cam_disp_mux_gpios)
/*
 * Route the shared MIPI D-PHY lanes to the display (E4/E6/E8 DK) before the
 * panel driver sends its initialisation commands over DSI.
 */
static int select_display_mux(void)
{
	const struct gpio_dt_spec mux =
		GPIO_DT_SPEC_GET(DSI_NODE, cam_disp_mux_gpios);

	if (!gpio_is_ready_dt(&mux)) {
		return -ENODEV;
	}

	return gpio_pin_configure_dt(&mux, GPIO_OUTPUT_ACTIVE);
}

SYS_INIT(select_display_mux, PRE_KERNEL_1, 46);
#endif

static void write_row(uint16_t y, const uint16_t *pixels, uint16_t width,
		      uint16_t x)
{
	struct display_buffer_descriptor desc = {
		.buf_size = width * sizeof(uint16_t),
		.width = width,
		.height = 1,
		.pitch = width,
	};

	display_write(display_dev, x, y, &desc, pixels);
}

static void clear_screen(void)
{
	memset(line, 0, sizeof(line));

	for (uint16_t y = 0; y < panel_h; y++) {
		write_row(y, line, panel_w, 0);
	}
}

int uvc_display_init(void)
{
	const struct device *dsi = DEVICE_DT_GET(DSI_NODE);
	struct display_capabilities caps;
	int ret;

	if (!device_is_ready(dsi) || !device_is_ready(display_dev)) {
		LOG_ERR("Display not ready");
		return -ENODEV;
	}

	ret = dsi_dw_set_mode(dsi, DSI_DW_VIDEO_MODE);
	if (ret) {
		LOG_ERR("Setting DSI video mode failed: %d", ret);
		return ret;
	}

#if DT_NODE_HAS_STATUS(PANEL_NODE, okay)
	const struct device *panel = DEVICE_DT_GET(PANEL_NODE);

	if (!device_is_ready(panel) || display_blanking_off(panel) != 0) {
		LOG_WRN("Display panel not ready, continuing");
	}
#endif

	cdc200_set_enable(display_dev, true);

	display_get_capabilities(display_dev, &caps);
	if (caps.current_pixel_format != PIXEL_FORMAT_RGB_565 ||
	    caps.x_resolution > MAX_PANEL_WIDTH) {
		LOG_ERR("Display must be RGB565 and at most %u wide",
			MAX_PANEL_WIDTH);
		return -ENOTSUP;
	}

	panel_w = caps.x_resolution;
	panel_h = caps.y_resolution;
	ready = true;

	clear_screen();

	LOG_INF("Display %ux%u ready", panel_w, panel_h);

	return 0;
}

bool uvc_display_set_format(const struct uvc_format *fmt, uint16_t width,
			    uint16_t height)
{
	bool rgb565 = (fmt->subtype == UVC_VS_FORMAT_UNCOMPRESSED &&
		       memcmp(fmt->guid, "RGBP", 4) == 0);
	bool yuy2 = (fmt->subtype == UVC_VS_FORMAT_UNCOMPRESSED &&
		     memcmp(fmt->guid, "YUY2", 4) == 0);
	uint16_t step;

	view.ok = false;

	if (!ready) {
		return false;
	}

	if (!rgb565 && !yuy2) {
		LOG_WRN("Format cannot be shown, only RGB565 and YUY2 can");
		clear_screen();
		return false;
	}

	step = MAX(DIV_ROUND_UP(width, panel_w), DIV_ROUND_UP(height, panel_h));

	view.yuy2 = yuy2;
	view.width = width;
	view.height = height;
	view.step = step;
	view.out_w = width / step;
	view.out_h = height / step;
	view.x = (panel_w - view.out_w) / 2;
	view.y = (panel_h - view.out_h) / 2;
	view.ok = true;

	/* Drop whatever an earlier stream left around the picture */
	clear_screen();

	LOG_INF("Showing %ux%u as %ux%u at (%u, %u)", width, height,
		view.out_w, view.out_h, view.x, view.y);

	return true;
}

static uint16_t yuv_to_rgb565(int y, int u, int v)
{
	/* ITU-R BT.601, limited range */
	int c = 298 * (y - 16) + 128;
	int d = u - 128;
	int e = v - 128;
	int r = CLAMP((c + 409 * e) >> 8, 0, 255);
	int g = CLAMP((c - 100 * d - 208 * e) >> 8, 0, 255);
	int b = CLAMP((c + 516 * d) >> 8, 0, 255);

	return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void convert_row(const uint8_t *src)
{
	for (uint16_t i = 0; i < view.out_w; i++) {
		uint32_t col = (uint32_t)i * view.step;

		if (view.yuy2) {
			/* Y0 U Y1 V: each pair of pixels shares U and V */
			const uint8_t *pair = &src[(col & ~1U) * 2];

			line[i] = yuv_to_rgb565(src[col * 2], pair[1], pair[3]);
		} else {
			line[i] = sys_get_le16(&src[col * 2]);
		}
	}
}

void uvc_display_frame(const uint8_t *frame, size_t len)
{
	size_t stride = (size_t)view.width * 2;

	if (!view.ok || len < stride * view.height) {
		return;
	}

	if (!view.yuy2 && view.step == 1) {
		struct display_buffer_descriptor desc = {
			.buf_size = len,
			.width = view.width,
			.height = view.height,
			.pitch = view.width,
		};

		display_write(display_dev, view.x, view.y, &desc, frame);
		return;
	}

	for (uint16_t row = 0; row < view.out_h; row++) {
		convert_row(&frame[(size_t)row * view.step * stride]);
		write_row(view.y + row, line, view.out_w, view.x);
	}
}
