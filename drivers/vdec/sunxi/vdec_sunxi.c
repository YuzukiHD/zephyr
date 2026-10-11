/*
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT allwinner_sunxi_ve

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/vdec.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "ve_log.h"
#include "ve_mem.h"
#include "ve_rvv.h"
#include "ve_session.h"

VE_LOG_DECLARE();

#define PNG_SIGNATURE_LEN	8
#define PNG_IHDR_END		24
#define IMAGE_MIN_VBV		(256 * 1024)

/* the stream buffer needs a little room beyond the picture itself */
#define IMAGE_VBV_MARGIN	(64 * 1024)

struct vdec_sunxi_data {
	/* claimed from a successful decode until the frame is released */
	struct k_sem claim;
};

/* what a returned frame needs to give back */
struct vdec_sunxi_frame {
	struct ve_session *session;
	struct ve_picture *picture;
	/* the stream the frame belongs to, NULL for a single picture */
	struct vdec_stream *stream;
};

struct vdec_stream {
	struct ve_session *session;
	enum vdec_format format;
	bool eos;
	int held;
	bool jpeg;		/* motion JPEG: every feed is one whole picture */
	bool no_cache_ops;	/* the CPU does not read the pictures */
};

/* give up after this many decoding steps that produced nothing */
#define STREAM_MAX_STEPS	64

/* ---- picture headers: the engine wants the size up front ---- */

static int jpeg_size(const uint8_t *p, size_t len, int *width, int *height)
{
	size_t i = 2;

	if (len < 4 || p[0] != 0xFF || p[1] != 0xD8) {
		return -EINVAL;
	}
	while (i + 4 <= len) {
		uint8_t marker;
		size_t seg;

		if (p[i] != 0xFF) {
			i++;
			continue;
		}
		marker = p[i + 1];
		if (marker == 0xFF) {
			i++;
			continue;
		}
		if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01 ||
		    marker == 0x00) {
			i += 2;
			continue;
		}
		seg = ((size_t)p[i + 2] << 8) | p[i + 3];
		/* start of frame, but not the DHT (C4), JPG (C8) and DAC (CC) markers */
		if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
		    marker != 0xCC) {
			if (i + 9 > len) {
				return -EINVAL;
			}
			*height = ((int)p[i + 5] << 8) | p[i + 6];
			*width = ((int)p[i + 7] << 8) | p[i + 8];
			return 0;
		}
		i += 2 + seg;
	}

	return -EINVAL;
}

static int png_size(const uint8_t *p, size_t len, int *width, int *height)
{
	static const uint8_t sig[PNG_SIGNATURE_LEN] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

	if (len < PNG_IHDR_END || memcmp(p, sig, sizeof(sig)) != 0) {
		return -EINVAL;
	}
	*width = (p[16] << 24) | (p[17] << 16) | (p[18] << 8) | p[19];
	*height = (p[20] << 24) | (p[21] << 16) | (p[22] << 8) | p[23];

	return 0;
}

/* ---- one picture ---- */

/* Describe a picture of the decoder as a frame; the picture is flushed for the CPU */
static struct vdec_sunxi_frame *make_frame(struct ve_session *session, struct ve_picture *pic,
					   bool rgba, enum vdec_format format,
					   struct vdec_stream *stream, struct vdec_frame *frame)
{
	struct vdec_sunxi_frame *held = malloc(sizeof(*held));

	if (held == NULL) {
		return NULL;
	}
	held->session = session;
	held->picture = pic;
	held->stream = stream;

	LOG_DBG("picture %d: fmt %d %dx%d stride %d crop %d,%d-%d,%d size %zu", pic->id,
		pic->format, pic->width, pic->height, pic->stride, pic->left, pic->top, pic->right,
		pic->bottom, pic->size);

	/* the engine wrote the picture behind the cache */
	if (stream == NULL || !stream->no_cache_ops) {
		ve_mem_flush(pic->plane[0], pic->size);
	}

	/* the decoders produce NV12: NV21 is the same picture with the chroma bytes swapped */
	if (!rgba && format == VDEC_FORMAT_NV21) {
#if VE_HAVE_RVV
		ve_rvv_swap_pairs(pic->plane[1], (size_t)((pic->bottom + 1) / 2) * pic->stride / 2);
#else
		for (int y = 0; y < (pic->bottom + 1) / 2; y++) {
			uint8_t *p = pic->plane[1] + (size_t)y * pic->stride;

			for (int x = 0; x + 1 < pic->stride; x += 2) {
				uint8_t t = p[x];

				p[x] = p[x + 1];
				p[x + 1] = t;
			}
		}
#endif
		ve_mem_flush(pic->plane[1], pic->size - (pic->plane[1] - pic->plane[0]));
	}
	memset(frame, 0, sizeof(*frame));
	frame->format = format;
	frame->width = pic->right - pic->left;
	frame->height = pic->bottom - pic->top;
	frame->stride[0] = pic->stride;
	frame->plane[0] = pic->plane[0] + pic->top * pic->stride + pic->left * (rgba ? 4 : 1);
	if (!rgba) {
		frame->stride[1] = pic->stride;
		frame->plane[1] = pic->plane[1] + (pic->top / 2) * pic->stride + pic->left;
	}
	frame->pts = pic->pts;
	frame->priv = held;

