/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * PNG decoder. It is a software decoder: the zlib stream is inflated
 * through a sliding window, every scanline is unfiltered as soon as it is
 * complete and converted straight into the picture buffer, so the memory used
 * besides the picture is the window (64 KiB and a few rows) however large the
 * picture is. All colour types and bit depths, tRNS transparency and Adam7
 * interlacing are handled. The picture is delivered as 32 bit words with R on
 * top, A, B, G, R in memory.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "ve_codec.h"
#include "ve_log.h"
#include "ve_rvv.h"
#include "ve_session.h"

VE_LOG_DECLARE();

#define PNG_SIGNATURE_LEN	8
#define PNG_MAX_DIMENSION	0x7fff
#define WINDOW_SIZE		32768
#define MAX_MATCH		258

#define CT_GRAY			0
#define CT_RGB			2
#define CT_PALETTE		3
#define CT_GRAY_ALPHA		4
#define CT_RGBA			6

#define FAST_BITS		9
#define MAX_BITS		15

/* ---- inflate ---- */

struct huff {
	uint16_t count[MAX_BITS + 1];
	uint16_t symbol[288];
	uint16_t fast[1 << FAST_BITS];	/* (length << 10) | symbol, 0: longer than FAST_BITS */
};

struct png;

struct inflate {
	/* compressed data: the IDAT chunks one after the other */
	const uint8_t *p;
	const uint8_t *end;
	const uint8_t *next_chunk;	/* header of the chunk behind the current one */
	const uint8_t *file_end;
	uint32_t bitbuf;
	int bitcnt;
	bool eof;
	/* output */
	uint8_t *win;
	uint32_t mask;
	uint32_t pos;			/* bytes produced */
	uint32_t lined;			/* bytes handed to the scanline stage */
	uint32_t total;			/* bytes the picture needs */
	uint32_t adler_a, adler_b;
	struct huff lit, dist;
	struct png *png;
};

static const uint16_t len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t clen_order[19] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* Build the decoding tables of a canonical code; returns <0 when over-subscribed */
static int huff_build(struct huff *h, const uint8_t *lengths, int n)
{
	uint16_t offs[MAX_BITS + 2];
	uint16_t next[MAX_BITS + 2];
	int left = 1;
	uint32_t code = 0;

	memset(h->count, 0, sizeof(h->count));
	memset(h->fast, 0, sizeof(h->fast));
	for (int i = 0; i < n; i++) {
		h->count[lengths[i]]++;
	}
	h->count[0] = 0;
	for (int len = 1; len <= MAX_BITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0) {
			return -1;
		}
	}
	offs[1] = 0;
	for (int len = 1; len < MAX_BITS; len++) {
		offs[len + 1] = offs[len] + h->count[len];
	}
	for (int len = 1; len <= MAX_BITS; len++) {
		next[len] = code;
		code = (code + h->count[len]) << 1;
	}
	for (int i = 0; i < n; i++) {
		int len = lengths[i];

		if (len == 0) {
			continue;
		}
		h->symbol[offs[len]++] = i;
		if (len <= FAST_BITS) {
			uint32_t c = next[len]++;
			uint32_t rev = 0;

			for (int b = 0; b < len; b++) {
				rev |= ((c >> b) & 1U) << (len - 1 - b);
			}
			for (uint32_t k = rev; k < (1U << FAST_BITS); k += 1U << len) {
				h->fast[k] = (len << 10) | i;
			}
		} else {
			next[len]++;
		}
	}

	return 0;
}

/* Next byte of the zlib stream, which continues over the IDAT chunks */
static int next_byte(struct inflate *s)
{
	while (s->p >= s->end) {
		const uint8_t *c = s->next_chunk;
		uint32_t len;

		if (c == NULL || c + 12 > s->file_end || memcmp(c + 4, "IDAT", 4) != 0) {
			s->eof = true;
			return -1;
		}
		len = sys_get_be32(c);
		if (len > (uint32_t)(s->file_end - c - 12)) {
			len = s->file_end - c - 12;	/* truncated file: use what is there */
			s->next_chunk = NULL;
		} else {
			s->next_chunk = c + 12 + len;
		}
		s->p = c + 8;
		s->end = c + 8 + len;
	}

	return *s->p++;
}

