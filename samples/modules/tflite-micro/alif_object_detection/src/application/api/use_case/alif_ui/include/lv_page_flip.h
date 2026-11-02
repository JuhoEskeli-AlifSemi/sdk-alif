/*
 * Copyright (c) 2026 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LV_PAGE_FLIP_H_
#define LV_PAGE_FLIP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Make LVGL draw into two full-screen CDC200 framebuffers and put each
 * finished frame on screen in the vertical blanking, so that the panel never
 * shows a partly drawn frame. Call once LVGL is initialised, before the first
 * refresh. Returns 0 on success; without a CDC200 display, -ENODEV and LVGL
 * keeps its default buffering.
 */
int lv_page_flip_init(void);

/* Number of frames put on screen so far (0 without page flipping) */
uint32_t lv_page_flip_count(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_PAGE_FLIP_H_ */
