/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/mp4.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d))

#define STSD_MAX	1024
#define MAX_DEPTH	8

struct box {
	uint32_t type;
	uint64_t payload;	/* offset of the content */
	uint64_t end;
};

static int rd(const struct mp4 *m, uint64_t off, void *buf, size_t len)
{
	if (off + len > m->size) {
		return -EINVAL;
	}

	return m->read(m->ctx, off, buf, len);
}

static int rd_u32(const struct mp4 *m, uint64_t off, uint32_t *v)
{
	uint8_t b[4];
	int ret = rd(m, off, b, 4);

	*v = sys_get_be32(b);

	return ret;
}

/* Header of the box at @p off inside a parent that ends at @p limit */
static int box_at(const struct mp4 *m, uint64_t off, uint64_t limit, struct box *b)
{
	uint8_t h[16];
	uint64_t size;
	int ret;

	if (off + 8 > limit) {
		return -ENOENT;
	}
	ret = rd(m, off, h, 8);
	if (ret != 0) {
		return ret;
	}
	size = sys_get_be32(h);
	b->type = sys_get_be32(h + 4);
	b->payload = off + 8;
	if (size == 1U) {
		ret = rd(m, off + 8, h + 8, 8);
		if (ret != 0) {
			return ret;
		}
		size = ((uint64_t)sys_get_be32(h + 8) << 32) | sys_get_be32(h + 12);
		b->payload = off + 16;
	} else if (size == 0U) {
		size = limit - off;
	}
	if (size < (b->payload - off) || off + size > limit) {
		return -EINVAL;
	}
	b->end = off + size;

	return 0;
}

/* Reads @p count 32-bit big endian values into a fresh array */
static int read_array(const struct mp4 *m, uint64_t off, uint32_t count, uint32_t **out)
{
	uint32_t *a;
	int ret;

	if (count == 0U) {
		*out = NULL;
		return 0;
	}
	a = malloc((size_t)count * 4U);
	if (a == NULL) {
		return -ENOMEM;
	}
	ret = rd(m, off, a, (size_t)count * 4U);
	if (ret != 0) {
		free(a);
		return ret;
	}
	for (uint32_t i = 0; i < count; i++) {
		a[i] = sys_be32_to_cpu(a[i]);
	}
	*out = a;

	return 0;
}

static void track_free(struct mp4_track *t)
{
	free(t->extra);
	free(t->sizes);
	free(t->chunk_offset);
	free(t->stsc);
	free(t->stts);
	free(t->ctts);
	free(t->stss);
	memset(t, 0, sizeof(*t));
}

/* Length field of an MPEG-4 descriptor, 7 bits per byte */
static uint32_t desc_len(const uint8_t **p, const uint8_t *end)
{
	uint32_t len = 0;

	for (int i = 0; i < 4 && *p < end; i++) {
		uint8_t c = *(*p)++;

		len = (len << 7) | (c & 0x7f);
		if ((c & 0x80) == 0U) {
			break;
		}
	}

	return len;
}

/* AudioSpecificConfig out of the elementary stream descriptor of an esds box */
static int parse_esds(struct mp4_track *t, const uint8_t *p, const uint8_t *end)
{
	p += 4;	/* version and flags */
	while (p < end) {
		uint8_t tag = *p++;
		uint32_t len = desc_len(&p, end);

		if (tag == 0x03) {
			uint8_t flags;

			if (p + 3 > end) {
				return -EINVAL;
			}
			flags = p[2];
			p += 3;
			if ((flags & 0x80) != 0U) {
				p += 2;
			}
			if ((flags & 0x40) != 0U && p < end) {
				p += 1 + *p;
			}
			if ((flags & 0x20) != 0U) {
				p += 2;
			}
		} else if (tag == 0x04) {
			if (p + 13 > end) {
				return -EINVAL;
			}
			p += 13;
		} else if (tag == 0x05) {
			if (len == 0U || p + len > end) {
				return -EINVAL;
			}
			t->extra = malloc(len);
			if (t->extra == NULL) {
				return -ENOMEM;
			}
			memcpy(t->extra, p, len);
			t->extra_len = len;
			return 0;
		} else {
			p += len;
		}
	}

	return -ENOENT;
}

