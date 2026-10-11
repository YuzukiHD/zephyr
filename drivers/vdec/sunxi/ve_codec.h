/*
 * SPDX-License-Identifier: Apache-2.0
 */

/* What a codec implements for the session that drives it */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_CODEC_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_CODEC_H_

#include <stdbool.h>

#include "ve_hw.h"

struct ve_session;

enum ve_codec_id {
	VE_CODEC_JPEG,
	VE_CODEC_H264,
	VE_CODEC_PNG,
};

/* Result of one decoding step */
enum ve_step {
	VE_STEP_PROGRESS,	/* something was consumed or produced: step again */
	VE_STEP_NEED_DATA,	/* no complete unit is queued */
	VE_STEP_NEED_PICTURE,	/* no free picture to decode into */
	VE_STEP_UNSUPPORTED,	/* the data cannot be decoded */
};

struct ve_codec {
	const char *name;
	enum ve_codec_id id;
	/* decoder block of the engine, VE_HW_NONE for a decoder that runs on the CPU */
	enum ve_hw_decoder hw;
	/* private state of a session, NULL when there is no memory */
	void *(*open)(struct ve_session *session);
	void (*close)(void *ctx);
	/* back to the state before the first unit, all pictures returned */
	void (*reset)(void *ctx);
	/* consume at most one unit; end of stream tells that no more data will come */
	enum ve_step (*step)(void *ctx, bool end_of_stream);
};

extern const struct ve_codec ve_jpeg_codec;
extern const struct ve_codec ve_h264_codec;
extern const struct ve_codec ve_png_codec;

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_CODEC_H_ */
