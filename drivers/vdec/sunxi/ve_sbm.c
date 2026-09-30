/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Stream buffer: the compressed data on its way to the decoder.
 *
 * The data lives in one ring of video-engine-visible memory. A producer asks
 * for room at the write position, copies bytes in (the copy may wrap around
 * the end of the ring), and then submits a descriptor with the length, the
 * time stamp and the first/last part flags. Descriptors go through a
 * fixed-size FIFO with three cursors:
 *
 *   write  where the next submitted descriptor goes
 *   read   the next descriptor the decoder will look at
 *   flush  the oldest descriptor whose bytes are still in the ring
 *
 * The decoder may hand a descriptor back unread (return) and moves flush
 * forward when it has consumed the data (flush), which frees the room.
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "ve_abi.h"
#include "ve_log.h"

VE_LOG_DECLARE();

#define SBM_STREAM_FIFO_SIZE	2048
/* room kept free beyond a request so the write pointer never catches the flush point */
#define SBM_REQUEST_MARGIN	64

struct sbm_stream {
	struct ve_sbm iface; /* first: the archive sees only this */
	struct k_mutex lock;
	struct ve_sbm_config config;
	char *buffer;
	char *buffer_end; /* last byte of the ring */
	int buffer_size;
	char *write_addr;
	int valid_data_size;
	struct ve_stream_data *frames;
	int max_frames;
	int valid_frames;
	int unread_frames;
	int read_pos;
	int write_pos;
	int flush_pos;
};

static inline struct sbm_stream *to_sbm(struct ve_sbm *self)
{
	return CONTAINER_OF(self, struct sbm_stream, iface);
}

static int sbm_init(struct ve_sbm *self, struct ve_sbm_config *cfg)
{
	struct sbm_stream *s = to_sbm(self);
	char *buf;

	if (cfg == NULL || cfg->sbm_buffer_total_size <= 0) {
		LOG_ERR("invalid stream buffer configuration");
		return -1;
	}
	s->config = *cfg;

	if (cfg->vir_flag == 1) {
		buf = malloc(cfg->sbm_buffer_total_size);
	} else {
		buf = cfg->memops->palloc(cfg->sbm_buffer_total_size, cfg->ve_ops,
					  cfg->ve_ops_self);
	}
	if (buf == NULL) {
		LOG_ERR("no memory for a %d byte stream buffer", cfg->sbm_buffer_total_size);
		return -1;
	}

	s->frames = calloc(SBM_STREAM_FIFO_SIZE, sizeof(*s->frames));
	if (s->frames == NULL) {
		if (cfg->vir_flag == 1) {
			free(buf);
		} else {
			cfg->memops->pfree(buf, cfg->ve_ops, cfg->ve_ops_self);
		}
		return -1;
	}
	for (int i = 0; i < SBM_STREAM_FIFO_SIZE; i++) {
		s->frames[i].id = i;
	}

	k_mutex_init(&s->lock);
	s->buffer = buf;
	s->buffer_size = cfg->sbm_buffer_total_size;
	s->buffer_end = buf + s->buffer_size - 1;
	s->write_addr = buf;
	s->valid_data_size = 0;
	s->max_frames = SBM_STREAM_FIFO_SIZE;
	s->valid_frames = s->unread_frames = 0;
	s->read_pos = s->write_pos = s->flush_pos = 0;

	return 0;
}

static void sbm_destroy(struct ve_sbm *self)
{
	struct sbm_stream *s = to_sbm(self);

	if (s->buffer != NULL) {
		if (s->config.vir_flag == 1) {
			free(s->buffer);
		} else {
			s->config.memops->pfree(s->buffer, s->config.ve_ops, s->config.ve_ops_self);
		}
	}
	free(s->frames);
	free(s);
}

/* Throw everything away and start over at the beginning of the ring */
static void sbm_reset(struct ve_sbm *self)
{
	struct sbm_stream *s = to_sbm(self);

	k_mutex_lock(&s->lock, K_FOREVER);
	if (s->write_addr == NULL) {
		s->write_addr = s->buffer;
	}
	s->valid_data_size = 0;
	s->read_pos = s->write_pos = s->flush_pos = 0;
	s->valid_frames = s->unread_frames = 0;
	k_mutex_unlock(&s->lock);
}

static void *sbm_get_buffer_address(struct ve_sbm *self)
{
	return to_sbm(self)->buffer;
}

static int sbm_get_buffer_size(struct ve_sbm *self)
{
	return to_sbm(self)->buffer_size;
}

static int sbm_get_stream_frame_num(struct ve_sbm *self)
{
	return to_sbm(self)->valid_frames;
}

static int sbm_get_stream_data_size(struct ve_sbm *self)
{
	return to_sbm(self)->valid_data_size;
}

static char *sbm_get_buffer_write_pointer(struct ve_sbm *self)
{
	return to_sbm(self)->write_addr;
}

/* The video info attached to the oldest unread descriptor */
static void *sbm_get_buffer_data_info(struct ve_sbm *self)
{
	struct sbm_stream *s = to_sbm(self);
	void *info = NULL;

	k_mutex_lock(&s->lock, K_FOREVER);
	if (s->unread_frames != 0) {
		info = s->frames[s->read_pos].video_info;
	}
	k_mutex_unlock(&s->lock);

	return info;
}

/*
 * Room for `size` bytes at the write position. The caller handles the wrap
 * itself: the room is described by the write pointer and the requested size,
 * and the bytes may continue at the start of the ring.
 */
