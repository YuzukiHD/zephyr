/*
 * SPDX-License-Identifier: Apache-2.0
 */

/* Marker segment reader and table images for the JPEG engine. No hardware access. */

#include <errno.h>
#include <string.h>

#include "ve_jpeg_hw.h"

#define M_SOF0 0xc0
#define M_SOF1 0xc1
#define M_SOF2 0xc2
#define M_DHT  0xc4
#define M_SOI  0xd8
#define M_EOI  0xd9
#define M_SOS  0xda
#define M_DQT  0xdb
#define M_DRI  0xdd

struct reader {
	const uint8_t *p;
	size_t len;
	size_t pos;
};

static int rd8(struct reader *r)
{
	return r->pos < r->len ? r->p[r->pos++] : -1;
}

static int rd16(struct reader *r)
{
	int hi = rd8(r);
	int lo = rd8(r);

	return (hi < 0 || lo < 0) ? -1 : (hi << 8) | lo;
}

enum ve_jpeg_format ve_jpeg_format_of(const struct ve_jpeg_comp *c, int ncomp)
{
	uint32_t key = 0;

	if (ncomp == 1) {
		return VE_JPEG_400;
	}
	if (ncomp != 3) {
		return VE_JPEG_ERR;
	}
	/* nibbles h0 v0 h1 v1 h2 v2 */
	for (int i = 0; i < 3; i++) {
		key = (key << 8) | ((uint32_t)c[i].h << 4) | (uint32_t)c[i].v;
	}
	switch (key) {
	case 0x221111:
		return VE_JPEG_420;
	case 0x411111:
		return VE_JPEG_411;
	case 0x211111:
		return VE_JPEG_422;
	case 0x111111:
		return VE_JPEG_444;
	case 0x121111:
		return VE_JPEG_422T;
	default:
		return VE_JPEG_ERR;
	}
}

/* counts[i] codes of length i + 1 and their symbols, in the engine's form */
static void build_table(struct ve_jpeg_huff *t, const uint8_t counts[16], const uint8_t *symbols)
{
	unsigned int code = 0;
	int total = 0;
	int last = 16;

	memset(t, 0, sizeof(*t));
	for (int i = 0; i < 16; i++) {
		t->offset[i] = (uint8_t)total;
		t->start_code[i] = (uint16_t)code;
		total += counts[i];
		code = (code + counts[i]) << 1;
	}
	memcpy(t->symbol, symbols, total);
	/* lengths beyond the longest code never match */
	while (last > 0 && counts[last - 1] == 0) {
		t->start_code[last - 1] = 0xffff;
		last--;
	}
}

static int get_dqt(struct reader *r, struct ve_jpeg_frame *f)
{
	int len = rd16(r) - 2;

	while (len > 0) {
		int pq = rd8(r);
		int prec = pq >> 4;
		int id = pq & 15;

		len--;
		if (pq < 0 || id > 3 || prec > 1) {
			return -EINVAL;
		}
		for (int i = 0; i < 64; i++) {
			int v = prec ? rd16(r) : rd8(r);

			if (v < 0) {
				return -EINVAL;
			}
			f->qtab[id][i] = (uint16_t)v;
		}
		len -= prec ? 128 : 64;
	}

	return len == 0 ? 0 : -EINVAL;
}

static int get_dht(struct reader *r, struct ve_jpeg_frame *f)
{
	int len = rd16(r) - 2;

	f->has_dht = 1;
	while (len > 16) {
		int tc = rd8(r);
		int id = tc & 15;
		uint8_t counts[16];
		uint8_t symbols[256];
		int total = 0;

		if (tc < 0 || id > 1) {
			return -EINVAL;
		}
		for (int i = 0; i < 16; i++) {
			int c = rd8(r);

			if (c < 0) {
				return -EINVAL;
			}
			counts[i] = (uint8_t)c;
			total += c;
		}
		len -= 17;
		if (total > 256 || total > len) {
			return -EINVAL;
		}
		for (int i = 0; i < total; i++) {
			symbols[i] = (uint8_t)rd8(r);
		}
		len -= total;
		build_table((tc & 0x10) ? &f->ac[id] : &f->dc[id], counts, symbols);
	}

	return len == 0 ? 0 : -EINVAL;
}