/* Make at least n bits (n <= 24) available if the data lasts */
static inline void refill(struct inflate *s, int n)
{
	while (s->bitcnt < n) {
		int b = next_byte(s);

		if (b < 0) {
			return;
		}
		s->bitbuf |= (uint32_t)b << s->bitcnt;
		s->bitcnt += 8;
	}
}

static int get_bits(struct inflate *s, int n)
{
	int v;

	if (n == 0) {
		return 0;
	}
	refill(s, n);
	if (s->bitcnt < n) {
		return -1;
	}
	v = s->bitbuf & ((1U << n) - 1U);
	s->bitbuf >>= n;
	s->bitcnt -= n;

	return v;
}

static int decode_symbol(struct inflate *s, const struct huff *h)
{
	uint32_t e;
	int code = 0, first = 0, index = 0;

	refill(s, MAX_BITS);
	e = h->fast[s->bitbuf & ((1U << FAST_BITS) - 1U)];
	if (e != 0) {
		int len = e >> 10;

		if (len > s->bitcnt) {
			return -1;
		}
		s->bitbuf >>= len;
		s->bitcnt -= len;

		return e & 0x3ff;
	}
	for (int len = 1; len <= MAX_BITS; len++) {
		int count;

		if (s->bitcnt == 0) {
			return -1;
		}
		code |= s->bitbuf & 1U;
		s->bitbuf >>= 1;
		s->bitcnt--;
		count = h->count[len];
		if (code - count < first) {
			return h->symbol[index + (code - first)];
		}
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}

	return -1;
}

/* ---- the picture and its scanlines ---- */

struct png {
	struct ve_session *session;

	/* header */
	uint32_t width, height;
	int depth, color_type, interlace;
	int channels;
	int bpp;			/* bytes per pixel for the filters, at least 1 */
	uint8_t plte[256][3];
	int plte_count;
	uint8_t trns_alpha[256];
	int trns_count;
	bool has_key;
	uint16_t key[3];

	/* scanline state */
	uint8_t *rowbuf;		/* the memory of both rows */
	uint8_t *row;			/* the row being unfiltered */
	uint8_t *prev;
	uint32_t rowbytes;
	int pass;			/* 0..6 for Adam7, 0 otherwise */
	uint32_t pass_w, pass_h, pass_row;
	bool done;
	bool bad;
	uint8_t *dst;
	int dst_stride;
};

static const uint8_t a7_xs[7] = {0, 4, 0, 2, 0, 1, 0};
static const uint8_t a7_ys[7] = {0, 0, 4, 0, 2, 0, 1};
static const uint8_t a7_dx[7] = {8, 8, 4, 4, 2, 2, 1};
static const uint8_t a7_dy[7] = {8, 8, 8, 4, 4, 2, 2};

/* Set up the dimensions of the current pass; skips passes without pixels */
static bool start_pass(struct png *p)
{
	for (;;) {
		if (p->interlace) {
			if (p->pass >= 7) {
				return false;
			}
			p->pass_w = p->width > a7_xs[p->pass] ?
				(p->width - a7_xs[p->pass] + a7_dx[p->pass] - 1) / a7_dx[p->pass] : 0;
			p->pass_h = p->height > a7_ys[p->pass] ?
				(p->height - a7_ys[p->pass] + a7_dy[p->pass] - 1) / a7_dy[p->pass] : 0;
		} else {
			if (p->pass >= 1) {
				return false;
			}
			p->pass_w = p->width;
			p->pass_h = p->height;
		}
		if (p->pass_w != 0 && p->pass_h != 0) {
			p->rowbytes = ((uint64_t)p->pass_w * p->depth * p->channels + 7) / 8;
			p->pass_row = 0;
			memset(p->prev, 0, p->rowbytes);
			return true;
		}
		p->pass++;
	}
}

