/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * One decoder instance: it owns the stream buffer that compressed data is
 * written into, the picture pool the codec decodes into, and the codec state.
 * Calls on one session must be serialised by the caller; the engine itself is
 * shared and guarded by its own lock.
 */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_SESSION_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_SESSION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ve_codec.h"
#include "ve_pool.h"
#include "ve_stream.h"

struct ve_params {
	enum ve_codec_id codec;
	/* the picture size, needed up front for JPEG */
	int width;
	int height;
	/* stream buffer size in bytes when more than the configured default is needed */
	size_t stream_size;
	/* a single picture: the pool is made as small as it can be and nothing is reset at the end */
	bool image;
};

struct ve_session {
	const struct ve_codec *codec;
	struct ve_params params;
	struct ve_stream *stream;
	/* made by the codec once it knows the picture size */
	struct ve_pool *pool;
	/* bytes the codec may use for pictures and tables beyond the stream buffer */
	size_t memory_budget;
	void *ctx;
	/* the unit being assembled from the parts written so far */
	struct {
		uint8_t *data;
		int length;
		int64_t pts;
	} partial;
};

int ve_session_open(const struct ve_params *params, struct ve_session **session);
void ve_session_close(struct ve_session *session);

/*
 * Copy @p len bytes into the stream buffer as one unit (a JPEG or PNG file, a NAL unit);
 * -ENOSPC when there is no room yet.
 */
int ve_session_write(struct ve_session *session, const void *data, size_t len, int64_t pts);

enum ve_step ve_session_step(struct ve_session *session, bool end_of_stream);

/* Pictures ready for the consumer, and the next one (NULL when there is none) */
int ve_session_ready(const struct ve_session *session);
struct ve_picture *ve_session_take(struct ve_session *session);
void ve_session_give_back(struct ve_session *session, struct ve_picture *pic);

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_SESSION_H_ */
