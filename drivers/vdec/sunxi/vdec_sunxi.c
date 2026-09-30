/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
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

#include "glue/glue_mem.h"
#include "ve_abi.h"
#include "ve_decoder.h"
#include "ve_log.h"

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
	struct ve_decoder *decoder;
	struct ve_picture *picture;
};

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

static void fill_config(struct ve_stream_info *info, struct ve_vconfig *cfg, enum vdec_codec codec,
			enum vdec_format format, int width, int height, size_t len)
{
	memset(info, 0, sizeof(*info));
	memset(cfg, 0, sizeof(*cfg));

	info->width = width;
	info->height = height;
	cfg->align_stride = 16;
	cfg->thumbnail_mode = 1; /* one picture, the buffers beyond the first two are shared */
	cfg->vbv_buffer_size = MAX(len + IMAGE_VBV_MARGIN, IMAGE_MIN_VBV);

	if (codec == VDEC_CODEC_JPEG) {
		info->codec_format = VE_CODEC_MJPEG;
		cfg->output_pixel_format = format == VDEC_FORMAT_NV21 ? VE_PIX_NV21 : VE_PIX_NV12;
	} else {
		info->codec_format = VE_CODEC_PNG;
		cfg->output_pixel_format = VE_PIX_RGBA;
	}
}

static int result_to_errno(int result)
{
	switch (result) {
	case VE_RESULT_UNSUPPORTED:
		return -ENOTSUP;
	case VE_RESULT_NO_FRAME_BUFFER:
		return -ENOMEM;
	default:
		return -EIO;
	}
}

static int vdec_sunxi_decode_image(const struct device *dev, enum vdec_codec codec,
				   const void *data, size_t len, enum vdec_format format,
				   struct vdec_frame *frame)
{
	struct vdec_sunxi_data *dd = dev->data;
	struct ve_stream_info info;
	struct ve_vconfig cfg;
	struct ve_stream_data sd = {0};
	struct vdec_sunxi_frame *held;
	struct ve_decoder *dec;
	struct ve_picture *pic;
	char *buf, *ring;
	int width = 0, height = 0, buf_len, ring_len, ret;

	if (data == NULL || len == 0 || frame == NULL) {
		return -EINVAL;
	}
	if (codec == VDEC_CODEC_JPEG) {
		if (format != VDEC_FORMAT_NV12 && format != VDEC_FORMAT_NV21) {
			return -ENOTSUP;
		}
		ret = jpeg_size(data, len, &width, &height);
	} else if (codec == VDEC_CODEC_PNG) {
		if (format != VDEC_FORMAT_RGBA8888) {
			return -ENOTSUP;
		}
		ret = png_size(data, len, &width, &height);
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

	dec = ve_decoder_create();
	if (dec == NULL) {
		ret = -ENOMEM;
		goto err_claim;
	}
	fill_config(&info, &cfg, codec, format, width, height, len);
	if (ve_decoder_init(dec, &info, &cfg) != 0) {
		ret = -ENOTSUP;
		goto err_decoder;
	}

	if (ve_decoder_request_stream_buffer(dec, len, &buf, &buf_len, &ring, &ring_len, 0) != 0 ||
	    buf_len + ring_len < (int)len) {
		LOG_ERR("the picture does not fit in the stream buffer");
		ret = -ENOMEM;
		goto err_decoder;
	}
	if (buf_len >= (int)len) {
		memcpy(buf, data, len);
	} else {
		memcpy(buf, data, buf_len);
		memcpy(ring, (const uint8_t *)data + buf_len, len - buf_len);
	}
	sd.data = buf;
	sd.length = len;
	sd.is_first_part = 1;
	sd.is_last_part = 1;
	sd.valid = 1;
	sd.pts = -1;
	if (ve_decoder_submit_stream(dec, &sd, 0) != 0) {
		ret = -EIO;
		goto err_decoder;
	}

	ret = ve_decoder_decode(dec, 0, 0, 0, 0);
	if (ret != VE_RESULT_OK && ret != VE_RESULT_FRAME_DECODED &&
	    ret != VE_RESULT_KEYFRAME_DECODED && ret != VE_RESULT_NO_FRAME_BUFFER) {
		LOG_ERR("decoding failed: %d", ret);
		ret = result_to_errno(ret);
		goto err_decoder;
	}
	if (ve_decoder_valid_picture_num(dec, 0) <= 0) {
		LOG_ERR("no picture came out");
		ret = -EIO;
		goto err_decoder;
	}
	pic = ve_decoder_request_picture(dec, 0);
	if (pic == NULL) {
		ret = -EIO;
		goto err_decoder;
	}

	held = malloc(sizeof(*held));
	if (held == NULL) {
		ve_decoder_return_picture(dec, pic);
		ret = -ENOMEM;
		goto err_decoder;
	}
	held->decoder = dec;
	held->picture = pic;

	LOG_DBG("picture %d: fmt %d %dx%d stride %d crop %d,%d-%d,%d size %d", pic->id,
		pic->pixel_format, pic->width, pic->height, pic->line_stride, pic->left_offset,
		pic->top_offset, pic->right_offset, pic->bottom_offset, pic->buf_size);

	/* the engine wrote the picture behind the cache */
	ve_mem_get_ops()->flush_cache(pic->data0, pic->buf_size);

	memset(frame, 0, sizeof(*frame));
	frame->format = format;
	frame->width = pic->right_offset - pic->left_offset;
	frame->height = pic->bottom_offset - pic->top_offset;
	frame->stride[0] = pic->line_stride;
	frame->plane[0] = (uint8_t *)pic->data0 + pic->top_offset * pic->line_stride +
			  pic->left_offset * (codec == VDEC_CODEC_PNG ? 1 : 1);
	if (codec == VDEC_CODEC_JPEG) {
		frame->stride[1] = pic->line_stride;
		frame->plane[1] = (uint8_t *)pic->data1 +
				  (pic->top_offset / 2) * pic->line_stride + pic->left_offset;
	}
	frame->pts = pic->pts;
	frame->priv = held;

	return 0;

err_decoder:
	ve_decoder_destroy(dec);
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
	ve_decoder_return_picture(held->decoder, held->picture);
	ve_decoder_destroy(held->decoder);
	free(held);
	frame->priv = NULL;
	memset(frame->plane, 0, sizeof(frame->plane));

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
};

static struct vdec_sunxi_data vdec_sunxi_data0;

DEVICE_DT_INST_DEFINE(0, vdec_sunxi_init, NULL, &vdec_sunxi_data0, NULL, POST_KERNEL,
		      CONFIG_VDEC_INIT_PRIORITY, &vdec_sunxi_api);
