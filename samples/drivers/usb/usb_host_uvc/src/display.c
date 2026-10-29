/*
 * Copyright (c) 2026 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shows each captured frame centred on the DK display. The CDC200 layer is
 * RGB565, so RGB565 ("RGBP") frames are copied as they are and YUY2 frames
 * are converted. A frame is turned by 90 degrees when it fills more of the
 * panel that way, e.g. a landscape frame on a portrait panel, and frames
 * larger than the panel are shrunk by an integer factor (nearest neighbour).
 *
 * Drawing a frame takes longer than the panel takes to refresh, so frames
 * are drawn into a framebuffer that is not on screen, which is then put on
 * screen in the vertical blanking (cdc200_swap_fb()). Without that, the
 * panel would show part of the old and part of the new frame.
 *
 * With AIPL (CONFIG_AIPL), turned frames are drawn by the D/AVE2D GPU: YUY2
 * frames are first converted to RGB565 with AIPL's Helium code, as D/AVE2D
 * cannot read YUY2, then D/AVE2D turns the RGB565 frame straight into the
 * framebuffer, in narrow strips. Other frames, and turned ones if the GPU
 * fails, are drawn by the CPU.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/cdc200.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dsi/dsi_dw.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/byteorder.h>

#if defined(CONFIG_AIPL)
#include <aipl_cache.h>
#include <aipl_color_conversion.h>
#include <aipl_dave2d.h>
#include <dave_d0lib.h>
#endif

#include "display.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(uvc_display, LOG_LEVEL_INF);

#define DISPLAY_NODE	DT_CHOSEN(zephyr_display)
#define DSI_NODE	DT_ALIAS(mipi_dsi)
#define PANEL_NODE	DT_ALIAS(panel)

/* RGB565 framebuffer the size of the panel */
#define FB_SIZE		(DT_PROP(DISPLAY_NODE, width) * \
			 DT_PROP(DISPLAY_NODE, height) * sizeof(uint16_t))

/*
 * Panel rows built at a time when turning a frame. Each panel row is a
 * column of the frame, so building several at once reads the frame a few
 * neighbouring pixels at a time instead of one pixel per frame row.
 */
#define ROTATE_ROWS	16

/* How long a buffer swap may take: the next vertical blanking */
#define SWAP_TIMEOUT	K_MSEC(100)

/* Frames drawn between logs of the time drawing takes */
#define DRAW_LOG_FRAMES	100

static const struct device *const display_dev = DEVICE_DT_GET(DISPLAY_NODE);

static uint16_t panel_w;
static uint16_t panel_h;
static bool ready;

/*
 * The driver's framebuffer and a second one, in SRAM0 like the driver's,
 * which the CDC200 reads. fbs[back] is the one not on screen.
 */
static uint8_t fb1[FB_SIZE] __aligned(64) __alif_sram0_section;
static uint8_t *fbs[2];
static int back;

/* A swap was requested and fbs[back] may still be on screen */
static bool swap_pending;

/* Time the frame being drawn spent in wait_swap() */
static uint32_t frame_wait_cycles;

/* Time spent drawing frames, logged every DRAW_LOG_FRAMES frames */
static struct {
	uint32_t frames;
	uint32_t gpu_frames;
	uint64_t cycles;	/* Whole draw, without the swap */
	uint64_t convert_cycles; /* GPU path: YUY2 -> RGB565 with Helium */
	uint64_t flush_cycles;	/* GPU path: writing the RGB565 frame back */
	uint64_t gpu_cycles;	/* GPU path: D/AVE2D, invalidate overlapped */
	uint64_t swap_cycles;	/* Waiting for the previous buffer swap */
} draw_stats;

