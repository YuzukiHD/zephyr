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
	/**
	 * 32 bit little-endian words with R in the top byte and A in the lowest
	 * (so the bytes in memory are A, B, G, R)
	 */
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

/** A video stream being decoded */
struct vdec_stream;

/** vdec_stream_config.holding_frames: keep no picture */
#define VDEC_HOLD_NONE	(-1)

/** Stream parameters */
struct vdec_stream_config {
	enum vdec_codec codec;
	/** Wanted frame layout (NV12 or NV21) */
	enum vdec_format format;
	/**
	 * Size of the compressed data buffer in bytes, 0 for the default. It has to hold
	 * the largest group of NAL units handed to vdec_stream_feed() in one call.
	 */
	size_t buffer_size;
	/**
	 * The frames are not read by the CPU (the display engine, the 2D accelerator or nothing reads
	 * them): skip the cache maintenance of the picture memory.
	 */
	bool no_cache_ops;
	/**
	 * Pictures the application keeps (shown) while the engine decodes the next ones; 0 means the
	 * default of 3, VDEC_HOLD_NONE none. The engine allocates two pictures plus this many.
	 */
	int holding_frames;
	/** Picture size, JPEG streams only (the engine wants it up front) */
	uint16_t width;
	uint16_t height;
};

/** @cond INTERNAL_HIDDEN */
__subsystem struct vdec_driver_api {
	int (*decode_image)(const struct device *dev, enum vdec_codec codec, const void *data,
			    size_t len, enum vdec_format format, struct vdec_frame *frame);
	void (*frame_release)(const struct device *dev, struct vdec_frame *frame);
	int (*stream_open)(const struct device *dev, const struct vdec_stream_config *config,
			   struct vdec_stream **stream);
	int (*stream_feed)(const struct device *dev, struct vdec_stream *stream, const void *data,
			   size_t len, int64_t pts, size_t *consumed);
	int (*stream_get_frame)(const struct device *dev, struct vdec_stream *stream,
				struct vdec_frame *frame);
	int (*stream_flush)(const struct device *dev, struct vdec_stream *stream);
	void (*stream_close)(const struct device *dev, struct vdec_stream *stream);
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
 * @brief Give a frame back
 *
 * For a frame of vdec_decode_image() this also lets the decoder be used again;
 * for a frame of a stream the picture goes back to the decoder's pool.
 */
static inline void vdec_frame_release(const struct device *dev, struct vdec_frame *frame)
{
	const struct vdec_driver_api *api = (const struct vdec_driver_api *)dev->api;

	api->frame_release(dev, frame);
}

/**
 * @brief Start decoding a video stream
 *
 * The decoder is claimed by the stream until vdec_stream_close(); other
 * streams and vdec_decode_image() wait.
 *
 * @retval 0 on success
 * @retval -ENOTSUP the codec or format is not supported
 * @retval -ENOMEM no memory for the stream
 */
static inline int vdec_stream_open(const struct device *dev, const struct vdec_stream_config *config,
				   struct vdec_stream **stream)
{
	const struct vdec_driver_api *api = (const struct vdec_driver_api *)dev->api;

	return api->stream_open(dev, config, stream);
}

/**
 * @brief Hand compressed data to a stream
 *
 * H.264 data is an Annex B byte stream. @p data must hold whole NAL units; the
 * call takes as many of them as fit in the stream buffer and reports how many
 * bytes that was, so call it again with the rest after taking frames out.
 * Nothing is decoded here, vdec_stream_get_frame() does the work.
 *
 * @param pts Presentation time stamp of the data, -1 when there is none
 * @param consumed Set to the number of bytes taken
 *
 * @retval 0 at least one NAL unit was taken
 * @retval -EAGAIN the stream buffer is full: take frames out first
 * @retval -EINVAL no NAL unit in the data
 */
static inline int vdec_stream_feed(const struct device *dev, struct vdec_stream *stream,
				   const void *data, size_t len, int64_t pts, size_t *consumed)
{
	const struct vdec_driver_api *api = (const struct vdec_driver_api *)dev->api;

	return api->stream_feed(dev, stream, data, len, pts, consumed);
}

/**
 * @brief Take the next decoded frame out of a stream
 *
 * Decodes what has been fed until a frame is ready. The frame has to be given
 * back with vdec_frame_release(); the decoder holds only a few frames, so
 * release them in time.
 *
 * @retval 0 a frame was returned
 * @retval -EAGAIN more data is needed
 * @retval -EBUSY every frame is held by the application: release some
 * @retval -ENODATA the stream was flushed and everything has been delivered
 * @retval -EIO the hardware failed
 */
static inline int vdec_stream_get_frame(const struct device *dev, struct vdec_stream *stream,
					struct vdec_frame *frame)
{
	const struct vdec_driver_api *api = (const struct vdec_driver_api *)dev->api;

	return api->stream_get_frame(dev, stream, frame);
}

/**
 * @brief Tell the stream that no more data comes
 *
 * vdec_stream_get_frame() then delivers the frames still inside the decoder and
 * finally returns -ENODATA.
 */
static inline int vdec_stream_flush(const struct device *dev, struct vdec_stream *stream)
{
	const struct vdec_driver_api *api = (const struct vdec_driver_api *)dev->api;

	return api->stream_flush(dev, stream);
}

/** @brief End a stream; all its frames must have been released */
static inline void vdec_stream_close(const struct device *dev, struct vdec_stream *stream)
{
	const struct vdec_driver_api *api = (const struct vdec_driver_api *)dev->api;

	api->stream_close(dev, stream);
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_VDEC_H_ */