static int get_sof(struct reader *r, struct ve_jpeg_frame *f)
{
	int len = rd16(r);
	int prec = rd8(r);

	f->height = rd16(r);
	f->width = rd16(r);
	f->ncomp = rd8(r);
	if (len < 0 || prec != 8 || f->height <= 0 || f->width <= 0 || f->ncomp < 0) {
		return -EINVAL;
	}
	if (f->ncomp != 1 && f->ncomp != 3) {
		return -ENOTSUP;
	}
	if (len != 8 + 3 * f->ncomp) {
		return -EINVAL;
	}
	f->max_h = f->max_v = 0;
	for (int i = 0; i < f->ncomp; i++) {
		int id = rd8(r);
		int hv = rd8(r);
		int tq = rd8(r);

		if (id < 0 || hv < 0 || tq < 0) {
			return -EINVAL;
		}
		f->comp[i].id = id;
		f->comp[i].h = hv >> 4;
		f->comp[i].v = hv & 15;
		f->comp[i].tq = tq & 3;
		if (f->comp[i].h == 0 || f->comp[i].v == 0) {
			return -EINVAL;
		}
		if (f->comp[i].h > f->max_h) {
			f->max_h = f->comp[i].h;
		}
		if (f->comp[i].v > f->max_v) {
			f->max_v = f->comp[i].v;
		}
	}
	f->format = ve_jpeg_format_of(f->comp, f->ncomp);

	return f->format == VE_JPEG_ERR ? -ENOTSUP : 0;
}

static int get_sos(struct reader *r, struct ve_jpeg_frame *f)
{
	int len = rd16(r);
	int n = rd8(r);

	if (f->ncomp == 0 || n != f->ncomp || len != 6 + 2 * n) {
		return -EINVAL;
	}
	for (int i = 0; i < n; i++) {
		int id = rd8(r);
		int tt = rd8(r);
		int k;

		if (id < 0 || tt < 0) {
			return -EINVAL;
		}
		for (k = 0; k < f->ncomp; k++) {
			if (f->comp[k].id == id) {
				break;
			}
		}
		if (k == f->ncomp) {
			return -EINVAL;
		}
		f->comp[k].td = (tt >> 4) & 1;
		f->comp[k].ta = tt & 1;
		f->scan_order[i] = id;
	}
	/* spectral selection and successive approximation: baseline has 0, 63, 0 */
	if (rd8(r) != 0 || rd8(r) != 63 || rd8(r) != 0) {
		return -ENOTSUP;
	}

	return 0;
}

int ve_jpeg_parse(const uint8_t *buf, size_t len, struct ve_jpeg_frame *f)
{
	struct reader r = {.p = buf, .len = len};
	int sof = 0;

	memset(f, 0, sizeof(*f));
	if (len < 4 || buf[0] != 0xff || buf[1] != M_SOI) {
		return -EINVAL;
	}
	r.pos = 2;
	for (;;) {
		int c = rd8(&r);
		int ret = 0;

		if (c < 0) {
			return -EINVAL;
		}
		if (c != 0xff) {
			continue; /* damaged: look for the next marker */
		}
		while ((c = rd8(&r)) == 0xff) {
		}
		if (c < 0) {
			return -EINVAL;
		}
		switch (c) {
		case 0:
		case 1:
		case M_SOI:
			break;
		case M_SOF0:
		case M_SOF1:
			ret = get_sof(&r, f);
			sof = ret == 0;
			break;
		case M_SOF2:
		case 0xc3:
		case 0xc5 ... 0xc7:
		case 0xc9 ... 0xcf:
			return -ENOTSUP; /* progressive, lossless, arithmetic */
		case M_DHT:
			ret = get_dht(&r, f);
			break;
		case M_DQT:
			ret = get_dqt(&r, f);
			break;
		case M_DRI:
			if (rd16(&r) != 4) {
				return -EINVAL;
			}
			f->restart_interval = rd16(&r);
			break;
		case M_SOS:
			if (!sof) {
				return -EINVAL;
			}
			ret = get_sos(&r, f);
			if (ret == 0) {
				size_t end = len;

				f->data_off = r.pos;
				/* the data ends with the last EOI */
				while (end > f->data_off + 1 &&
				       !(buf[end - 2] == 0xff && buf[end - 1] == M_EOI)) {
					end--;
				}
				/* the end marker belongs to the data: the engine needs it to flush
				 * the last block */
				f->data_size = end - f->data_off;
			}
			return ret;
		case M_EOI:
			return -EINVAL;
		default:
			if (c >= 0xd0 && c <= 0xd7) {
				break;
			}
			/* APPn, COM and the rest: skip the segment */
			{
				int seg = rd16(&r);

				if (seg < 2 || r.pos + seg - 2 > len) {
					return -EINVAL;
				}
				r.pos += seg - 2;
			}
			break;
		}
		if (ret != 0) {
			return ret;
		}
	}
}