/* Where and how the current format is drawn */
static struct {
	bool ok;
	bool yuy2;
	bool rotate;		/* Turned by 90 degrees */
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

/* Pixel (x, y) of the framebuffer not on screen */
static inline uint16_t *back_pixel(uint16_t x, uint16_t y)
{
	return (uint16_t *)fbs[back] + (size_t)y * panel_w + x;
}

/* Put the framebuffer drawn into on screen and draw into the other one */
/*
 * Have the framebuffer drawn into put on screen in the next vertical
 * blanking and draw into the other one, once that has left the screen:
 * see wait_swap().
 */
static void request_swap(void)
{
	struct cdc200_fb_desc fb = {
		.fb_addr = fbs[back],
		.fb_size = FB_SIZE,
	};

	cdc200_swap_fb(display_dev, 0, &fb);
	swap_pending = true;
	back ^= 1;
}

/*
 * Wait until the framebuffer to draw into is no longer on screen. Called
 * just before writing it, so that the next frame can be prepared (e.g. a
 * YUY2 conversion) while the swap is still pending.
 */
static void wait_swap(void)
{
	uint32_t start;

	if (!swap_pending) {
		return;
	}

	start = k_cycle_get_32();
	if (cdc200_wait_vsync(display_dev, SWAP_TIMEOUT) != 0) {
		LOG_WRN("Framebuffer swap timed out");
	}
	swap_pending = false;

	frame_wait_cycles += k_cycle_get_32() - start;
}

static void clear_screen(void)
{
	wait_swap();

	for (int i = 0; i < ARRAY_SIZE(fbs); i++) {
		memset(fbs[i], 0, FB_SIZE);
		sys_cache_data_flush_range(fbs[i], FB_SIZE);
	}
}

static void gpu_init(void);

int uvc_display_init(void)
{
	const struct device *dsi = DEVICE_DT_GET(DSI_NODE);
	struct display_capabilities caps;
	struct cdc200_fb_desc fb;
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
	cdc200_get_framebuffer(display_dev, 0, &fb);
	if (caps.current_pixel_format != PIXEL_FORMAT_RGB_565 ||
	    fb.fb_size != FB_SIZE) {
		LOG_ERR("Display must be one full-screen RGB565 layer");
		return -ENOTSUP;
	}

	panel_w = caps.x_resolution;
	panel_h = caps.y_resolution;
	fbs[0] = fb.fb_addr;
	fbs[1] = fb1;
	back = 1;
	ready = true;

	clear_screen();
	gpu_init();

	LOG_INF("Display %ux%u ready", panel_w, panel_h);

	return 0;
}

static bool is_yuy2(const struct uvc_format *fmt)
{
	return fmt->subtype == UVC_VS_FORMAT_UNCOMPRESSED &&
	       memcmp(fmt->guid, "YUY2", 4) == 0;
}

static bool can_show(const struct uvc_format *fmt)
{
	return is_yuy2(fmt) ||
	       (fmt->subtype == UVC_VS_FORMAT_UNCOMPRESSED &&
		memcmp(fmt->guid, "RGBP", 4) == 0);
}

bool uvc_display_fits(const struct uvc_format *fmt, uint16_t width,
		      uint16_t height)
{
	if (!ready || !can_show(fmt)) {
		return false;
	}

	return (width <= panel_w && height <= panel_h) ||
	       (height <= panel_w && width <= panel_h);
}

/* Shrink factor that fits a w x h picture on the panel */
static uint16_t fit_step(uint16_t w, uint16_t h)
{
	return MAX(DIV_ROUND_UP(w, panel_w), DIV_ROUND_UP(h, panel_h));
}

bool uvc_display_set_format(const struct uvc_format *fmt, uint16_t width,
			    uint16_t height)
{
	uint16_t step = fit_step(width, height);
	uint16_t rot_step = fit_step(height, width);
	uint32_t area = (uint32_t)(width / step) * (height / step);
	uint32_t rot_area = (uint32_t)(height / rot_step) * (width / rot_step);

	view.ok = false;

	if (!ready) {
		return false;
	}

	if (!can_show(fmt)) {
		LOG_WRN("Format cannot be shown, only RGB565 and YUY2 can");
		clear_screen();
		return false;
	}

	/* Turn the picture if it then fills more of the panel */
	view.rotate = rot_area > area;
	if (view.rotate) {
		step = rot_step;
	}

	view.yuy2 = is_yuy2(fmt);
	view.width = width;
	view.height = height;
	view.step = step;
	view.out_w = (view.rotate ? height : width) / step;
	view.out_h = (view.rotate ? width : height) / step;
	view.x = (panel_w - view.out_w) / 2;
	view.y = (panel_h - view.out_h) / 2;
	view.ok = true;

	/* Drop whatever an earlier stream left around the picture */
	clear_screen();

	LOG_INF("Showing %ux%u as %ux%u at (%u, %u)%s", width, height,
		view.out_w, view.out_h, view.x, view.y,
		!view.rotate ? "" :
		IS_ENABLED(CONFIG_SAMPLE_UVC_DISPLAY_ROTATE_CCW) ?
		", turned counter-clockwise" : ", turned clockwise");

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

/* Pixel @p col of frame row @p src as RGB565 */
static inline uint16_t src_pixel(const uint8_t *src, uint32_t col)
{
	if (view.yuy2) {
		/* Y0 U Y1 V: each pair of pixels shares U and V */
		const uint8_t *pair = &src[(col & ~1U) * 2];

		return yuv_to_rgb565(src[col * 2], pair[1], pair[3]);
	}

	return sys_get_le16(&src[col * 2]);
}

static void convert_row(const uint8_t *src, uint16_t *dst)
{
	for (uint16_t i = 0; i < view.out_w; i++) {
		dst[i] = src_pixel(src, (uint32_t)i * view.step);
	}
}

/*
 * Draw the frame turned by 90 degrees: each panel row is a frame column.
 * Clockwise, the top of the frame ends up on the right of the panel;
 * counter-clockwise, on the left.
 */
static void draw_rotated(const uint8_t *frame, size_t stride)
{
	const bool ccw = IS_ENABLED(CONFIG_SAMPLE_UVC_DISPLAY_ROTATE_CCW);

	for (uint16_t y0 = 0; y0 < view.out_h; y0 += ROTATE_ROWS) {
		uint16_t rows = MIN(ROTATE_ROWS, view.out_h - y0);
		uint16_t *dst = back_pixel(view.x, view.y + y0);

		for (uint16_t x = 0; x < view.out_w; x++) {
			uint32_t src_row = ccw ? x : (view.out_w - 1 - x);
			const uint8_t *src =
				&frame[src_row * view.step * stride];

			for (uint16_t r = 0; r < rows; r++) {
				uint32_t y = y0 + r;
				uint32_t col = ccw ? (view.out_h - 1 - y) : y;

				dst[r * panel_w + x] =
					src_pixel(src, col * view.step);
			}
		}
	}
}

#if defined(CONFIG_AIPL)
/* Display lists and other memory of the D/AVE2D driver */
#define D0_HEAP_SIZE	(256 * 1024)

/*
 * Width of the strips D/AVE2D draws a turned frame in. In a strip w panel
 * columns wide, each panel row reads one pixel from each of w frame rows,
 * and the next panel row the pixels next to those. Measured on the E8 with
 * 800x448 frames, the texture cache holds about 8 frame rows: up to 8 wide
 * nearly every read hits and the time halves with each doubling of the
 * width, as the panel rows are written in longer pieces (31 ms at 1 wide,
 * 5 ms at 8), while at 16 and 32 every read misses (41 ms).
 */
#define ROTATE_STRIP_WIDTH	8

static uint8_t d0_heap[D0_HEAP_SIZE] __aligned(64) __alif_sram1_section;

/* A YUY2 frame converted to RGB565 for D/AVE2D. Only frames that fit the
 * panel are turned, so a framebuffer's worth is enough.
 */
static uint8_t rgb_frame[FB_SIZE] __aligned(64) __alif_sram1_section;

static bool gpu_ready;


/* Cache maintenance AIPL does around D/AVE2D accesses */
void aipl_cpu_cache_clean(const void *ptr, uint32_t size)
{
	sys_cache_data_flush_range((void *)ptr, size);
}

void aipl_cpu_cache_invalidate(const void *ptr, uint32_t size)
{
	sys_cache_data_invd_range((void *)ptr, size);
}

static void gpu_init(void)
{
#if defined(CONFIG_D1_MALLOC_D0LIB)
	if (!d0_initheapmanager(d0_heap, sizeof(d0_heap), d0_mm_fixed_range,
				NULL, 0, 0, 0, d0_ma_unified)) {
		LOG_ERR("D/AVE2D heap initialisation failed");
		return;
	}
#endif

	if (aipl_dave2d_init() != D2_OK) {
		LOG_ERR("D/AVE2D initialisation failed, drawing with the CPU");
		return;
	}

	gpu_ready = true;
}

/* Keep the first D/AVE2D error of a sequence of calls */
static inline void d2_check(d2_s32 *ret, d2_s32 result)
{
	if (*ret == D2_OK) {
		*ret = result;
	}
}

/*
 * Turn an RGB565 frame into the framebuffer not on screen with D/AVE2D,
 * drawn as textured boxes ROTATE_STRIP_WIDTH panel columns wide.
 */
static d2_s32 gpu_rotate(const void *src)
{
	const bool ccw = IS_ENABLED(CONFIG_SAMPLE_UVC_DISPLAY_ROTATE_CCW);
	d2_device *d2 = aipl_dave2d_handle();
	/* Panel pixel that shows the first frame pixel */
	d2_point x0 = view.x + (ccw ? 0 : view.out_w - 1);
	d2_point y0 = view.y + (ccw ? view.out_h - 1 : 0);
	/* Frame column per panel row down, frame row per panel column across */
	d2_s32 dyu = ccw ? -D2_FIX16(1) : D2_FIX16(1);
	d2_s32 dxv = ccw ? D2_FIX16(1) : -D2_FIX16(1);
	d2_s32 ret = D2_OK;

	d2_check(&ret, d2_startframe(d2));
	d2_check(&ret, d2_framebuffer(d2, fbs[back], panel_w, panel_w,
				      panel_h, d2_mode_rgb565));

	/* Opaque copy of the texture */
	d2_check(&ret, d2_settextureoperation(d2, d2_to_one, d2_to_copy,
					      d2_to_copy, d2_to_copy));
	d2_check(&ret, d2_settexture(d2, (void *)src, view.width, view.width,
				     view.height, d2_mode_rgb565));
	d2_check(&ret, d2_settexturemode(d2, 0));
	d2_check(&ret, d2_setfillmode(d2, d2_fm_texture));
	d2_check(&ret, d2_setblendmode(d2, d2_bm_alpha,
				       d2_bm_one_minus_alpha));
	d2_check(&ret, d2_setalphablendmode(d2, d2_bm_one,
					    d2_bm_one_minus_alpha));

	/*
	 * Clockwise, the frame top is on the right, so each panel column to
	 * the left is the next frame row; counter-clockwise, to the right.
	 */
	d2_check(&ret, d2_settexturemapping(d2, D2_FIX4(x0), D2_FIX4(y0), 0, 0,
					    0, dyu, dxv, 0));

	for (uint16_t x = 0; x < view.out_w; x += ROTATE_STRIP_WIDTH) {
		uint16_t w = MIN(ROTATE_STRIP_WIDTH, view.out_w - x);

		d2_check(&ret, d2_renderbox(d2, D2_FIX4(view.x + x),
					    D2_FIX4(view.y), D2_FIX4(w),
					    D2_FIX4(view.out_h)));
	}

	/* Close the display list and start it */
	d2_check(&ret, d2_endframe(d2));
	d2_check(&ret, d2_startframe(d2));

	/*
	 * Drop cached framebuffer lines while D/AVE2D draws, so that none is
	 * written back over the picture later
	 */
	sys_cache_data_invd_range(back_pixel(0, view.y),
				  (size_t)view.out_h * panel_w *
				  sizeof(uint16_t));

	/* Wait for the drawing to finish */
	d2_check(&ret, d2_endframe(d2));

	return ret;
}

/* Turn the frame into the framebuffer not on screen with D/AVE2D */
static bool draw_rotated_gpu(const uint8_t *frame)
{
	const void *src = frame;
	aipl_error_t err;
	d2_s32 ret;

	uint32_t t0 = k_cycle_get_32();
	uint32_t t1;

	if (!gpu_ready || view.step != 1) {
		return false;
	}

	if (view.yuy2) {
		err = aipl_color_convert_yuy2_to_rgb565(frame, rgb_frame,
							view.width, view.width,
							view.height);
		if (err != AIPL_ERR_OK) {
			LOG_ERR("YUY2 conversion failed: %d", err);
			gpu_ready = false;
			return false;
		}
		src = rgb_frame;
	}

	t1 = k_cycle_get_32();
	draw_stats.convert_cycles += t1 - t0;

	/* D/AVE2D reads the frame from memory */
	sys_cache_data_flush_range((void *)src,
				   (size_t)view.width * view.height *
				   sizeof(uint16_t));

	draw_stats.flush_cycles += k_cycle_get_32() - t1;

	wait_swap();

	t0 = k_cycle_get_32();
	ret = gpu_rotate(src);
	if (ret != D2_OK) {
		LOG_ERR("D/AVE2D rotation failed: %d, drawing with the CPU",
			(int)ret);
		gpu_ready = false;
		return false;
	}

	draw_stats.gpu_cycles += k_cycle_get_32() - t0;

	return true;
}
#else
static void gpu_init(void)
{
}

static bool draw_rotated_gpu(const uint8_t *frame)
{
	return false;
}
#endif

/* Average of a cycle total over the frames counted, in us */
static uint32_t avg_us(uint64_t cycles, uint32_t frames)
{
	return frames ? k_cyc_to_us_floor32(cycles / frames) : 0;
}

static void log_draw_time(bool gpu, uint32_t start)
{
	static uint32_t last_log_ms;
	uint32_t now = k_uptime_get_32();
	uint32_t fps_x10;

	draw_stats.cycles += k_cycle_get_32() - start - frame_wait_cycles;
	draw_stats.swap_cycles += frame_wait_cycles;
	draw_stats.gpu_frames += gpu;

	if (++draw_stats.frames < DRAW_LOG_FRAMES) {
		return;
	}

	/* Frames shown per second since the last log; the first is rough */
	fps_x10 = (now > last_log_ms) ?
		  draw_stats.frames * 10000U / (now - last_log_ms) : 0;
	last_log_ms = now;

	LOG_INF("%u.%u fps on screen; drawing a frame takes %u us on average, "
		"plus %u us waiting for the previous swap (%u of %u by the GPU)",
		fps_x10 / 10, fps_x10 % 10,
		avg_us(draw_stats.cycles, draw_stats.frames),
		avg_us(draw_stats.swap_cycles, draw_stats.frames),
		draw_stats.gpu_frames, draw_stats.frames);

	if (draw_stats.gpu_frames > 0) {
		LOG_INF("  GPU frames: YUY2 conversion %u us, cache flush %u us, "
			"D/AVE2D rotation %u us",
			avg_us(draw_stats.convert_cycles, draw_stats.gpu_frames),
			avg_us(draw_stats.flush_cycles, draw_stats.gpu_frames),
			avg_us(draw_stats.gpu_cycles, draw_stats.gpu_frames));
	}

	memset(&draw_stats, 0, sizeof(draw_stats));
}

/* Draw the frame into the framebuffer not on screen with the CPU */
static void draw_cpu(const uint8_t *frame, size_t stride)
{
	if (view.rotate) {
		draw_rotated(frame, stride);
	} else if (!view.yuy2 && view.step == 1) {
		for (uint16_t row = 0; row < view.out_h; row++) {
			memcpy(back_pixel(view.x, view.y + row),
			       &frame[row * stride], stride);
		}
	} else {
		for (uint16_t row = 0; row < view.out_h; row++) {
			convert_row(&frame[(size_t)row * view.step * stride],
				    back_pixel(view.x, view.y + row));
		}
	}

	/* The CDC200 reads the framebuffer from memory */
	sys_cache_data_flush_range(back_pixel(0, view.y),
				   (size_t)view.out_h * panel_w *
				   sizeof(uint16_t));
}

void uvc_display_frame(const uint8_t *frame, size_t len)
{
	size_t stride = (size_t)view.width * 2;
	uint32_t start = k_cycle_get_32();
	bool gpu = false;

	if (!view.ok || len < stride * view.height) {
		return;
	}

	frame_wait_cycles = 0;

	/* The GPU path waits for the swap itself, after the conversion */
	if (view.rotate) {
		gpu = draw_rotated_gpu(frame);
	}

	if (!gpu) {
		wait_swap();
		draw_cpu(frame, stride);
	}

	request_swap();

	log_draw_time(gpu, start);
}