static inline int paeth(int a, int b, int c)
{
	int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);

	if (pa <= pb && pa <= pc) {
		return a;
	}

	return pb <= pc ? b : c;
}

static void unfilter(struct png *p, int type)
{
	uint8_t *cur = p->row;
	const uint8_t *up = p->prev;
	uint32_t n = p->rowbytes, bpp = p->bpp;

	switch (type) {
	case 1:
		for (uint32_t i = bpp; i < n; i++) {
			cur[i] += cur[i - bpp];
		}
		break;
	case 2:
#if VE_HAVE_RVV
		ve_rvv_add_bytes(cur, up, n);
#else
		for (uint32_t i = 0; i < n; i++) {
			cur[i] += up[i];
		}
#endif
		break;
	case 3:
		for (uint32_t i = 0; i < bpp && i < n; i++) {
			cur[i] += up[i] >> 1;
		}
		for (uint32_t i = bpp; i < n; i++) {
			cur[i] += (cur[i - bpp] + up[i]) >> 1;
		}
		break;
	case 4:
		for (uint32_t i = 0; i < bpp && i < n; i++) {
			cur[i] += up[i];
		}
		for (uint32_t i = bpp; i < n; i++) {
			cur[i] += paeth(cur[i - bpp], up[i], up[i - bpp]);
		}
		break;
	default:
		break;
	}
}

static inline void put_pixel(uint8_t *o, int r, int g, int b, int a)
{
	o[0] = a;
	o[1] = b;
	o[2] = g;
	o[3] = r;
}

/* Convert the unfiltered row to pixels at x = xs + i * dx of line y */
static void convert_row(struct png *p, uint8_t *out, int dx)
{
	const uint8_t *r = p->row;
	int step = dx * 4;
	uint32_t w = p->pass_w;
	int depth = p->depth;

	switch (p->color_type) {
	case CT_RGBA:
		if (depth == 8) {
#if VE_HAVE_RVV
			if (dx == 1) {
				ve_rvv_reverse_pixels(out, r, w);
				break;
			}
#endif
			for (uint32_t i = 0; i < w; i++, r += 4, out += step) {
				put_pixel(out, r[0], r[1], r[2], r[3]);
			}
		} else {
			for (uint32_t i = 0; i < w; i++, r += 8, out += step) {
				put_pixel(out, r[0], r[2], r[4], r[6]);
			}
		}
		break;
	case CT_RGB:
		for (uint32_t i = 0; i < w; i++, out += step) {
			int a = 255;

			if (depth == 8) {
				if (p->has_key && r[0] == (p->key[0] & 0xff) &&
				    r[1] == (p->key[1] & 0xff) && r[2] == (p->key[2] & 0xff)) {
					a = 0;
				}
				put_pixel(out, r[0], r[1], r[2], a);
				r += 3;
			} else {
				if (p->has_key && sys_get_be16(r) == p->key[0] &&
				    sys_get_be16(r + 2) == p->key[1] &&
				    sys_get_be16(r + 4) == p->key[2]) {
					a = 0;
				}
				put_pixel(out, r[0], r[2], r[4], a);
				r += 6;
			}
		}
		break;
	case CT_GRAY_ALPHA:
		if (depth == 8) {
			for (uint32_t i = 0; i < w; i++, r += 2, out += step) {
				put_pixel(out, r[0], r[0], r[0], r[1]);
			}
		} else {
			for (uint32_t i = 0; i < w; i++, r += 4, out += step) {
				put_pixel(out, r[0], r[0], r[0], r[2]);
			}
		}
		break;
	case CT_GRAY:
	case CT_PALETTE:
		for (uint32_t i = 0; i < w; i++, out += step) {
			uint32_t v;
			int a = 255;

			if (depth == 16) {
				v = sys_get_be16(r + 2 * i);
			} else if (depth == 8) {
				v = r[i];
			} else {
				uint32_t bit = i * depth;

				v = (r[bit >> 3] >> (8 - depth - (bit & 7))) & ((1U << depth) - 1U);
			}
			if (p->color_type == CT_PALETTE) {
				if ((int)v >= p->plte_count) {
					p->bad = true;
					return;
				}
				if ((int)v < p->trns_count) {
					a = p->trns_alpha[v];
				}
				put_pixel(out, p->plte[v][0], p->plte[v][1], p->plte[v][2], a);
			} else {
				int g;

				if (p->has_key && v == p->key[0]) {
					a = 0;
				}
				if (depth == 16) {
					g = v >> 8;
				} else if (depth == 8) {
					g = v;
				} else {
					g = v * 255 / ((1U << depth) - 1U);
				}
				put_pixel(out, g, g, g, a);
			}
		}
		break;
	}
}