/*
 * Table memory of the engine: 1024 half words, four 128-entry blocks of
 * start codes / offsets / zero padding for DC0 AC0 DC1 AC1, then the
 * symbols in four 128-entry blocks starting at 512.
 */
static void put_table(uint16_t *img, int block, const struct ve_jpeg_huff *t)
{
	uint16_t *b = img + block * 32;

	memcpy(b, t->start_code, sizeof(t->start_code));
	for (int i = 0; i < 8; i++) {
		b[16 + i] = (uint16_t)(t->offset[2 * i] | (t->offset[2 * i + 1] << 8));
		b[24 + i] = 0;
	}
	for (int i = 0; i < 128; i++) {
		img[512 + block * 128 + i] =
			(uint16_t)(t->symbol[2 * i] | (t->symbol[2 * i + 1] << 8));
	}
}

static const struct ve_jpeg_comp *scan_comp(const struct ve_jpeg_frame *f, int n)
{
	int id = f->scan_order[n < f->ncomp ? n : 0];

	for (int i = 0; i < f->ncomp; i++) {
		if (f->comp[i].id == id) {
			return &f->comp[i];
		}
	}

	return &f->comp[0];
}

void ve_jpeg_huff_image(const struct ve_jpeg_frame *f, uint32_t out[VE_JPEG_HUFF_WORDS])
{
	uint16_t img[1024] = {0};
	const struct ve_jpeg_comp *c0 = scan_comp(f, 0);
	const struct ve_jpeg_comp *c1 = scan_comp(f, 1);

	put_table(img, 0, &f->dc[c0->td]);
	put_table(img, 1, &f->ac[c0->ta]);
	put_table(img, 2, &f->dc[c1->td]);
	put_table(img, 3, &f->ac[c1->ta]);
	memcpy(out, img, sizeof(img));
}

/* The standard tables of the baseline process, used when a stream has none */
static const uint8_t std_dc_l_counts[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t std_dc_c_counts[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t std_dc_symbols[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t std_ac_l_counts[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t std_ac_l_symbols[162] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
	0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
	0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
	0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
	0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
	0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
	0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
	0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
	0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3,
	0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
	0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};
static const uint8_t std_ac_c_counts[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t std_ac_c_symbols[162] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61,
	0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33,
	0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
	0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
	0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63,
	0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
	0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
	0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
	0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
	0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
	0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

const uint32_t *ve_jpeg_default_huff_image(void)
{
	static uint32_t image[VE_JPEG_HUFF_WORDS];
	static int ready;

	if (!ready) {
		struct ve_jpeg_frame f = {.ncomp = 3};

		build_table(&f.dc[0], std_dc_l_counts, std_dc_symbols);
		build_table(&f.dc[1], std_dc_c_counts, std_dc_symbols);
		build_table(&f.ac[0], std_ac_l_counts, std_ac_l_symbols);
		build_table(&f.ac[1], std_ac_c_counts, std_ac_c_symbols);
		for (int i = 0; i < 3; i++) {
			f.comp[i].id = i;
			f.scan_order[i] = i;
			f.comp[i].td = f.comp[i].ta = i > 0;
		}
		ve_jpeg_huff_image(&f, image);
		ready = 1;
	}

	return image;
}
