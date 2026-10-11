/*
 * SPDX-License-Identifier: Apache-2.0
 */

/* Baseline JPEG decoder: software reads the markers, the engine does the rest */

#include <errno.h>
#include <stdlib.h>

#include "ve_codec.h"
#include "ve_hw.h"
#include "ve_jpeg_hw.h"
#include "ve_log.h"
#include "ve_session.h"

VE_LOG_DECLARE();

/* pictures of a motion JPEG stream: a few to decode ahead, a single picture needs two */
#define STREAM_PICTURES	4
#define IMAGE_PICTURES	2

struct jpeg {
	struct ve_session *session;
	struct ve_jpeg_frame frame;
};

static void *jpeg_open(struct ve_session *session)
{
	struct jpeg *j = calloc(1, sizeof(*j));

	if (j != NULL) {
		j->session = session;
	}

	return j;
}

static void jpeg_close(void *ctx)
{
	struct jpeg *j = ctx;

	ve_pool_destroy(j->session->pool);
	j->session->pool = NULL;
	free(j);
}

static void jpeg_reset(void *ctx)
{
	struct jpeg *j = ctx;

	if (j->session->pool != NULL) {
		ve_pool_flush(j->session->pool);
	}
}

static int make_pool(struct jpeg *j)
{
	struct ve_pool_config cfg = {
		.frames = j->session->params.image ? IMAGE_PICTURES : STREAM_PICTURES,
		.width = j->frame.width,
		.height = j->frame.height,
		.format = VE_PIX_TILED_420,
	};

	j->session->pool = ve_pool_create(&cfg);

	return j->session->pool != NULL ? 0 : -ENOMEM;
}

static enum ve_step jpeg_step(void *ctx, bool eos)
{
	struct jpeg *j = ctx;
	struct ve_session *s = j->session;
	struct ve_unit *unit;
	struct ve_picture *pic;
	struct ve_jpeg_job job = {0};
	const uint8_t *base;
	int64_t pts;
	int ret;

	ARG_UNUSED(eos);
	unit = ve_stream_next(s->stream);
	if (unit == NULL) {
		return VE_STEP_NEED_DATA;
	}
	base = ve_stream_base(s->stream);
	pts = unit->pts;

	ret = ve_jpeg_parse(unit->data, unit->length, &j->frame);
	if (ret != 0) {
		ve_stream_done(s->stream, unit);
		return ret == -ENOTSUP ? VE_STEP_UNSUPPORTED : VE_STEP_PROGRESS;
	}
	/*
	 * Only 4:2:0 pictures are made: the other sampling layouts (and gray, which decodes to a
	 * wrong luma plane) come out in tiled formats of their own that the buffers and the
	 * conversion here do not cover.
	 */
	if (j->frame.format != VE_JPEG_420) {
		ve_stream_done(s->stream, unit);
		return VE_STEP_UNSUPPORTED;
	}
	if (s->pool == NULL && make_pool(j) != 0) {
		ve_stream_unread(s->stream, unit);
		return VE_STEP_UNSUPPORTED;
	}
	/* the engine writes the tiled picture; submitting it converts it to linear NV12 */
	pic = ve_pool_acquire(s->pool);
	if (pic == NULL) {
		ve_stream_unread(s->stream, unit);
		return VE_STEP_NEED_PICTURE;
	}

	ve_hw_lock();
	/* the conversion of an earlier picture left the top register in its own mode */
	ve_hw_select(VE_HW_JPEG);
	job.frame = &j->frame;
	job.ref_y = (uint32_t)(uintptr_t)pic->plane[0];
	job.ref_c = (uint32_t)(uintptr_t)pic->plane[1];
	job.vbv_phys = (uint32_t)(uintptr_t)base;
	job.vbv_size = ve_stream_size(s->stream);
	job.data_offset = (uint32_t)(unit->data - base) + (uint32_t)j->frame.data_off;
	job.data_size = (uint32_t)j->frame.data_size;
	job.direct = 1;
	ret = ve_jpeg_hw_decode(&job);
	ve_hw_unlock();

	ve_stream_done(s->stream, unit);
	if (ret != 0) {
		ve_pool_discard(s->pool, pic);
		return VE_STEP_PROGRESS;
	}
	/*
	 * The width and height of the picture stay the aligned sizes of the buffer (the conversion
	 * to linear output uses them), the visible part is given by the edges.
	 */
	pic->left = 0;
	pic->top = 0;
	pic->right = j->frame.width;
	pic->bottom = j->frame.height;
	pic->pts = pts;
	ve_pool_submit(s->pool, pic);

	return VE_STEP_PROGRESS;
}

const struct ve_codec ve_jpeg_codec = {
	.name = "jpeg",
	.id = VE_CODEC_JPEG,
	.hw = VE_HW_JPEG,
	.open = jpeg_open,
	.close = jpeg_close,
	.reset = jpeg_reset,
	.step = jpeg_step,
};
