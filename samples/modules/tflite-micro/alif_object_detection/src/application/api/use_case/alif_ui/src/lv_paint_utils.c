/* Copyright (C) 2022 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

#include <inttypes.h>
#include <stdlib.h>

#if defined __clang__ || defined __GNUC__
#pragma GCC diagnostic ignored "-Wvla"
#endif

#include "lvgl.h"

#define RGB_BYTES       3

#if 1
/* MVE optimization for RGB888 output */
#if defined __ARMCC_VERSION && (__ARM_FEATURE_MVE & 1)
#define ENABLE_MVE_WRITE 1
#else
#define ENABLE_MVE_WRITE 0
#endif
#else
#define ENABLE_MVE_WRITE 1
#endif

#if ENABLE_MVE_WRITE
#include <arm_mve.h>
#endif

#include <string.h>

#if !(LV_COLOR_DEPTH == 16 || LV_COLOR_DEPTH == 32)
#error "Unsupported LV_COLOR_DEPTH"
#endif

void write_to_lvgl_buf_doubled(
        int width, int height,
        const uint8_t * restrict src_ptr,
        lv_color_t * restrict dst_ptr)
{
    const uint8_t (*src)[width][RGB_BYTES] = (const uint8_t (*)[width][RGB_BYTES]) src_ptr;
    lv_color_t (*dst)[width * 2] = (lv_color_t (*)[width * 2]) dst_ptr;
    if (width % 16) {
        abort();
    }
	for (int y1 = 0; y1 < height; y1++) {

#if ENABLE_MVE_WRITE
		/*
		 * MVE optimized path for RGB888 output (lv_color_t = B, G, R), doubled.
		 * Input: RGB (R, G, B order), 3 bytes per pixel
		 * Output: BGR (B, G, R order), 3 bytes per pixel, doubled horizontally
		 *
		 * Note: MVE lacks efficient interleaved store (vst3) and table lookup,
		 * so we use gather loads and scatter stores with explicit offset patterns.
		 */
		const uint8x16_t inc3_gather = vmulq_n_u8(vidupq_n_u8(0, 1), 3); /* 0,3,6,9,... */

		const uint8_t *restrict srcp = src[y1][0];
		uint8_t *restrict dstp = (uint8_t *)dst[y1 * 2];
		uint8_t *restrict dst2p = (uint8_t *)dst[y1 * 2 + 1];

		for (int x1 = 0; x1 < width; x1 += 16)
		{
			/* Gather load R, G, B channels from 16 pixels */
			uint8x16_t r16 = vldrbq_gather_offset_u8(srcp + 0, inc3_gather);
			uint8x16_t g16 = vldrbq_gather_offset_u8(srcp + 1, inc3_gather);
			uint8x16_t b16 = vldrbq_gather_offset_u8(srcp + 2, inc3_gather);
			srcp += 16 * RGB_BYTES;

			/* Scatter store as BGR interleaved, doubled */
			/* Output: B0 G0 R0 B0 G0 R0 B1 G1 R1 B1 G1 R1 ... (6 bytes per input pixel) */
			/* Process first 8 pixels -> 16 output pixels (48 bytes) */
			const uint8x16_t scatter_b_lo = { 0, 3,  6,  9, 12, 15, 18, 21, 0, 0, 0, 0, 0, 0, 0, 0 };
			const uint8x16_t scatter_g_lo = { 1, 4,  7, 10, 13, 16, 19, 22, 0, 0, 0, 0, 0, 0, 0, 0 };
			const uint8x16_t scatter_r_lo = { 2, 5,  8, 11, 14, 17, 20, 23, 0, 0, 0, 0, 0, 0, 0, 0 };

			mve_pred16_t mask_lo = 0x00FF; /* First 8 elements */
			vstrbq_scatter_offset_p_u8(dstp,      scatter_b_lo, b16, mask_lo);
			vstrbq_scatter_offset_p_u8(dstp,      scatter_g_lo, g16, mask_lo);
			vstrbq_scatter_offset_p_u8(dstp,      scatter_r_lo, r16, mask_lo);
			/* Duplicate for doubled output */
			const uint8x16_t scatter_b_lo2 = { 24, 27, 30, 33, 36, 39, 42, 45, 0, 0, 0, 0, 0, 0, 0, 0 };
			const uint8x16_t scatter_g_lo2 = { 25, 28, 31, 34, 37, 40, 43, 46, 0, 0, 0, 0, 0, 0, 0, 0 };
			const uint8x16_t scatter_r_lo2 = { 26, 29, 32, 35, 38, 41, 44, 47, 0, 0, 0, 0, 0, 0, 0, 0 };
			vstrbq_scatter_offset_p_u8(dstp,      scatter_b_lo2, b16, mask_lo);
			vstrbq_scatter_offset_p_u8(dstp,      scatter_g_lo2, g16, mask_lo);
			vstrbq_scatter_offset_p_u8(dstp,      scatter_r_lo2, r16, mask_lo);

			/* Process second 8 pixels (upper half of vectors) */
			mve_pred16_t mask_hi = 0xFF00; /* Last 8 elements */
			const uint8x16_t scatter_b_hi = { 0, 0, 0, 0, 0, 0, 0, 0,  0,  3,  6,  9, 12, 15, 18, 21 };
			const uint8x16_t scatter_g_hi = { 0, 0, 0, 0, 0, 0, 0, 0,  1,  4,  7, 10, 13, 16, 19, 22 };
			const uint8x16_t scatter_r_hi = { 0, 0, 0, 0, 0, 0, 0, 0,  2,  5,  8, 11, 14, 17, 20, 23 };
			vstrbq_scatter_offset_p_u8(dstp + 48, scatter_b_hi, b16, mask_hi);
			vstrbq_scatter_offset_p_u8(dstp + 48, scatter_g_hi, g16, mask_hi);
			vstrbq_scatter_offset_p_u8(dstp + 48, scatter_r_hi, r16, mask_hi);
			const uint8x16_t scatter_b_hi2 = { 0, 0, 0, 0, 0, 0, 0, 0, 24, 27, 30, 33, 36, 39, 42, 45 };
			const uint8x16_t scatter_g_hi2 = { 0, 0, 0, 0, 0, 0, 0, 0, 25, 28, 31, 34, 37, 40, 43, 46 };
			const uint8x16_t scatter_r_hi2 = { 0, 0, 0, 0, 0, 0, 0, 0, 26, 29, 32, 35, 38, 41, 44, 47 };
			vstrbq_scatter_offset_p_u8(dstp + 48, scatter_b_hi2, b16, mask_hi);
			vstrbq_scatter_offset_p_u8(dstp + 48, scatter_g_hi2, g16, mask_hi);
			vstrbq_scatter_offset_p_u8(dstp + 48, scatter_r_hi2, r16, mask_hi);

			dstp += 32 * RGB_BYTES; /* 16 input pixels * 2 (doubled) * 3 bytes = 96 bytes */
		}
		/* Copy first row to second row */
		memcpy(dst2p, (uint8_t *)dst[y1 * 2], 2 * width * RGB_BYTES);
#else
		for (int x1 = 0; x1 < width; x1++) {
			uint8_t r, g, b;
			int32_t x, y;

			r = src[y1][x1][0];
			g = src[y1][x1][1];
			b = src[y1][x1][2];

			x = (x1 << 1);
			y = (y1 << 1);

			lv_color_t c = lv_color_make(r, g, b);
			dst[y][x] = c;
			dst[y][x+1] = c;
			dst[y+1][x] = c;
			dst[y+1][x+1] = c;
		}
#endif
	}
}