/* Adler-32 of a linear block, the modulo taken once per 5552 bytes */
static void adler_update(struct inflate *s, const uint8_t *p, uint32_t n)
{
	uint32_t a = s->adler_a, b = s->adler_b;

	while (n > 0) {
		uint32_t k = MIN(n, 5552U);

		n -= k;
#if VE_HAVE_RVV
		ve_rvv_adler_block(&a, &b, p, k);
		p += k;
#else
		while (k--) {
			a += *p++;
			b += a;
		}
#endif
		a %= 65521U;
		b %= 65521U;
	}
	s->adler_a = a;
	s->adler_b = b;
}

/* Hand the complete scanlines in the window to the unfilter/convert stage */
static int flush_lines(struct inflate *s)
{
	struct png *p = s->png;

	while (!p->done && s->pos - s->lined >= p->rowbytes + 1U) {
		uint32_t at = s->lined & s->mask;
		uint32_t n = p->rowbytes + 1U;
		uint32_t ring = s->mask + 1U;
		int filter;
		uint32_t x, y, dx;

		/* the filter byte, then the row, which may wrap in the window */
		filter = s->win[at];
		if (at + n <= ring) {
			memcpy(p->row, s->win + at + 1, n - 1);
			adler_update(s, s->win + at, n);
		} else {
			uint32_t first = ring - at;

			adler_update(s, s->win + at, first);
			adler_update(s, s->win, n - first);
			if (first > 1) {
				memcpy(p->row, s->win + at + 1, first - 1);
				memcpy(p->row + first - 1, s->win, n - first);
			} else {
				memcpy(p->row, s->win, n - 1);
			}
		}
		s->lined += n;
		if (filter > 4) {
			return -1;
		}
		unfilter(p, filter);

		if (p->interlace) {
			x = a7_xs[p->pass];
			dx = a7_dx[p->pass];
			y = a7_ys[p->pass] + p->pass_row * a7_dy[p->pass];
		} else {
			x = 0;
			dx = 1;
			y = p->pass_row;
		}
		convert_row(p, p->dst + (size_t)y * p->dst_stride + x * 4, dx);
		if (p->bad) {
			return -1;
		}

		/* the unfiltered row is the "up" row of the next one */
		uint8_t *t = p->row;

		p->row = p->prev;
		p->prev = t;
		if (++p->pass_row >= p->pass_h) {
			p->pass++;
			if (!start_pass(p)) {
				p->done = true;
			}
		}
	}

	return 0;
}

#define EMIT(b)								\
	do {								\
		s->win[s->pos & s->mask] = (b);				\
		s->pos++;						\
	} while (0)