	return held;
}

static int step_to_errno(enum ve_step step)
{
	return step == VE_STEP_UNSUPPORTED ? -ENOTSUP : -EIO;
}

static int vdec_sunxi_decode_image(const struct device *dev, enum vdec_codec codec,
				   const void *data, size_t len, enum vdec_format format,
				   struct vdec_frame *frame)
{
	struct vdec_sunxi_data *dd = dev->data;
	struct ve_params params = {.image = true};
	struct vdec_sunxi_frame *held;
	struct ve_session *session;
	struct ve_picture *pic;
	enum ve_step step;
	int width = 0, height = 0, ret;

	if (data == NULL || len == 0 || frame == NULL) {
		return -EINVAL;
	}
	if (codec == VDEC_CODEC_JPEG) {
		if (format != VDEC_FORMAT_NV12 && format != VDEC_FORMAT_NV21) {
			return -ENOTSUP;
		}
		ret = jpeg_size(data, len, &width, &height);
		params.codec = VE_CODEC_JPEG;
	} else if (codec == VDEC_CODEC_PNG) {
		if (format != VDEC_FORMAT_RGBA8888) {
			return -ENOTSUP;
		}
		ret = png_size(data, len, &width, &height);
		params.codec = VE_CODEC_PNG;
	} else {
		return -ENOTSUP;
	}
	if (ret != 0) {
		LOG_ERR("not a valid picture");
		return ret;
	}
	LOG_DBG("%s %dx%d, %zu bytes", codec == VDEC_CODEC_JPEG ? "jpeg" : "png", width, height,
		len);

	k_sem_take(&dd->claim, K_FOREVER);

	params.width = width;
	params.height = height;
	params.stream_size = MAX(len + IMAGE_VBV_MARGIN, IMAGE_MIN_VBV);
	if (ve_session_open(&params, &session) != 0) {
		ret = -ENOTSUP;
		goto err_claim;
	}
	if (ve_session_write(session, data, len, -1) != 0) {
		LOG_ERR("the picture does not fit in the stream buffer");
		ret = -ENOMEM;
		goto err_session;
	}

	step = ve_session_step(session, false);
	if (step != VE_STEP_PROGRESS && step != VE_STEP_NEED_PICTURE) {
		LOG_ERR("decoding failed: %d", step);
		ret = step_to_errno(step);
		goto err_session;
	}
	if (ve_session_ready(session) <= 0) {
		LOG_ERR("no picture came out");
		ret = -EIO;
		goto err_session;
	}
	pic = ve_session_take(session);
	if (pic == NULL) {
		ret = -EIO;
		goto err_session;
	}

	held = make_frame(session, pic, codec == VDEC_CODEC_PNG, format, NULL, frame);
	if (held == NULL) {
		ve_session_give_back(session, pic);
		ret = -ENOMEM;
		goto err_session;
	}

	return 0;

err_session:
	ve_session_close(session);
err_claim:
	k_sem_give(&dd->claim);

	return ret;
}

static void vdec_sunxi_frame_release(const struct device *dev, struct vdec_frame *frame)
{
	struct vdec_sunxi_data *dd = dev->data;
	struct vdec_sunxi_frame *held = frame->priv;

	if (held == NULL) {
		return;
	}
	ve_session_give_back(held->session, held->picture);
	if (held->stream != NULL) {
		held->stream->held--;
	} else {
		/* a single picture owns its decoder */
		ve_session_close(held->session);
		k_sem_give(&dd->claim);
	}
	free(held);
	frame->priv = NULL;
	memset(frame->plane, 0, sizeof(frame->plane));
}

/* ---- video streams ---- */

static int vdec_sunxi_stream_open(const struct device *dev, const struct vdec_stream_config *config,
				  struct vdec_stream **out)
{
	struct vdec_sunxi_data *dd = dev->data;
	struct ve_params params = {0};
	struct vdec_stream *st;

	if ((config->codec != VDEC_CODEC_H264 && config->codec != VDEC_CODEC_JPEG) ||
	    (config->format != VDEC_FORMAT_NV12 && config->format != VDEC_FORMAT_NV21)) {
		return -ENOTSUP;
	}
	if (config->codec == VDEC_CODEC_JPEG && (!config->width || !config->height)) {
		return -EINVAL;
	}

