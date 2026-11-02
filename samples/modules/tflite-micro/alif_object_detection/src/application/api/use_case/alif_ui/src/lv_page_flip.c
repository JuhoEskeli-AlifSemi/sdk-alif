/*
 * Copyright (c) 2026 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tear-free LVGL output on the CDC200.
 *
 * Zephyr's LVGL port renders the screen in parts and copies each part into
 * the framebuffer on screen with display_write(), so a redraw that spans
 * several panel refreshes (e.g. the camera image) shows part of the old and
 * part of the new frame. Here LVGL instead renders in direct mode into two
 * full-screen framebuffers: the one off screen is drawn, then swapped in with
 * cdc200_swap_fb() in the vertical blanking. LVGL copies the areas it redrew
 * into the other buffer before drawing the next frame, so both stay complete.
 *
 * The swap is not waited for in the flush: LVGL calls flush_wait_cb when it
 * next needs the buffer, at the start of the next redraw, by when the swap
 * has normally happened. Whoever holds a lock around lv_timer_handler() is
 * then not held up for a vertical blanking after every frame.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>

#include "lvgl.h"
#include "lv_page_flip.h"

LOG_MODULE_REGISTER(lv_page_flip, LOG_LEVEL_INF);

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)

#if defined(CONFIG_CDC200) && DT_NODE_HAS_COMPAT(DISPLAY_NODE, tes_cdc_2_1) && \
	DT_NODE_HAS_STATUS(DISPLAY_NODE, okay)

#include <zephyr/drivers/display/cdc200.h>

/* One RGB565 layer the size of the panel */
#define FB_SIZE (DT_PROP(DISPLAY_NODE, width) * DT_PROP(DISPLAY_NODE, height) * 2)

/* How long a swap may take: the next vertical blanking */
#define SWAP_TIMEOUT K_MSEC(100)

static const struct device *const display_dev = DEVICE_DT_GET(DISPLAY_NODE);

/* Frames put on screen, for lv_page_flip_count() */
static atomic_t frames;

/* Second framebuffer, in SRAM0 like the driver's, which the CDC200 reads */
static uint8_t fb1[FB_SIZE] __aligned(64) __alif_sram0_section;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	struct cdc200_fb_desc fb = {
		.fb_addr = px_map,
		.fb_size = FB_SIZE,
	};

	ARG_UNUSED(area);

	/* In direct mode px_map is the whole buffer; swap once it is complete */
	if (!lv_display_flush_is_last(disp)) {
		lv_display_flush_ready(disp);
		return;
	}

	/*
	 * The CDC200 reads from memory. LVGL also copied the areas redrawn in
	 * the previous frame into this buffer, so write all of it back, not
	 * only the areas just drawn.
	 */
	sys_cache_data_flush_range(px_map, FB_SIZE);

	/* Left pending: flush_wait_cb() waits for the swap */
	cdc200_swap_fb(display_dev, 0, &fb);
}

static void flush_wait_cb(lv_display_t *disp)
{
	ARG_UNUSED(disp);

	if (cdc200_wait_vsync(display_dev, SWAP_TIMEOUT) != 0) {
		LOG_WRN("Framebuffer swap timed out");
	}

	atomic_inc(&frames);
}

uint32_t lv_page_flip_count(void)
{
	return (uint32_t)atomic_get(&frames);
}

int lv_page_flip_init(void)
{
	lv_display_t *disp = lv_display_get_default();
	struct cdc200_fb_desc fb0;

	if (disp == NULL || !device_is_ready(display_dev)) {
		return -ENODEV;
	}

	cdc200_get_framebuffer(display_dev, 0, &fb0);
	if (fb0.fb_size != FB_SIZE ||
	    lv_display_get_color_format(disp) != LV_COLOR_FORMAT_RGB565) {
		LOG_ERR("Page flipping needs one full-screen RGB565 layer");
		return -ENOTSUP;
	}

	memset(fb1, 0, sizeof(fb1));
	sys_cache_data_flush_range(fb1, sizeof(fb1));

	/*
	 * Draw the first frame into fb1, as the driver's buffer is on
	 * screen, then alternate.
	 */
	lv_display_set_buffers(disp, fb1, fb0.fb_addr, FB_SIZE,
			       LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, flush_cb);
	lv_display_set_flush_wait_cb(disp, flush_wait_cb);
	lv_obj_invalidate(lv_screen_active());

	LOG_INF("Page flipping between two %u byte framebuffers", FB_SIZE);

	return 0;
}

#else

int lv_page_flip_init(void)
{
	return -ENODEV;
}

uint32_t lv_page_flip_count(void)
{
	return 0;
}

#endif