static int inflate_codes(struct inflate *s)
{
	for (;;) {
		int sym = decode_symbol(s, &s->lit);

		if (sym < 0) {
			return -1;
		}
		if (sym < 256) {
			EMIT(sym);
		} else if (sym == 256) {
			return 0;
		} else {
			int len, dist, extra;

			sym -= 257;
			if (sym >= 29) {
				return -1;
			}
			extra = get_bits(s, len_extra[sym]);
			if (extra < 0) {
				return -1;
			}
			len = len_base[sym] + extra;
			sym = decode_symbol(s, &s->dist);
			if (sym < 0 || sym >= 30) {
				return -1;
			}
			extra = get_bits(s, dist_extra[sym]);
			if (extra < 0) {
				return -1;
			}
			dist = dist_base[sym] + extra;
			if ((uint32_t)dist > s->pos || dist > WINDOW_SIZE) {
				return -1;
			}
			uint32_t to = s->pos & s->mask, from = (s->pos - dist) & s->mask;

			if (dist >= len && to + len <= s->mask + 1U && from + len <= s->mask + 1U) {
				memcpy(s->win + to, s->win + from, len);
				s->pos += len;
			} else {
				for (int i = 0; i < len; i++) {
					s->win[s->pos & s->mask] = s->win[(s->pos - dist) & s->mask];
					s->pos++;
				}
			}
		}
		if (s->pos > s->total) {
			return -1;		/* more data than the picture holds */
		}
		if (s->pos - s->lined >= s->png->rowbytes + 1U && flush_lines(s) != 0) {
			return -1;
		}
	}
}

static int inflate_stored(struct inflate *s)
{
	int len, nlen;

	/* drop the bits up to the byte boundary; whole bytes read ahead stay in the buffer */
	if (get_bits(s, s->bitcnt & 7) < 0) {
		return -1;
	}
	len = get_bits(s, 16);
	nlen = get_bits(s, 16);
	if (len < 0 || nlen < 0 || (len ^ 0xffff) != nlen) {
		return -1;
	}
	for (int i = 0; i < len; i++) {
		int b = get_bits(s, 8);

		if (b < 0) {
			return -1;
		}
		EMIT(b);
		if (s->pos > s->total) {
			return -1;
		}
		if (s->pos - s->lined >= s->png->rowbytes + 1U && flush_lines(s) != 0) {
			return -1;
		}
	}

	return 0;
}

static int inflate_dynamic(struct inflate *s)
{
	uint8_t lengths[320];
	struct huff *tmp = &s->dist;	/* the code length code, before the distance code is read */
	int nlen = get_bits(s, 5), ndist = get_bits(s, 5), ncode = get_bits(s, 4);
	int index = 0;

	if (nlen < 0 || ndist < 0 || ncode < 0) {
		return -1;
	}
	nlen += 257;
	ndist += 1;
	ncode += 4;
	if (nlen > 286 || ndist > 30) {
		return -1;
	}
	memset(lengths, 0, 19);
	for (int i = 0; i < ncode; i++) {
		int v = get_bits(s, 3);

		if (v < 0) {
			return -1;
		}
		lengths[clen_order[i]] = v;
	}
	if (huff_build(tmp, lengths, 19) != 0) {
		return -1;
	}
	while (index < nlen + ndist) {
		int sym = decode_symbol(s, tmp);

		if (sym < 0) {
			return -1;
		}
		if (sym < 16) {
			lengths[index++] = sym;
		} else {
			int rep, val = 0;

			if (sym == 16) {
				if (index == 0) {
					return -1;
				}
				val = lengths[index - 1];
				rep = get_bits(s, 2);
				rep = rep < 0 ? rep : rep + 3;
			} else if (sym == 17) {
				rep = get_bits(s, 3);
				rep = rep < 0 ? rep : rep + 3;
			} else {
				rep = get_bits(s, 7);
				rep = rep < 0 ? rep : rep + 11;
			}
			if (rep < 0 || index + rep > nlen + ndist) {
				return -1;
			}
			while (rep--) {
				lengths[index++] = val;
			}
		}
	}
	if (lengths[256] == 0) {
		return -1;			/* no end of block code */
	}
	if (huff_build(&s->lit, lengths, nlen) != 0) {
		return -1;
	}
	/* a distance code may be incomplete (a single code) but not over-subscribed */
	if (huff_build(&s->dist, lengths + nlen, ndist) != 0) {
		return -1;
	}

	return inflate_codes(s);
}

