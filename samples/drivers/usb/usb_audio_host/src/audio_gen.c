/*
 * Copyright (c) 2025 Alif Semiconductor
 * SPDX-License-Identifier: Apache-2.0
 *
 * Audio frame generation helpers for the USB audio host sample.
 */

#include <string.h>
#include <stdint.h>

#include "audio_gen.h"

/* Stereo, 16-bit PCM at 48 kHz: one 1 ms USB frame carries up to 48 samples. */
#define AUDIO_CHANNELS		2
#define BYTES_PER_SAMPLE	2
#define MAX_SAMPLES_PER_FRAME	48
#define FRAME_BUF_BYTES	\
	(MAX_SAMPLES_PER_FRAME * BYTES_PER_SAMPLE * AUDIO_CHANNELS)

#if TEST_MODE == AUDIO_MODE_CLIP
#include "audio_clip.h"

/* Copy nsamp stereo frames from the embedded clip, wrapping at the end. */
static void fill_clip(int16_t *dst, int nsamp)
{
	static int clip_offset;
	int total = AUDIO_CLIP_NUM_FRAMES * AUDIO_CLIP_CHANNELS;

	for (int i = 0; i < nsamp * AUDIO_CHANNELS; i++) {
		dst[i] = audio_clip_data[clip_offset++];
		if (clip_offset >= total) {
			clip_offset = 0;
		}
	}
}
#elif TEST_MODE == AUDIO_MODE_TONE
/*
 * One approximate cycle of 440 Hz at 48 kHz. The true period is 109.09
 * samples, so the 48-entry table is indexed through a 109-sample phase.
 */
static const int16_t sine_table[48] = {
	0,     2139,  4240,  6270,  8192,  9974, 11585, 12998,
	14189, 15137, 15826, 16244, 16384, 16244, 15826, 15137,
	14189, 12998, 11585, 9974,  8192,  6270,  4240,  2139,
	0,    -2139, -4240, -6270, -8192, -9974,-11585,-12998,
	-14189,-15137,-15826,-16244,-16384,-16244,-15826,-15137,
	-14189,-12998,-11585, -9974, -8192, -6270, -4240, -2139,
};
#define SINE_PERIOD		109  /* samples per 440 Hz period at 48 kHz */
#define SINE_TABLE_LEN		48

/* Generate nsamp stereo frames of a continuous-phase 440 Hz tone. */
static void fill_tone(int16_t *dst, int nsamp)
{
	static int phase;

	for (int i = 0; i < nsamp; i++) {
		int idx = (phase * SINE_TABLE_LEN) / SINE_PERIOD;
		int16_t v = sine_table[idx];

		dst[i * 2] = v;
		dst[i * 2 + 1] = v;
		phase++;
		if (phase >= SINE_PERIOD) {
			phase = 0;
		}
	}
}
#endif

const uint8_t *audio_gen_next(size_t *bytes)
{
	static uint8_t frame_buf[FRAME_BUF_BYTES];
	int nsamp = MAX_SAMPLES_PER_FRAME;

#if SHED_MILLISAMPLES > 0
	/*
	 * Fractional sample shedding: keep a running remainder and drop one
	 * sample whenever it exceeds a whole sample, yielding a long-run
	 * average of (48 - SHED_MILLISAMPLES/1000) samples per frame.
	 */
	static int shed_acc;

	shed_acc += SHED_MILLISAMPLES;
	if (shed_acc >= 1000) {
		shed_acc -= 1000;
		nsamp--;
	}
#endif

	*bytes = (size_t)nsamp * BYTES_PER_SAMPLE * AUDIO_CHANNELS;

#if TEST_MODE == AUDIO_MODE_CLIP
	fill_clip((int16_t *)frame_buf, nsamp);
#elif TEST_MODE == AUDIO_MODE_TONE
	fill_tone((int16_t *)frame_buf, nsamp);
#else /* AUDIO_MODE_SILENCE (loopback does not call this) */
	memset(frame_buf, 0, *bytes);
#endif

	return frame_buf;
}
