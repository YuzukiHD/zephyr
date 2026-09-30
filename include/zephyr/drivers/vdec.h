/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Hardware image and video decoder API
 *
 * A decoder turns compressed pictures (JPEG, PNG) into raw frames without
 * the CPU touching the pixels. The frame memory belongs to the decoder and
 * is handed out without a copy: it stays valid, and the decoder stays
 * claimed, until vdec_frame_release().
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_VDEC_H_
#define ZEPHYR_INCLUDE_DRIVERS_VDEC_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Compressed formats a decoder may support */
enum vdec_codec {
	VDEC_CODEC_JPEG = 1,
	VDEC_CODEC_PNG,
	VDEC_CODEC_H264,
};

/** Raw frame layouts */
enum vdec_format {
	/** 8 bit luma plane followed by an interleaved Cb/Cr plane at half resolution */
	VDEC_FORMAT_NV12 = 1,
	/** 8 bit luma plane followed by an interleaved Cr/Cb plane at half resolution */
	VDEC_FORMAT_NV21,
	/** 8 bit R, G, B, A bytes per pixel in memory order */
	VDEC_FORMAT_RGBA8888,
};

/** A decoded frame */
struct vdec_frame {
	enum vdec_format format;
	/** Visible size in pixels */
	uint16_t width;
	uint16_t height;
	/** Pointers to the first visible pixel of each plane (unused planes are NULL) */
	uint8_t *plane[2];
	/** Bytes from one row of a plane to the next */
	uint16_t stride[2];
	/** Presentation time stamp given with the data, -1 when there is none */
	int64_t pts;
	/** Decoder private, do not touch */
	void *priv;
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct vdec_driver_api {
	int (*decode_image)(const struct device *dev, enum vdec_codec codec, const void *data,
			    size_t len, enum vdec_format format, struct vdec_frame *frame);
	void (*frame_release)(const struct device *dev, struct vdec_frame *frame);
};
/** @endcond */

/**
 * @brief Decode one JPEG or PNG picture
 *
 * Blocks until the picture is decoded. When the function succeeds the decoder
 * is claimed by the returned frame: other calls wait until it is released.
 *
 * @param dev Decoder device
 * @param codec VDEC_CODEC_JPEG or VDEC_CODEC_PNG
 * @param data Compressed picture, not kept after the call returns
 * @param len Size of @p data
 * @param format Wanted frame layout (JPEG: NV12 or NV21, PNG: RGBA8888)
 * @param frame Filled with the decoded frame
 *
 * @retval 0 on success
 * @retval -ENOTSUP the codec, format or picture is not supported
 * @retval -EINVAL bad arguments or a damaged picture
 * @retval -ENOMEM the picture does not fit in the decoder memory
 * @retval -EIO the hardware failed to decode it
 */
static inline int vdec_decode_image(const struct device *dev, enum vdec_codec codec,
				    const void *data, size_t len, enum vdec_format format,
				    struct vdec_frame *frame)
{
	const struct vdec_driver_api *api = (const struct vdec_driver_api *)dev->api;

	return api->decode_image(dev, codec, data, len, format, frame);
}

/**
 * @brief Give a frame back and let the decoder be used again
 */
static inline void vdec_frame_release(const struct device *dev, struct vdec_frame *frame)
{
	const struct vdec_driver_api *api = (const struct vdec_driver_api *)dev->api;

	api->frame_release(dev, frame);
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_VDEC_H_ */