static int inflate_fixed(struct inflate *s)
{
	uint8_t lengths[288];

	for (int i = 0; i < 144; i++) {
		lengths[i] = 8;
	}
	for (int i = 144; i < 256; i++) {
		lengths[i] = 9;
	}
	for (int i = 256; i < 280; i++) {
		lengths[i] = 7;
	}
	for (int i = 280; i < 288; i++) {
		lengths[i] = 8;
	}
	huff_build(&s->lit, lengths, 288);
	memset(lengths, 5, 30);
	huff_build(&s->dist, lengths, 30);

	return inflate_codes(s);
}

/* The zlib stream: header, blocks, adler checksum */
static int inflate_stream(struct inflate *s)
{
	int cmf = next_byte(s), flg = next_byte(s);
	int last;

	if (cmf < 0 || flg < 0 || (cmf & 0x0f) != 8 || (cmf >> 4) > 7 ||
	    ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) {
		return -1;
	}
	do {
		int type;

		last = get_bits(s, 1);
		type = get_bits(s, 2);
		if (last < 0 || type < 0) {
			return -1;
		}
		switch (type) {
		case 0:
			if (inflate_stored(s) != 0) {
				return -1;
			}
			break;
		case 1:
			if (inflate_fixed(s) != 0) {
				return -1;
			}
			break;
		case 2:
			if (inflate_dynamic(s) != 0) {
				return -1;
			}
			break;
		default:
			return -1;
		}
	} while (!last);

	if (flush_lines(s) != 0 || !s->png->done || s->pos != s->total) {
		return -1;		/* fewer rows than the header announced */
	}
	/* the checksum follows at the next byte boundary */
	if (get_bits(s, s->bitcnt & 7) < 0) {
		return -1;
	}
	{
		uint32_t want = 0;

		for (int i = 0; i < 4; i++) {
			int b = get_bits(s, 8);

			if (b < 0) {
				return -1;
			}
			want = (want << 8) | b;
		}
		if (want != ((s->adler_b << 16) | s->adler_a)) {
			LOG_WRN("png: checksum of the pixel data does not match");
			return -1;
		}
	}

	return 0;
}

/* ---- chunks ---- */