	k_sem_take(&dd->claim, K_FOREVER);
	st = calloc(1, sizeof(*st));
	if (st == NULL) {
		k_sem_give(&dd->claim);
		return -ENOMEM;
	}
	st->format = config->format;
	st->jpeg = config->codec == VDEC_CODEC_JPEG;
	st->no_cache_ops = config->no_cache_ops;
	params.codec = st->jpeg ? VE_CODEC_JPEG : VE_CODEC_H264;
	if (st->jpeg) {
		params.width = config->width;
		params.height = config->height;
	}
	params.stream_size = config->buffer_size;
	if (ve_session_open(&params, &st->session) != 0) {
		free(st);
		k_sem_give(&dd->claim);

		return -ENOTSUP;
	}
	*out = st;

	return 0;
}

/* Start of the Annex B start code that ends at or contains position i (00 00 01) */
static const uint8_t *next_start_code(const uint8_t *p, const uint8_t *end)
{
#if VE_HAVE_RVV
	size_t at = ve_rvv_find_start_code(p, end - p);
	const uint8_t *q = p + at;

	if (q + 3 <= end) {
		/* a four byte start code has one more leading zero */
		return (q > p && q[-1] == 0) ? q - 1 : q;
	}
#else
	for (const uint8_t *q = p; q + 3 <= end; q++) {
		if (q[0] == 0 && q[1] == 0 && q[2] == 1) {
			/* a four byte start code has one more leading zero */
			return (q > p && q[-1] == 0) ? q - 1 : q;
		}
	}
#endif

	return NULL;
}

static int vdec_sunxi_stream_feed(const struct device *dev, struct vdec_stream *st,
				  const void *data, size_t len, int64_t pts, size_t *consumed)
{
	const uint8_t *p = data, *end = p + len, *nal, *next;
	size_t taken = 0;

	*consumed = 0;
	if (st->jpeg) {
		int ret = ve_session_write(st->session, data, len, pts);

		if (ret != 0) {
			return ret == -ENOSPC ? -EAGAIN : -EIO;
		}
		*consumed = len;

		return 0;
	}
	nal = next_start_code(p, end);
	if (nal != p) {
		return -EINVAL;
	}

	while (nal != NULL) {
		size_t nal_len;
		int ret;

		/* the NAL unit ends where the next start code begins */
		next = next_start_code(nal + 3, end);
		nal_len = (next != NULL ? next : end) - nal;

		ret = ve_session_write(st->session, nal, nal_len, pts);
		if (ret == -ENOSPC) {
			break;
		} else if (ret != 0) {
			return -EIO;
		}
		taken += nal_len;
		nal = next;
	}
	*consumed = taken;

	return taken > 0 ? 0 : -EAGAIN;
}

static int vdec_sunxi_stream_get_frame(const struct device *dev, struct vdec_stream *st,
				       struct vdec_frame *frame)
{
	for (int step = 0; step < STREAM_MAX_STEPS; step++) {
		struct ve_picture *pic;

		if (ve_session_ready(st->session) > 0) {
			pic = ve_session_take(st->session);
			if (pic != NULL && make_frame(st->session, pic, false, st->format, st, frame)) {
				st->held++;
				return 0;
			}
			if (pic != NULL) {
				ve_session_give_back(st->session, pic);
			}
			return -ENOMEM;
		}

		switch (ve_session_step(st->session, st->eos)) {
		case VE_STEP_PROGRESS:
			break;
		case VE_STEP_NEED_PICTURE:
			return -EBUSY;
		case VE_STEP_NEED_DATA:
			if (ve_session_ready(st->session) > 0) {
				break;
			}
			return st->eos ? -ENODATA : -EAGAIN;
		default:
			LOG_ERR("decoding failed");
			return -EIO;
		}
	}

	return -EAGAIN;
}

static int vdec_sunxi_stream_flush(const struct device *dev, struct vdec_stream *st)
{
	st->eos = true;

	return 0;
}

static void vdec_sunxi_stream_close(const struct device *dev, struct vdec_stream *st)
{
	struct vdec_sunxi_data *dd = dev->data;

	if (st->held != 0) {
		LOG_WRN("stream closed with %d frames still held", st->held);
	}
	ve_session_close(st->session);
	free(st);
	k_sem_give(&dd->claim);
}

static int vdec_sunxi_init(const struct device *dev)
{
	struct vdec_sunxi_data *dd = dev->data;

	k_sem_init(&dd->claim, 1, 1);

	return 0;
}

static const struct vdec_driver_api vdec_sunxi_api = {
	.decode_image = vdec_sunxi_decode_image,
	.frame_release = vdec_sunxi_frame_release,
	.stream_open = vdec_sunxi_stream_open,
	.stream_feed = vdec_sunxi_stream_feed,
	.stream_get_frame = vdec_sunxi_stream_get_frame,
	.stream_flush = vdec_sunxi_stream_flush,
	.stream_close = vdec_sunxi_stream_close,
};

static struct vdec_sunxi_data vdec_sunxi_data0;

DEVICE_DT_INST_DEFINE(0, vdec_sunxi_init, NULL, &vdec_sunxi_data0, NULL, POST_KERNEL,
		      CONFIG_VDEC_INIT_PRIORITY, &vdec_sunxi_api);
