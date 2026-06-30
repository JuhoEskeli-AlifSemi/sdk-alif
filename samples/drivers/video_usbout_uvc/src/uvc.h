/*
 * Copyright (C) 2026 Alif Semiconductor.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal USB Video Class (UVC 1.10) MJPEG webcam, bulk-based.
 *
 * This is a sample-local USBD class implementation. It advertises a single
 * MJPEG format / single frame size to the host and streams JPEG frames out a
 * bulk IN endpoint, one UVC payload per frame. It is intentionally small: it
 * does not negotiate resolution or frame rate (the sensor delivers a fixed
 * full-resolution JPEG), so Probe/Commit always returns the one fixed
 * configuration.
 *
 * Why bulk and not isochronous: bulk needs no per-(micro)frame bandwidth
 * reservation and no alternate-setting juggling, the DWC3 controller streams
 * it efficiently, and Linux uvcvideo / Windows both accept bulk MJPEG. The
 * trade-off is there is no host-visible "stop" event for bulk streaming, so
 * stream teardown is driven by USB disable/disconnect (see uvc.c).
 */

#ifndef VIDEO_USBOUT_UVC_H_
#define VIDEO_USBOUT_UVC_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Declared frame geometry. Must match the format the sample configures on the
 * sensor (main.c). The host is told the stream is MJPEG of this size; the
 * actual JPEG bytes we send come straight from the sensor.
 */
#define UVC_FRAME_WIDTH  2592U
#define UVC_FRAME_HEIGHT 1944U

/*
 * Upper bound on a single JPEG frame, used for the dwMaxVideoFrameSize /
 * dwMaxVideoFrameBufferSize descriptor fields. Keep in sync with
 * JPEG_CAPTURE_MAX_BYTES in main.c.
 */
#define UVC_MAX_FRAME_SIZE (672U * 1024U)

/**
 * @brief Block until the host has committed the stream and wants frames.
 *
 * Returns once the host has issued the UVC COMMIT (SET_CUR on
 * VS_COMMIT_CONTROL) and the configuration is enabled, i.e. it is meaningful
 * to start capturing and pushing frames. Returns immediately if streaming is
 * already active.
 *
 * @param timeout_ms Maximum time to wait, or -1 to wait forever.
 * @return 0 when streaming is active, -EAGAIN on timeout.
 */
int uvc_wait_for_stream(int timeout_ms);

/**
 * @brief Send one JPEG frame to the host as a single UVC payload.
 *
 * Blocks until the whole frame has been transferred over the bulk IN endpoint
 * (which, because the host pulls at its own pace, naturally throttles the
 * caller's capture loop to host consumption). Returns early with an error if
 * streaming is torn down (USB disable / disconnect) or a chunk transfer stalls
 * past the internal timeout.
 *
 * @param jpeg Pointer to the JPEG frame.
 * @param len  Length of the JPEG frame in bytes.
 * @return 0 on success, negative errno on failure / aborted stream.
 */
int uvc_send_frame(const uint8_t *jpeg, size_t len);

/**
 * @brief Whether the host currently has the stream committed/enabled.
 */
bool uvc_is_streaming(void);

#endif /* VIDEO_USBOUT_UVC_H_ */
