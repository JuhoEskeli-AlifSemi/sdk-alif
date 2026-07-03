/*
 * Copyright (c) 2025 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 *
 * Audio frame generation helpers for the USB audio host sample.
 *
 * These generate one USB frame of stereo 16-bit PCM at 48 kHz for the
 * playback test modes. The live loopback mode does not use them - the driver
 * pumps mic IN straight to speaker OUT.
 */

#ifndef USB_AUDIO_HOST_AUDIO_GEN_H_
#define USB_AUDIO_HOST_AUDIO_GEN_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Playback source selection. This isolates the source of audio artifacts by
 * swapping the frame generator while keeping the USB path identical.
 */
#define AUDIO_MODE_CLIP		0	/* embedded arpeggio clip (audio_clip.h) */
#define AUDIO_MODE_SILENCE	1	/* all-zero frames */
#define AUDIO_MODE_TONE		2	/* steady 440 Hz tone */
#define AUDIO_MODE_LOOPBACK	3	/* live mic -> speaker (driver pumps) */

/* Active playback mode. */
#define TEST_MODE		AUDIO_MODE_LOOPBACK

/*
 * Rate adaptation for the generated playback modes (clip/silence/tone).
 * The host emits ~1008.6 USB frames/s but the DAC consumes a true 48000
 * samples/s, so a flat 48 samples/frame overflows the device FIFO (a click
 * roughly every 0.4 s). Shed a fractional sample per frame: add this value to
 * an accumulator each frame and drop one sample when it passes 1000.
 *   0   = no shed (48.000 samples/frame)
 *   410 = 47.590 samples/frame (confirmed click-free)
 * The live loopback mode needs no shedding.
 */
#define SHED_MILLISAMPLES	410

/**
 * @brief Produce the next generated playback frame
 *
 * Fills an internal buffer with one USB frame of stereo 16-bit PCM according
 * to @c TEST_MODE (clip, silence or tone), applying the rate-shedding
 * accumulator. Not used in loopback mode.
 *
 * @param bytes Set to the number of valid bytes in the returned buffer
 * @return pointer to the internal frame buffer
 */
const uint8_t *audio_gen_next(size_t *bytes);

#endif /* USB_AUDIO_HOST_AUDIO_GEN_H_ */
