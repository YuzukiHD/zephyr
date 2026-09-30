/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Decoder front end on top of the engine layer of the archive: owns the stream
 * buffer, hands compressed data in and decoded pictures out. One instance
 * decodes one stream; calls on an instance must be serialised by the caller.
 */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_DECODER_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_DECODER_H_

#include "ve_abi.h"

struct ve_decoder;

struct ve_decoder *ve_decoder_create(void);
void ve_decoder_destroy(struct ve_decoder *dec);

/* Open the engine for a stream; the configuration is copied */
int ve_decoder_init(struct ve_decoder *dec, const struct ve_stream_info *info,
		    const struct ve_vconfig *cfg);
void ve_decoder_reset(struct ve_decoder *dec);
int ve_decoder_reopen(struct ve_decoder *dec, struct ve_vconfig *cfg,
		      struct ve_stream_info *info);

/* One decoding step, returns an enum ve_decode_result */
int ve_decoder_decode(struct ve_decoder *dec, int end_of_stream, int key_frame_only,
		      int drop_b_frame_if_delay, int64_t current_time_us);

/*
 * Room in the stream buffer for `size` more bytes. The room may wrap around the
 * end of the ring: then it is (buf, buf_size) followed by (ring_buf, ring_buf_size).
 */
int ve_decoder_request_stream_buffer(struct ve_decoder *dec, int size, char **buf, int *buf_size,
				     char **ring_buf, int *ring_buf_size, int stream);
/* Publish bytes written into that room; parts of one picture are glued together */
int ve_decoder_submit_stream(struct ve_decoder *dec, struct ve_stream_data *data, int stream);

void *ve_decoder_stream_buffer_address(struct ve_decoder *dec, int stream);
int ve_decoder_stream_buffer_size(struct ve_decoder *dec, int stream);
int ve_decoder_stream_data_size(struct ve_decoder *dec, int stream);
int ve_decoder_stream_frame_num(struct ve_decoder *dec, int stream);

struct ve_picture *ve_decoder_request_picture(struct ve_decoder *dec, int stream);
int ve_decoder_return_picture(struct ve_decoder *dec, struct ve_picture *pic);
struct ve_picture *ve_decoder_next_picture_info(struct ve_decoder *dec, int stream);
int ve_decoder_total_picture_buffer_num(struct ve_decoder *dec, int stream);
int ve_decoder_empty_picture_buffer_num(struct ve_decoder *dec, int stream);
int ve_decoder_valid_picture_num(struct ve_decoder *dec, int stream);

int ve_decoder_config_extra_scale(struct ve_decoder *dec, int width_th, int height_th,
				  int horizon_ratio, int vertical_ratio);
int ve_decoder_config_rotate(struct ve_decoder *dec, int degree);
int ve_decoder_set_freq(struct ve_decoder *dec, int mhz);
void ve_decoder_get_stream_info(struct ve_decoder *dec, struct ve_stream_info *info);

/* Provided by ve_sbm.c */
struct ve_sbm *ve_sbm_create(int type);

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_DECODER_H_ */
