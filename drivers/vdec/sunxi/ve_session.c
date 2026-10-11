/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "ve_hw.h"
#include "ve_log.h"
#include "ve_mem.h"
#include "ve_session.h"

VE_LOG_DECLARE();

#define MAX_WIDTH	6144
#define MAX_HEIGHT	3840
/* A JPEG start/end marker is searched in at most this many bytes of a part */
#define JPEG_MARKER_SCAN	512

/* plenty of data queued but not enough for a picture: keep feeding */
#define NEED_DATA_LIMIT		(512 * 1024)

static const struct ve_codec *const codecs[] = {
#ifdef CONFIG_VDEC_SUNXI_JPEG
	&ve_jpeg_codec,
#endif
#ifdef CONFIG_VDEC_SUNXI_H264
	&ve_h264_codec,
#endif
#ifdef CONFIG_VDEC_SUNXI_PNG
	&ve_png_codec,
#endif
};

static const struct ve_codec *find_codec(enum ve_codec_id id)
{
	for (size_t i = 0; i < ARRAY_SIZE(codecs); i++) {
		if (codecs[i]->id == id) {
			return codecs[i];
		}
	}

	return NULL;
}

static void partial_clear(struct ve_session *s)
{
	s->partial.data = NULL;
	s->partial.length = 0;
	s->partial.pts = -1;
}

/* ---- open and close ---- */

/* The decoder blocks are routed and reset around everything a hardware codec does */
static void engine_enter(const struct ve_codec *codec)
{
	if (codec->hw != VE_HW_NONE) {
		ve_hw_lock();
		ve_hw_select(codec->hw);
	}
}

static void engine_leave(const struct ve_codec *codec)
{
	if (codec->hw != VE_HW_NONE) {
		ve_hw_idle();
		ve_hw_unlock();
	}
}

int ve_session_open(const struct ve_params *params, struct ve_session **out)
{
	const struct ve_codec *codec = find_codec(params->codec);
	struct ve_session *s;
	size_t stream_size = CONFIG_VDEC_SUNXI_STREAM_BUFFER_KB * 1024;

	if (codec == NULL) {
		return -ENOTSUP;
	}
	if (params->width > MAX_WIDTH || params->height > MAX_HEIGHT) {
		LOG_ERR("%dx%d is not supported", params->width, params->height);
		return -ENOTSUP;
	}
	/* a caller that knows the size of its data can ask for more room */
	stream_size = MAX(stream_size, params->stream_size);
	if (stream_size > CONFIG_VDEC_SUNXI_MAX_MEMORY_KB * 1024U) {
		LOG_ERR("not enough memory to decode");
		return -ENOMEM;
	}

	s = calloc(1, sizeof(*s));
	if (s == NULL) {
		return -ENOMEM;
	}
	s->codec = codec;
	s->params = *params;
	s->memory_budget = CONFIG_VDEC_SUNXI_MAX_MEMORY_KB * 1024U - stream_size;
	partial_clear(s);

	if (codec->hw != VE_HW_NONE && ve_hw_get() != 0) {
		free(s);
		return -EIO;
	}
	s->stream = ve_stream_create(stream_size);
	if (s->stream == NULL) {
		goto err;
	}
	engine_enter(codec);
	if (codec->hw != VE_HW_NONE) {
		ve_hw_reset(codec->hw, params->width);
	}
	s->ctx = codec->open(s);
	engine_leave(codec);
	if (s->ctx == NULL) {
		LOG_ERR("cannot open the %s decoder", codec->name);
		goto err;
	}
	*out = s;

	return 0;
err:
	ve_stream_destroy(s->stream);
	if (codec->hw != VE_HW_NONE) {
		ve_hw_put();
	}
	free(s);

	return -ENOMEM;
}

void ve_session_close(struct ve_session *s)
{
	if (s == NULL) {
		return;
	}
	engine_enter(s->codec);
	s->codec->close(s->ctx);
	engine_leave(s->codec);
	ve_stream_destroy(s->stream);
	if (s->codec->hw != VE_HW_NONE) {
		ve_hw_put();
	}
	free(s);
}

/* ---- compressed data in ---- */

/* The bytes just written must be visible to the engine, which reads memory directly */
static void flush_unit(const struct ve_session *s, const uint8_t *data, int length)
{
	const uint8_t *base = ve_stream_base(s->stream);
	const uint8_t *end = base + ve_stream_size(s->stream);

	if (data + length <= end) {
		ve_mem_flush(data, length);
	} else {
		int first = end - data;

		ve_mem_flush(data, first);
		ve_mem_flush(base, length - first);
	}
}

/* Copy @p len bytes at @p src, which may wrap around the end of the ring, into @p dst */
static void read_ring(const struct ve_stream *stream, uint8_t *dst, const uint8_t *src, int len)
{
	const uint8_t *base = ve_stream_base(stream);
	const uint8_t *end = base + ve_stream_size(stream);

	if (src + len <= end) {
		memcpy(dst, src, len);
	} else {
		int first = end - src;

		memcpy(dst, src, first);
		memcpy(dst + first, base, len - first);
	}
}

/*
 * A JPEG picture starts with the SOI marker (FFD8) and ends with EOI (FFD9). The caller does not
 * say where a picture begins or ends, so find out from the data. Fill bytes (00/FF) in front of a
 * marker are skipped.
 */