static int parse_stsd(const struct mp4 *m, const struct box *b, struct mp4_track *t)
{
	uint8_t buf[STSD_MAX];
	uint32_t esize, type;
	size_t avail = b->end - (b->payload + 8);
	size_t n = MIN(avail, sizeof(buf));
	size_t hdr, off;
	int ret;

	if (avail < 16U) {
		return -EINVAL;
	}
	ret = rd(m, b->payload + 8, buf, n);
	if (ret != 0) {
		return ret;
	}
	esize = sys_get_be32(buf);
	type = sys_get_be32(buf + 4);
	if (esize < 16U) {
		return -EINVAL;
	}
	n = MIN(n, esize);

	if (type == FOURCC('a', 'v', 'c', '1') || type == FOURCC('a', 'v', 'c', '3')) {
		t->codec = MP4_CODEC_H264;
		hdr = 86;
		if (n < hdr) {
			return -EINVAL;
		}
		t->width = sys_get_be16(buf + 32);
		t->height = sys_get_be16(buf + 34);
	} else if (type == FOURCC('m', 'p', '4', 'a')) {
		uint16_t version = sys_get_be16(buf + 16);

		t->codec = MP4_CODEC_AAC;
		hdr = 36;
		if (version == 1U) {
			hdr += 16;
		} else if (version == 2U) {
			hdr += 36;
		}
		if (n < hdr) {
			return -EINVAL;
		}
		t->channels = sys_get_be16(buf + 24);
		t->sample_rate = sys_get_be16(buf + 32);
	} else {
		return 0;	/* another codec, the track is ignored */
	}

	for (off = hdr; off + 8 <= n;) {
		uint32_t csize = sys_get_be32(buf + off);
		uint32_t ctype = sys_get_be32(buf + off + 4);

		if (csize < 8U || off + csize > n) {
			break;
		}
		if (t->codec == MP4_CODEC_H264 && ctype == FOURCC('a', 'v', 'c', 'C')) {
			t->extra = malloc(csize - 8U);
			if (t->extra == NULL) {
				return -ENOMEM;
			}
			memcpy(t->extra, buf + off + 8, csize - 8U);
			t->extra_len = csize - 8U;
		} else if (t->codec == MP4_CODEC_AAC && ctype == FOURCC('e', 's', 'd', 's')) {
			ret = parse_esds(t, buf + off + 8, buf + off + csize);
			if (ret != 0) {
				return ret;
			}
		}
		off += csize;
	}

	return t->extra != NULL ? 0 : -ENOENT;
}