static int parse_png(struct png *p, const uint8_t *d, size_t len, const uint8_t **idat)
{
	static const uint8_t sig[PNG_SIGNATURE_LEN] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	const uint8_t *end = d + len, *c;
	bool have_ihdr = false;

	*idat = NULL;
	if (len < PNG_SIGNATURE_LEN + 12 || memcmp(d, sig, sizeof(sig)) != 0) {
		return -EINVAL;
	}
	c = d + PNG_SIGNATURE_LEN;
	while (c + 12 <= end) {
		uint32_t clen = sys_get_be32(c);
		const uint8_t *body = c + 8;
		bool truncated = clen > (uint32_t)(end - body) || (uint32_t)(end - body) - clen < 4;

		if (!memcmp(c + 4, "IDAT", 4)) {
			if (!have_ihdr) {
				return -EINVAL;
			}
			*idat = c;
			break;
		}
		if (truncated) {
			return -EINVAL;
		}
		if (crc32_ieee(c + 4, clen + 4) != sys_get_be32(body + clen)) {
			LOG_WRN("png: bad chunk checksum");
			return -EINVAL;
		}
		if (!memcmp(c + 4, "IHDR", 4)) {
			if (clen != 13 || have_ihdr) {
				return -EINVAL;
			}
			p->width = sys_get_be32(body);
			p->height = sys_get_be32(body + 4);
			p->depth = body[8];
			p->color_type = body[9];
			p->interlace = body[12];
			if (body[10] != 0 || body[11] != 0 || p->interlace > 1 || p->width == 0 ||
			    p->height == 0 || p->width > PNG_MAX_DIMENSION ||
			    p->height > PNG_MAX_DIMENSION) {
				return -ENOTSUP;
			}
			switch (p->color_type) {
			case CT_GRAY:
				p->channels = 1;
				if (p->depth != 1 && p->depth != 2 && p->depth != 4 && p->depth != 8 &&
				    p->depth != 16) {
					return -ENOTSUP;
				}
				break;
			case CT_RGB:
				p->channels = 3;
				if (p->depth != 8 && p->depth != 16) {
					return -ENOTSUP;
				}
				break;
			case CT_PALETTE:
				p->channels = 1;
				if (p->depth != 1 && p->depth != 2 && p->depth != 4 && p->depth != 8) {
					return -ENOTSUP;
				}
				break;
			case CT_GRAY_ALPHA:
				p->channels = 2;
				if (p->depth != 8 && p->depth != 16) {
					return -ENOTSUP;
				}
				break;
			case CT_RGBA:
				p->channels = 4;
				if (p->depth != 8 && p->depth != 16) {
					return -ENOTSUP;
				}
				break;
			default:
				return -ENOTSUP;
			}
			p->bpp = MAX(1, p->channels * p->depth / 8);
			have_ihdr = true;
		} else if (!memcmp(c + 4, "PLTE", 4)) {
			if (!have_ihdr || clen % 3 != 0 || clen > 768 || clen == 0) {
				return -EINVAL;
			}
			p->plte_count = clen / 3;
			memcpy(p->plte, body, clen);
		} else if (!memcmp(c + 4, "tRNS", 4)) {
			if (!have_ihdr) {
				return -EINVAL;
			}
			if (p->color_type == CT_PALETTE) {
				p->trns_count = MIN(clen, 256);
				memcpy(p->trns_alpha, body, p->trns_count);
			} else if (p->color_type == CT_GRAY && clen == 2) {
				p->has_key = true;
				p->key[0] = sys_get_be16(body);
			} else if (p->color_type == CT_RGB && clen == 6) {
				p->has_key = true;
				for (int i = 0; i < 3; i++) {
					p->key[i] = sys_get_be16(body + 2 * i);
				}
			}
		}
		c = body + clen + 4;
	}
	if (!have_ihdr || *idat == NULL) {
		return -EINVAL;
	}
	if (p->color_type == CT_PALETTE && p->plte_count == 0) {
		return -EINVAL;
	}

	return 0;
}

/* ---- codec ---- */

static void *png_open(struct ve_session *session)
{
	struct png *p = calloc(1, sizeof(*p));

	if (p != NULL) {
		p->session = session;
	}

	return p;
}

static void png_close(void *ctx)
{
	struct png *p = ctx;

	ve_pool_destroy(p->session->pool);
	p->session->pool = NULL;
	free(p);
}

static void png_reset(void *ctx)
{
	struct png *p = ctx;

	if (p->session->pool != NULL) {
		ve_pool_flush(p->session->pool);
	}
}

static int make_pool(struct png *p)
{
	struct ve_pool_config cfg = {
		.frames = 1,
		.width = p->width,
		.height = p->height,
		.format = VE_PIX_RGBA,
	};

	p->session->pool = ve_pool_create(&cfg);

	return p->session->pool != NULL ? 0 : -ENOMEM;
}

static enum ve_step png_step(void *ctx, bool eos)
{
	struct png *p = ctx;
	struct ve_session *session = p->session;
	struct inflate *s = NULL;
	struct ve_unit *unit;
	struct ve_picture *pic;
	const uint8_t *idat;
	uint64_t total = 0;
	uint32_t ring;
	enum ve_step result = VE_STEP_PROGRESS;
	bool decoded = false;
	int64_t pts;
	int ret;