static int sbm_request_buffer(struct ve_sbm *self, int size, char **buf, int *buf_size)
{
	struct sbm_stream *s = to_sbm(self);
	int ret = -1;

	if (buf == NULL || buf_size == NULL) {
		return -1;
	}

	k_mutex_lock(&s->lock, K_FOREVER);
	if (s->valid_frames < s->max_frames && s->valid_data_size < s->buffer_size &&
	    size + SBM_REQUEST_MARGIN <= s->buffer_size - s->valid_data_size) {
		*buf = s->write_addr;
		*buf_size = size;
		ret = 0;
	}
	k_mutex_unlock(&s->lock);

	return ret;
}

static int sbm_add_stream(struct ve_sbm *self, struct ve_stream_data *data)
{
	struct sbm_stream *s = to_sbm(self);
	char *new_write;
	int ret = -1;

	if (data == NULL || data->data == NULL) {
		LOG_ERR("no stream data");
		return -1;
	}

	k_mutex_lock(&s->lock, K_FOREVER);
	if (s->valid_frames >= s->max_frames) {
		LOG_ERR("stream descriptor queue is full");
	} else if (data->length + s->valid_data_size > s->buffer_size) {
		LOG_ERR("stream buffer is full");
	} else {
		s->frames[s->write_pos] = *data;
		s->write_pos = (s->write_pos + 1) % s->max_frames;
		s->valid_frames++;
		s->unread_frames++;
		s->valid_data_size += data->length;

		new_write = s->write_addr + data->length;
		if (new_write > s->buffer_end) {
			new_write -= s->buffer_size;
		}
		s->write_addr = new_write;
		ret = 0;
	}
	k_mutex_unlock(&s->lock);

	return ret;
}

static struct ve_stream_data *sbm_request_stream(struct ve_sbm *self)
{
	struct sbm_stream *s = to_sbm(self);
	struct ve_stream_data *data = NULL;

	k_mutex_lock(&s->lock, K_FOREVER);
	if (s->unread_frames != 0) {
		data = &s->frames[s->read_pos];
		s->read_pos = (s->read_pos + 1) % s->max_frames;
		s->unread_frames--;
	}
	k_mutex_unlock(&s->lock);

	return data;
}

/* The decoder did not use the descriptor it requested last: make it available again */
static int sbm_return_stream(struct ve_sbm *self, struct ve_stream_data *data)
{
	struct sbm_stream *s = to_sbm(self);
	int pos, ret = -1;

	if (data == NULL) {
		return -1;
	}

	k_mutex_lock(&s->lock, K_FOREVER);
	if (s->valid_frames == 0) {
		LOG_ERR("no descriptor to return");
		goto out;
	}
	pos = (s->read_pos + s->max_frames - 1) % s->max_frames;
	if (data != &s->frames[pos]) {
		LOG_ERR("descriptors must be returned in reverse order of their request");
		goto out;
	}
	s->frames[pos] = *data;
	s->read_pos = pos;
	s->unread_frames++;
	ret = 0;
out:
	k_mutex_unlock(&s->lock);

	return ret;
}

/* The decoder is done with the oldest descriptor: its bytes in the ring are free again */
static int sbm_flush_stream(struct ve_sbm *self, struct ve_stream_data *data)
{
	struct sbm_stream *s = to_sbm(self);
	int ret = -1;

	k_mutex_lock(&s->lock, K_FOREVER);
	if (s->valid_frames == 0) {
		LOG_ERR("nothing to flush");
	} else if (data != &s->frames[s->flush_pos]) {
		LOG_ERR("descriptors must be flushed in the order they were submitted");
	} else {
		s->flush_pos = (s->flush_pos + 1) % s->max_frames;
		s->valid_frames--;
		s->valid_data_size -= data->length;
		ret = 0;
	}
	k_mutex_unlock(&s->lock);

	return ret;
}

static int sbm_set_eos(struct ve_sbm *self, int eos)
{
	ARG_UNUSED(self);
	ARG_UNUSED(eos);

	return 0;
}

struct ve_sbm *ve_sbm_create(int type)
{
	struct sbm_stream *s;

	if (type != VE_SBM_STREAM) {
		LOG_ERR("stream buffer type %d is not supported", type);
		return NULL;
	}
	s = calloc(1, sizeof(*s));
	if (s == NULL) {
		return NULL;
	}

	s->iface.init = sbm_init;
	s->iface.destroy = sbm_destroy;
	s->iface.reset = sbm_reset;
	s->iface.get_buffer_address = sbm_get_buffer_address;
	s->iface.get_buffer_size = sbm_get_buffer_size;
	s->iface.get_stream_frame_num = sbm_get_stream_frame_num;
	s->iface.get_stream_data_size = sbm_get_stream_data_size;
	s->iface.request_buffer = sbm_request_buffer;
	s->iface.add_stream = sbm_add_stream;
	s->iface.request_stream = sbm_request_stream;
	s->iface.return_stream = sbm_return_stream;
	s->iface.flush_stream = sbm_flush_stream;
	s->iface.get_buffer_write_pointer = sbm_get_buffer_write_pointer;
	s->iface.get_buffer_data_info = sbm_get_buffer_data_info;
	s->iface.set_eos = sbm_set_eos;
	s->iface.type = VE_SBM_STREAM;

	return &s->iface;
}