static int parse_stbl_box(const struct mp4 *m, const struct box *b, struct mp4_track *t)
{
	uint32_t count, size;
	int ret = 0;

	switch (b->type) {
	case FOURCC('s', 't', 's', 'd'):
		ret = parse_stsd(m, b, t);
		break;
	case FOURCC('s', 't', 't', 's'):
	case FOURCC('c', 't', 't', 's'):
		ret = rd_u32(m, b->payload + 4, &count);
		if (ret == 0 && b->payload + 8 + (uint64_t)count * 8U > b->end) {
			ret = -EINVAL;
		}
		if (ret == 0) {
			uint32_t **arr = b->type == FOURCC('s', 't', 't', 's') ? &t->stts : &t->ctts;
			uint32_t *cnt = b->type == FOURCC('s', 't', 't', 's') ? &t->stts_count :
									       &t->ctts_count;

			ret = read_array(m, b->payload + 8, count * 2U, arr);
			*cnt = count;
		}
		break;
	case FOURCC('s', 't', 's', 'z'):
		ret = rd_u32(m, b->payload + 4, &size);
		if (ret == 0) {
			ret = rd_u32(m, b->payload + 8, &count);
		}
		if (ret != 0) {
			break;
		}
		t->sample_count = count;
		t->fixed_size = size;
		if (size == 0U) {
			if (b->payload + 12 + (uint64_t)count * 4U > b->end) {
				ret = -EINVAL;
				break;
			}
			ret = read_array(m, b->payload + 12, count, &t->sizes);
			for (uint32_t i = 0; ret == 0 && i < count; i++) {
				t->max_sample = MAX(t->max_sample, t->sizes[i]);
			}
		} else {
			t->max_sample = size;
		}
		break;
	case FOURCC('s', 't', 'c', 'o'):
		ret = rd_u32(m, b->payload + 4, &count);
		if (ret == 0 && b->payload + 8 + (uint64_t)count * 4U > b->end) {
			ret = -EINVAL;
		}
		if (ret == 0) {
			ret = read_array(m, b->payload + 8, count, &t->chunk_offset);
			t->chunk_count = count;
		}
		break;
	case FOURCC('c', 'o', '6', '4'):
		ret = rd_u32(m, b->payload + 4, &count);
		if (ret == 0 && b->payload + 8 + (uint64_t)count * 8U > b->end) {
			ret = -EINVAL;
		}
		if (ret != 0) {
			break;
		}
		t->chunk_offset = count ? malloc((size_t)count * 4U) : NULL;
		if (count != 0U && t->chunk_offset == NULL) {
			ret = -ENOMEM;
			break;
		}
		t->chunk_count = count;
		for (uint32_t i = 0; i < count && ret == 0; i++) {
			uint32_t hi, lo;

			ret = rd_u32(m, b->payload + 8 + (uint64_t)i * 8U, &hi);
			if (ret == 0) {
				ret = rd_u32(m, b->payload + 12 + (uint64_t)i * 8U, &lo);
			}
			if (ret == 0 && hi != 0U) {
				ret = -EFBIG;
			}
			t->chunk_offset[i] = lo;
		}
		break;
	case FOURCC('s', 't', 's', 'c'):
		ret = rd_u32(m, b->payload + 4, &count);
		if (ret == 0 && b->payload + 8 + (uint64_t)count * 12U > b->end) {
			ret = -EINVAL;
		}
		if (ret != 0) {
			break;
		}
		t->stsc = count ? malloc((size_t)count * 8U) : NULL;
		if (count != 0U && t->stsc == NULL) {
			ret = -ENOMEM;
			break;
		}
		t->stsc_count = count;
		for (uint32_t i = 0; i < count && ret == 0; i++) {
			ret = rd_u32(m, b->payload + 8 + (uint64_t)i * 12U, &t->stsc[2 * i]);
			if (ret == 0) {
				ret = rd_u32(m, b->payload + 12 + (uint64_t)i * 12U,
					     &t->stsc[2 * i + 1]);
			}
		}
		break;
	case FOURCC('s', 't', 's', 's'):
		ret = rd_u32(m, b->payload + 4, &count);
		if (ret == 0 && b->payload + 8 + (uint64_t)count * 4U > b->end) {
			ret = -EINVAL;
		}
		if (ret == 0) {
			ret = read_array(m, b->payload + 8, count, &t->stss);
			t->stss_count = count;
		}
		break;
	default:
		break;
	}

	return ret;
}

/* First non-empty edit of the edit list: where in the media the presentation starts */
static int parse_edts(const struct mp4 *m, const struct box *edts, struct mp4_track *t)
{
	struct box b;
	uint8_t v;
	uint32_t count, hi, lo;
	uint64_t o;
	int ret = box_at(m, edts->payload, edts->end, &b);

	if (ret != 0 || b.type != FOURCC('e', 'l', 's', 't')) {
		return 0;
	}
	ret = rd(m, b.payload, &v, 1);
	if (ret == 0) {
		ret = rd_u32(m, b.payload + 4, &count);
	}
	o = b.payload + 8;
	for (uint32_t i = 0; ret == 0 && i < count; i++) {
		int64_t media_time;

		if (v == 1U) {
			ret = rd_u32(m, o + 8, &hi);
			if (ret == 0) {
				ret = rd_u32(m, o + 12, &lo);
			}
			media_time = (int64_t)(((uint64_t)hi << 32) | lo);
			o += 20;
		} else {
			ret = rd_u32(m, o + 4, &lo);
			media_time = (int32_t)lo;
			o += 12;
		}
		if (ret == 0 && media_time >= 0) {
			t->media_start = media_time;
			break;
		}
	}

	return ret;
}