	ARG_UNUSED(eos);
	unit = ve_stream_next(session->stream);
	if (unit == NULL) {
		return VE_STEP_NEED_DATA;
	}

	ret = parse_png(p, unit->data, unit->length, &idat);
	if (ret != 0) {
		ve_stream_done(session->stream, unit);
		return ret == -ENOTSUP ? VE_STEP_UNSUPPORTED : VE_STEP_PROGRESS;
	}

	/* the bytes of the zlib stream: every pass has rows of (row bytes + filter byte) */
	p->pass = 0;
	for (int pass = 0; pass < (p->interlace ? 7 : 1); pass++) {
		uint32_t pw, ph;

		if (p->interlace) {
			pw = p->width > a7_xs[pass] ?
				(p->width - a7_xs[pass] + a7_dx[pass] - 1) / a7_dx[pass] : 0;
			ph = p->height > a7_ys[pass] ?
				(p->height - a7_ys[pass] + a7_dy[pass] - 1) / a7_dy[pass] : 0;
		} else {
			pw = p->width;
			ph = p->height;
		}
		if (pw != 0 && ph != 0) {
			total += (uint64_t)ph * (((uint64_t)pw * p->depth * p->channels + 7) / 8 + 1);
		}
	}
	if (total > 0x7fffffffULL) {
		ve_stream_done(session->stream, unit);
		return VE_STEP_UNSUPPORTED;
	}

	if (session->pool == NULL && make_pool(p) != 0) {
		ve_stream_unread(session->stream, unit);
		return VE_STEP_NEED_PICTURE;
	}
	pic = ve_pool_acquire(session->pool);
	if (pic == NULL) {
		ve_stream_unread(session->stream, unit);
		return VE_STEP_NEED_PICTURE;
	}

	/* the window must hold 32 KiB of history besides the rows not yet unfiltered */
	{
		uint32_t maxrow = (((uint64_t)p->width * p->depth * p->channels + 7) / 8) + 1;

		ring = 1;
		while (ring < WINDOW_SIZE + MAX_MATCH + maxrow) {
			ring <<= 1;
		}
		s = calloc(1, sizeof(*s));
		if (s == NULL) {
			ve_pool_discard(session->pool, pic);
			ve_stream_done(session->stream, unit);
			return VE_STEP_NEED_PICTURE;
		}
		p->rowbuf = malloc(2 * (size_t)maxrow);
		p->row = p->rowbuf;
		s->win = malloc(ring);
		if (p->rowbuf == NULL || s->win == NULL) {
			result = VE_STEP_NEED_PICTURE;
			goto out;
		}
		p->prev = p->rowbuf + maxrow;
	}

	s->mask = ring - 1;
	s->total = total;
	s->adler_a = 1;
	s->png = p;
	s->p = s->end = NULL;
	s->next_chunk = idat;
	s->file_end = unit->data + unit->length;
	p->dst = pic->plane[0];
	p->dst_stride = pic->stride;
	p->done = false;
	p->bad = false;
	start_pass(p);

	decoded = inflate_stream(s) == 0;
out:
	if (s != NULL) {
		free(s->win);
	}
	free(s);
	free(p->rowbuf);
	p->rowbuf = p->row = p->prev = NULL;
	pts = unit->pts;
	ve_stream_done(session->stream, unit);
	if (!decoded) {
		ve_pool_discard(session->pool, pic);
		return result;
	}
	pic->left = 0;
	pic->top = 0;
	pic->right = p->width;
	pic->bottom = p->height;
	pic->pts = pts;
	ve_pool_submit(session->pool, pic);

	return VE_STEP_PROGRESS;
}


const struct ve_codec ve_png_codec = {
	.name = "png",
	.id = VE_CODEC_PNG,
	.hw = VE_HW_NONE,
	.open = png_open,
	.close = png_close,
	.reset = png_reset,
	.step = png_step,
};