static void jpeg_mark_parts(const struct ve_session *s, const uint8_t *data, int length,
			    bool *first, bool *last)
{
	uint8_t buf[JPEG_MARKER_SCAN];
	int check = MIN(length, JPEG_MARKER_SCAN);
	uint16_t code = 0;
	int i;

	*first = *last = false;

	read_ring(s->stream, buf, data, check);
	for (i = 0; i < check; i++) {
		code = (code << 8) | buf[i];
		if (code == 0xFFD8) {
			*first = true;
			break;
		} else if (code != 0 && code != 0xFFFF && code != 0x00FF) {
			break;
		}
	}

	{
		const uint8_t *tail = data + length - check;
		const uint8_t *base = ve_stream_base(s->stream);
		int size = ve_stream_size(s->stream);

		if (tail < base) {
			tail += size;
		} else if (tail >= base + size) {
			tail -= size;
		}
		read_ring(s->stream, buf, tail, check);
	}
	code = 0;
	for (i = check - 1; i >= 0; i--) {
		code = (code >> 8) | ((uint16_t)buf[i] << 8);
		if (code == 0xFFD9) {
			*last = true;
			break;
		} else if (code != 0 && code != 0xFFFF && code != 0x00FF) {
			if (i > 0 && ((buf[i - 1] << 8) | buf[i]) == 0xFFD9) {
				*last = true;
				break;
			}
		}
	}
}

/* Queue the unit assembled so far */
static void partial_add(struct ve_session *s)
{
	struct ve_unit unit = {
		.data = s->partial.data,
		.length = s->partial.length,
		.pts = s->partial.pts,
	};

	flush_unit(s, unit.data, unit.length);
	(void)ve_stream_add(s->stream, &unit);
}

/*
 * The bytes of one part sit at @p data. A motion JPEG part is a whole picture, or a piece of one
 * when the marker at its start or end is missing: the pieces are glued together.
 */
static void commit(struct ve_session *s, uint8_t *data, int length, int64_t pts)
{
	bool first = true, last = true;

	if (s->codec->id == VE_CODEC_JPEG) {
		jpeg_mark_parts(s, data, length, &first, &last);
	}
	if (first) {
		/* the previous picture never got its last part: hand over what there is */
		if (s->partial.length != 0) {
			partial_add(s);
		}
		s->partial.data = data;
		s->partial.length = length;
		s->partial.pts = pts;
	} else {
		if (s->partial.data == NULL) {
			s->partial.data = data;
		}
		s->partial.length += length;
		if (s->partial.pts == -1 && pts != -1) {
			s->partial.pts = pts;
		}
	}
	if (last) {
		if (s->partial.data != NULL && s->partial.length != 0) {
			partial_add(s);
		}
		partial_clear(s);
	}
}

int ve_session_write(struct ve_session *s, const void *data, size_t len, int64_t pts)
{
	const uint8_t *src = data;
	uint8_t *mem, *start;
	const uint8_t *base = ve_stream_base(s->stream);
	int size = ve_stream_size(s->stream);
	int already = s->partial.length;

	if (len == 0) {
		return -EINVAL;
	}
	if (ve_stream_room(s->stream, len + already, &mem) != 0) {
		return -ENOSPC;
	}
	start = mem + already;
	if (start >= base + size) {
		start -= size;
	}
	if (start + len <= base + size) {
		memcpy(start, src, len);
	} else {
		size_t first = base + size - start;

		memcpy(start, src, first);
		memcpy((uint8_t *)base, src + first, len - first);
	}
	commit(s, start, len, pts);

	return 0;
}

/* ---- decoding ---- */

enum ve_step ve_session_step(struct ve_session *s, bool eos)
{
	enum ve_step step;

	/*
	 * The engine writes tiled pictures and converts each one into a second, linear picture, so a
	 * decoding step needs two free pictures.
	 */
	if (s->codec->id != VE_CODEC_PNG && s->pool != NULL && ve_pool_free(s->pool) < 2) {
		return VE_STEP_NEED_PICTURE;
	}

	engine_enter(s->codec);
	step = s->codec->step(s->ctx, eos);
	engine_leave(s->codec);

	if (!s->params.image && eos && step == VE_STEP_NEED_DATA) {
		/* the end: give the buffers back, the stream can be started again */
		engine_enter(s->codec);
		s->codec->reset(s->ctx);
		engine_leave(s->codec);
		ve_stream_reset(s->stream);
		partial_clear(s);
	}
	if (step == VE_STEP_NEED_DATA && ve_stream_used(s->stream) > NEED_DATA_LIMIT) {
		step = VE_STEP_PROGRESS;
	}

	return step;
}

/* ---- pictures out ---- */

int ve_session_ready(const struct ve_session *s)
{
	return s->pool != NULL ? ve_pool_ready(s->pool) : 0;
}

struct ve_picture *ve_session_take(struct ve_session *s)
{
	return s->pool != NULL ? ve_pool_take(s->pool) : NULL;
}

void ve_session_give_back(struct ve_session *s, struct ve_picture *pic)
{
	if (s->pool != NULL && pic != NULL) {
		(void)ve_pool_give_back(s->pool, pic);
	}
}
