/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "ve_log.h"
#include "ve_mem.h"
#include "ve_stream.h"

VE_LOG_DECLARE();

#define MAX_UNITS	2048
/* room kept free beyond a request so the write pointer never catches the done point */
#define REQUEST_MARGIN	64

struct ve_stream {
	struct k_mutex lock;
	uint8_t *buffer;
	uint8_t *buffer_end;	/* last byte of the ring */
	int buffer_size;
	uint8_t *write_addr;
	int used;
	struct ve_unit *units;
	int valid;		/* units in the ring */
	int unread;		/* units the decoder did not look at yet */
	int read_pos;
	int write_pos;
	int done_pos;
};

struct ve_stream *ve_stream_create(size_t size)
{
	struct ve_stream *s;

	if (size == 0) {
		LOG_ERR("invalid stream buffer size");
		return NULL;
	}
	s = calloc(1, sizeof(*s));
	if (s == NULL) {
		return NULL;
	}
	s->buffer = ve_mem_alloc(size);
	s->units = calloc(MAX_UNITS, sizeof(*s->units));
	if (s->buffer == NULL || s->units == NULL) {
		LOG_ERR("no memory for a %zu byte stream buffer", size);
		ve_stream_destroy(s);
		return NULL;
	}
	k_mutex_init(&s->lock);
	s->buffer_size = size;
	s->buffer_end = s->buffer + size - 1;
	s->write_addr = s->buffer;

	return s;
}

void ve_stream_destroy(struct ve_stream *s)
{
	if (s == NULL) {
		return;
	}
	if (s->buffer != NULL) {
		ve_mem_free(s->buffer);
	}
	free(s->units);
	free(s);
}

void ve_stream_reset(struct ve_stream *s)
{
	k_mutex_lock(&s->lock, K_FOREVER);
	s->write_addr = s->buffer;
	s->used = 0;
	s->read_pos = s->write_pos = s->done_pos = 0;
	s->valid = s->unread = 0;
	k_mutex_unlock(&s->lock);
}

uint8_t *ve_stream_base(const struct ve_stream *s)
{
	return s->buffer;
}

size_t ve_stream_size(const struct ve_stream *s)
{
	return s->buffer_size;
}

int ve_stream_used(const struct ve_stream *s)
{
	return s->used;
}

int ve_stream_units(const struct ve_stream *s)
{
	return s->valid;
}

int ve_stream_room(struct ve_stream *s, int size, uint8_t **start)
{
	int ret = -ENOSPC;

	k_mutex_lock(&s->lock, K_FOREVER);
	if (s->valid < MAX_UNITS && s->used < s->buffer_size &&
	    size + REQUEST_MARGIN <= s->buffer_size - s->used) {
		*start = s->write_addr;
		ret = 0;
	}
	k_mutex_unlock(&s->lock);

	return ret;
}

int ve_stream_add(struct ve_stream *s, const struct ve_unit *unit)
{
	int ret = -ENOSPC;

	k_mutex_lock(&s->lock, K_FOREVER);
	if (s->valid >= MAX_UNITS) {
		LOG_ERR("stream unit queue is full");
	} else if (unit->length + s->used > s->buffer_size) {
		LOG_ERR("stream buffer is full");
	} else {
		uint8_t *next = s->write_addr + unit->length;

		s->units[s->write_pos] = *unit;
		s->write_pos = (s->write_pos + 1) % MAX_UNITS;
		s->valid++;
		s->unread++;
		s->used += unit->length;
		if (next > s->buffer_end) {
			next -= s->buffer_size;
		}
		s->write_addr = next;
		ret = 0;
	}
	k_mutex_unlock(&s->lock);

	return ret;
}

struct ve_unit *ve_stream_next(struct ve_stream *s)
{
	struct ve_unit *unit = NULL;

	k_mutex_lock(&s->lock, K_FOREVER);
	if (s->unread != 0) {
		unit = &s->units[s->read_pos];
		s->read_pos = (s->read_pos + 1) % MAX_UNITS;
		s->unread--;
	}
	k_mutex_unlock(&s->lock);

	return unit;
}

int ve_stream_unread(struct ve_stream *s, struct ve_unit *unit)
{
	int pos, ret = -EINVAL;

	k_mutex_lock(&s->lock, K_FOREVER);
	pos = (s->read_pos + MAX_UNITS - 1) % MAX_UNITS;
	if (s->valid == 0 || unit != &s->units[pos]) {
		LOG_ERR("units must be given back in reverse order of their request");
	} else {
		s->read_pos = pos;
		s->unread++;
		ret = 0;
	}
	k_mutex_unlock(&s->lock);

	return ret;
}

int ve_stream_done(struct ve_stream *s, struct ve_unit *unit)
{
	int ret = -EINVAL;

	k_mutex_lock(&s->lock, K_FOREVER);
	if (s->valid == 0) {
		LOG_ERR("nothing to finish");
	} else if (unit != &s->units[s->done_pos]) {
		LOG_ERR("units must be finished in the order they were added");
	} else {
		s->done_pos = (s->done_pos + 1) % MAX_UNITS;
		s->valid--;
		s->used -= unit->length;
		ret = 0;
	}
	k_mutex_unlock(&s->lock);

	return ret;
}
