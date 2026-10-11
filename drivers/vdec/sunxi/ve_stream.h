/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Stream buffer: the compressed data on its way to the decoder.
 *
 * The data lives in one ring of memory the engine can read. A producer asks for
 * room at the write position, copies bytes in (the copy may wrap around the end
 * of the ring), and then adds a unit with the length and the time stamp. Units
 * go through a fixed-size queue with three cursors:
 *
 *   write  where the next unit goes
 *   read   the next unit the decoder will look at
 *   done   the oldest unit whose bytes are still in the ring
 *
 * The decoder may hand a unit back unread and moves the done cursor forward
 * when it has consumed the data, which frees the room.
 */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_STREAM_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_STREAM_H_

#include <stddef.h>
#include <stdint.h>

/* One piece of compressed data, a NAL unit or a picture; it may wrap around the end of the ring */
struct ve_unit {
	uint8_t *data;
	int length;
	int64_t pts;
};

struct ve_stream;

struct ve_stream *ve_stream_create(size_t size);
void ve_stream_destroy(struct ve_stream *stream);
/* Throw everything away and start over at the beginning of the ring */
void ve_stream_reset(struct ve_stream *stream);

uint8_t *ve_stream_base(const struct ve_stream *stream);
size_t ve_stream_size(const struct ve_stream *stream);
/* Bytes and units in the ring */
int ve_stream_used(const struct ve_stream *stream);
int ve_stream_units(const struct ve_stream *stream);

/*
 * Room for @p size bytes at the write position: sets @p start and returns 0, or returns -ENOSPC.
 * The room continues at the start of the ring when it runs past the end.
 */
int ve_stream_room(struct ve_stream *stream, int size, uint8_t **start);
/* Queue a unit whose bytes were written at the write position */
int ve_stream_add(struct ve_stream *stream, const struct ve_unit *unit);

/* The oldest unit not looked at yet, NULL when there is none */
struct ve_unit *ve_stream_next(struct ve_stream *stream);
/* Make the unit returned last by ve_stream_next() available again */
int ve_stream_unread(struct ve_stream *stream, struct ve_unit *unit);
/* The oldest unit is consumed: its bytes in the ring are free again */
int ve_stream_done(struct ve_stream *stream, struct ve_unit *unit);

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_STREAM_H_ */