void write_to_lvgl_buf(
        int width, int height,
        const uint8_t * restrict src_ptr,
        lv_color_t * restrict dst_ptr)
{
    const uint8_t (*src)[width][RGB_BYTES] = (const uint8_t (*)[width][RGB_BYTES]) src_ptr;
    lv_color_t (*dst)[width] = (lv_color_t (*)[width]) dst_ptr;
    if (width % 16) {
        abort();
    }
	for (int y = 0; y < height; y++) {
#if ENABLE_MVE_WRITE
		/*
		 * MVE optimized path for RGB888 output (lv_color_t = B, G, R).
		 * Input: RGB (R, G, B order), 3 bytes per pixel
		 * Output: BGR (B, G, R order), 3 bytes per pixel
		 * Process 16 pixels at a time using gather loads and scatter stores.
		 */
		const uint8x16_t inc3 = vmulq_n_u8(vidupq_n_u8(0, 1), 3);
		/* Scatter offsets for BGR output: B0,G0,R0,B1,G1,R1,... */
		const uint8x16_t scatter_b = { 0, 3,  6,  9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45 };
		const uint8x16_t scatter_g = { 1, 4,  7, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46 };
		const uint8x16_t scatter_r = { 2, 5,  8, 11, 14, 17, 20, 23, 26, 29, 32, 35, 38, 41, 44, 47 };

		const uint8_t *restrict srcp = src[y][0];
		uint8_t *restrict dstp = (uint8_t *)dst[y];

		for (int x = 0; x < width; x += 16)
		{
			/* Gather load R, G, B channels from 16 pixels */
			uint8x16_t r = vldrbq_gather_offset_u8(srcp + 0, inc3);
			uint8x16_t g = vldrbq_gather_offset_u8(srcp + 1, inc3);
			uint8x16_t b = vldrbq_gather_offset_u8(srcp + 2, inc3);
			srcp += 16 * RGB_BYTES;

			/* Scatter store as BGR (B, G, R order) */
			vstrbq_scatter_offset_u8(dstp, scatter_b, b);
			vstrbq_scatter_offset_u8(dstp, scatter_g, g);
			vstrbq_scatter_offset_u8(dstp, scatter_r, r);
			dstp += 16 * RGB_BYTES;
		}
#else
		for (int x = 0; x < width; x++) {
			uint8_t r, g, b;

			r = src[y][x][0];
			g = src[y][x][1];
			b = src[y][x][2];

			dst[y][x] = lv_color_make(r, g, b);
		}
#endif
	}
}