/* Walks the children of a container, @p depth selects what is entered */
static int parse_children(const struct mp4 *m, uint64_t off, uint64_t end, int depth,
			  struct mp4_track *t, struct mp4 *out)
{
	struct box b;
	int ret;

	while ((ret = box_at(m, off, end, &b)) == 0) {
		off = b.end;
		if (depth == 0) {
			if (b.type == FOURCC('m', 'o', 'o', 'v')) {
				ret = parse_children(m, b.payload, b.end, 1, NULL, out);
				return ret;
			}
		} else if (depth == 1) {
			if (b.type == FOURCC('t', 'r', 'a', 'k')) {
				struct mp4_track tmp = {0};

				ret = parse_children(m, b.payload, b.end, 2, &tmp, out);
				if (ret == 0 && tmp.codec == MP4_CODEC_H264 &&
				    out->video.codec == MP4_CODEC_NONE) {
					out->video = tmp;
				} else if (ret == 0 && tmp.codec == MP4_CODEC_AAC &&
					   out->audio.codec == MP4_CODEC_NONE) {
					out->audio = tmp;
				} else {
					track_free(&tmp);
				}
				if (ret != 0) {
					return ret;
				}
			}
		} else if (depth == 2) {
			/* inside trak: the media and the edit list matter */
			if (b.type == FOURCC('e', 'd', 't', 's')) {
				ret = parse_edts(m, &b, t);
				if (ret != 0) {
					return ret;
				}
			} else if (b.type == FOURCC('m', 'd', 'i', 'a')) {
				ret = parse_children(m, b.payload, b.end, 3, t, out);
				if (ret != 0) {
					return ret;
				}
			}
		} else if (depth == 3) {
			if (b.type == FOURCC('m', 'd', 'h', 'd')) {
				uint8_t v;
				uint32_t ts, hi, lo;

				ret = rd(m, b.payload, &v, 1);
				if (ret == 0) {
					uint64_t o = b.payload + 4 + (v == 1U ? 16U : 8U);

					ret = rd_u32(m, o, &ts);
					if (ret == 0 && v == 1U) {
						ret = rd_u32(m, o + 4, &hi);
						if (ret == 0) {
							ret = rd_u32(m, o + 8, &lo);
						}
						t->duration = ((uint64_t)hi << 32) | lo;
					} else if (ret == 0) {
						ret = rd_u32(m, o + 4, &lo);
						t->duration = lo;
					}
					t->timescale = ts;
				}
				if (ret != 0) {
					return ret;
				}
			} else if (b.type == FOURCC('m', 'i', 'n', 'f')) {
				ret = parse_children(m, b.payload, b.end, 4, t, out);
				if (ret != 0) {
					return ret;
				}
			}
		} else if (depth == 4) {
			if (b.type == FOURCC('s', 't', 'b', 'l')) {
				ret = parse_children(m, b.payload, b.end, 5, t, out);
				if (ret != 0) {
					return ret;
				}
			}
		} else {
			ret = parse_stbl_box(m, &b, t);
			if (ret != 0) {
				return ret;
			}
		}
	}

	return ret == -ENOENT ? 0 : ret;
}

static bool track_valid(const struct mp4_track *t)
{
	return t->sample_count > 0U && t->chunk_count > 0U && t->stsc_count > 0U &&
	       t->stts_count > 0U && t->timescale > 0U;
}

