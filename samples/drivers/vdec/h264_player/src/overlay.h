/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef H264_PLAYER_OVERLAY_H_
#define H264_PLAYER_OVERLAY_H_

#include <stdint.h>

/*
 * Counters the player keeps and the overlay turns into rates once a second.
 * They only ever count up; the player and the overlay thread run apart.
 */
struct play_stats {
	/* frames put on the screen */
	volatile uint32_t frames;
	/* bytes read from the card */
	volatile uint32_t sd_bytes;
	/* time the decoder calls took, in microseconds */
	volatile uint32_t decode_us;
	/* size of the picture */
	volatile uint16_t width;
	volatile uint16_t height;
	/* frames that missed their time */
	volatile uint32_t late;
};

extern struct play_stats play_stats;

/* Start the thread that draws the statistics on top of the video */
int overlay_start(void);

#endif /* H264_PLAYER_OVERLAY_H_ */
