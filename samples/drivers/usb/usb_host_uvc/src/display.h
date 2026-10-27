/*
 * Copyright (c) 2026 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rendering of captured frames on the DK display (CDC200 + MIPI DSI panel).
 */

#ifndef USB_HOST_UVC_DISPLAY_H_
#define USB_HOST_UVC_DISPLAY_H_

#include <stddef.h>
#include <stdint.h>

#include "uvc.h"

#if defined(CONFIG_DISPLAY)

/* Bring up the DSI link, panel and display controller; clear the screen */
int uvc_display_init(void);

/*
 * Select the format and size of the frames to show next. Returns false if
 * the format cannot be shown, in which case frames are ignored.
 */
bool uvc_display_set_format(const struct uvc_format *fmt, uint16_t width,
			    uint16_t height);

/* Show one complete frame of the format set with uvc_display_set_format() */
void uvc_display_frame(const uint8_t *frame, size_t len);

#else

static inline int uvc_display_init(void)
{
	return 0;
}

static inline bool uvc_display_set_format(const struct uvc_format *fmt,
					  uint16_t width, uint16_t height)
{
	return false;
}

static inline void uvc_display_frame(const uint8_t *frame, size_t len)
{
}

#endif /* CONFIG_DISPLAY */

#endif /* USB_HOST_UVC_DISPLAY_H_ */