int mp4_open(struct mp4 *m, mp4_read_t read, void *ctx, uint64_t file_size)
{
	int ret;

	memset(m, 0, sizeof(*m));
	m->read = read;
	m->ctx = ctx;
	m->size = file_size;
	ret = parse_children(m, 0, file_size, 0, NULL, m);
	if (ret == 0 && ((m->video.codec != MP4_CODEC_NONE && !track_valid(&m->video)) ||
			 (m->audio.codec != MP4_CODEC_NONE && !track_valid(&m->audio)))) {
		ret = -EINVAL;
	}
	if (ret == 0 && m->video.codec == MP4_CODEC_NONE && m->audio.codec == MP4_CODEC_NONE) {
		ret = -ENOENT;
	}
	if (ret != 0) {
		mp4_close(m);
	}

	return ret;
}

void mp4_close(struct mp4 *m)
{
	track_free(&m->video);
	track_free(&m->audio);
}

void mp4_iter_init(const struct mp4_track *t, struct mp4_iter *it)
{
	memset(it, 0, sizeof(*it));
	it->chunk_pos = t->chunk_offset[0];
	it->stts_left = t->stts[0];
	if (t->ctts_count > 0U) {
		it->ctts_left = t->ctts[0];
	}
}

int mp4_next(const struct mp4_track *t, struct mp4_iter *it, struct mp4_sample *s)
{
	uint32_t size, spc;
	int32_t cto = 0;

	if (it->next >= t->sample_count || it->chunk >= t->chunk_count) {
		return -ENODATA;
	}
	size = t->fixed_size != 0U ? t->fixed_size : t->sizes[it->next];

	s->index = it->next;
	s->offset = it->chunk_pos;
	s->size = size;
	s->dts = it->dts;
	if (t->ctts_count > 0U && it->ctts_idx < t->ctts_count) {
		cto = (int32_t)t->ctts[2 * it->ctts_idx + 1];
	}
	s->pts = s->dts + cto;
	if (t->stss == NULL) {
		s->sync = 1;
	} else {
		while (it->stss_idx < t->stss_count && t->stss[it->stss_idx] < it->next + 1U) {
			it->stss_idx++;
		}
		s->sync = it->stss_idx < t->stss_count && t->stss[it->stss_idx] == it->next + 1U;
	}

	/* step to the following sample */
	it->next++;
	it->chunk_pos += size;
	spc = t->stsc[2 * it->stsc_idx + 1];
	if (++it->in_chunk >= spc) {
		it->in_chunk = 0;
		it->chunk++;
		if (it->chunk < t->chunk_count) {
			if (it->stsc_idx + 1U < t->stsc_count &&
			    t->stsc[2 * (it->stsc_idx + 1U)] - 1U <= it->chunk) {
				it->stsc_idx++;
			}
			it->chunk_pos = t->chunk_offset[it->chunk];
		}
	}
	it->dts += t->stts[2 * it->stts_idx + 1];
	if (--it->stts_left == 0U && it->stts_idx + 1U < t->stts_count) {
		it->stts_idx++;
		it->stts_left = t->stts[2 * it->stts_idx];
	}
	if (t->ctts_count > 0U && it->ctts_idx < t->ctts_count && --it->ctts_left == 0U) {
		it->ctts_idx++;
		if (it->ctts_idx < t->ctts_count) {
			it->ctts_left = t->ctts[2 * it->ctts_idx];
		}
	}

	return 0;
}

int mp4_seek_sync(const struct mp4_track *t, struct mp4_iter *it, uint32_t index)
{
	struct mp4_iter cur, last;
	struct mp4_sample s;

	mp4_iter_init(t, &cur);
	last = cur;
	while (cur.next <= index) {
		struct mp4_iter before = cur;

		if (mp4_next(t, &cur, &s) != 0) {
			break;
		}
		if (s.sync) {
			last = before;
		}
	}
	*it = last;

	return 0;
}
