/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Runs every operation of the G2D on small buffers and compares the result
 * with a CPU implementation (exact for copies, rotations and fills, within a
 * tolerance for scaling and blending).
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/g2d.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(g2d_sample, LOG_LEVEL_INF);

#define W	128
#define H	96

static const struct device *g2d = DEVICE_DT_GET_ANY(allwinner_sunxi_g2d);
static int failures;

static void *alloc_buf(size_t size)
{
	void *p = aligned_alloc(64, ROUND_UP(size, 64));

	__ASSERT_NO_MSG(p != NULL);
	return p;
}

static struct g2d_surface surface(enum g2d_format fmt, uint16_t w, uint16_t h)
{
	struct g2d_surface s = { .format = fmt, .width = w, .height = h };

	s.plane[0] = alloc_buf((size_t)w * h * g2d_format_bytes_per_pixel(fmt));
	return s;
}

static void release(struct g2d_surface *s)
{
	free(s->plane[0]);
}

static uint32_t *px32(const struct g2d_surface *s, int x, int y)
{
	return (uint32_t *)((uint8_t *)s->plane[0] + ((size_t)y * s->width + x) * 4);
}

static uint16_t *px16(const struct g2d_surface *s, int x, int y)
{
	return (uint16_t *)((uint8_t *)s->plane[0] + ((size_t)y * s->width + x) * 2);
}

/* test pattern: gradients and an alpha ramp, distinct per pixel */
static uint32_t pattern(int x, int y, uint8_t alpha)
{
	return ((uint32_t)alpha << 24) | ((x * 255 / W) << 16) | ((y * 255 / H) << 8) |
	       ((x + y) & 0xff);
}

static void fill_pattern(const struct g2d_surface *s, uint8_t alpha)
{
	for (int y = 0; y < s->height; y++) {
		for (int x = 0; x < s->width; x++) {
			*px32(s, x, y) = pattern(x, y, alpha);
		}
	}
	sys_cache_data_flush_range(s->plane[0], (size_t)s->width * s->height * 4);
}

static uint16_t to565(uint32_t argb)
{
	return ((argb >> 8) & 0xf800) | ((argb >> 5) & 0x07e0) | ((argb >> 3) & 0x001f);
}

static int chan_diff(uint32_t a, uint32_t b)
{
	int d = 0;

	for (int sh = 0; sh < 32; sh += 8) {
		d = MAX(d, abs((int)((a >> sh) & 0xff) - (int)((b >> sh) & 0xff)));
	}
	return d;
}

static uint32_t t0;

static void begin(void)
{
	t0 = k_cycle_get_32();
}

static void report(const char *name, bool ok, int worst, int rc)
{
	uint32_t us = k_cyc_to_us_near32(k_cycle_get_32() - t0);

	if (ok) {
		LOG_INF("%-28s PASS  max diff %3d  %u us", name, worst, us);
	} else {
		LOG_ERR("%-28s FAIL  max diff %3d  rc %d", name, worst, rc);
		failures++;
	}
}

static void test_fill(enum g2d_format fmt, const char *name)
{
	struct g2d_surface d = surface(fmt, W, H);
	struct g2d_rect r = { 16, 8, 64, 48 };
	const uint32_t color = 0xff3080c0;
	bool bpp4 = g2d_format_bytes_per_pixel(fmt) == 4;
	bool ok = true;
	int rc;

	memset(d.plane[0], 0, (size_t)W * H * g2d_format_bytes_per_pixel(fmt));
	sys_cache_data_flush_range(d.plane[0], (size_t)W * H * g2d_format_bytes_per_pixel(fmt));
	begin();
	rc = g2d_fill(g2d, &d, &r, color);
	for (int y = 0; ok && y < H; y++) {
		for (int x = 0; x < W; x++) {
			bool in = x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;

			if (bpp4) {
				uint32_t want = in ? color : 0;

				if (fmt == G2D_PIXFMT_XRGB8888) {
					want &= 0x00ffffff;
				}
				if (chan_diff(*px32(&d, x, y), want) != 0 &&
				    !(fmt == G2D_PIXFMT_XRGB8888 &&
				      (*px32(&d, x, y) & 0xffffff) == (want & 0xffffff))) {
					ok = false;
					break;
				}
			} else if (*px16(&d, x, y) != (in ? to565(color) : 0)) {
				ok = false;
				break;
			}
		}
	}
	report(name, ok && rc == 0, ok ? 0 : 255, rc);
	release(&d);
}

static void test_blit_copy(void)
{
	struct g2d_surface s = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_surface d = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_rect sr = { 10, 6, 80, 60 }, dr = { 30, 20, 80, 60 };
	bool ok = true;
	int rc;

	fill_pattern(&s, 0xff);
	memset(d.plane[0], 0, (size_t)W * H * 4);
	sys_cache_data_flush_range(d.plane[0], (size_t)W * H * 4);
	begin();
	rc = g2d_blit(g2d, &s, &sr, &d, &dr, G2D_ROTATE_0, 0);
	for (int y = 0; ok && y < sr.height; y++) {
		for (int x = 0; x < sr.width; x++) {
			if (*px32(&d, dr.x + x, dr.y + y) != *px32(&s, sr.x + x, sr.y + y)) {
				ok = false;
				break;
			}
		}
	}
	report("blit ARGB8888 copy", ok && rc == 0, ok ? 0 : 255, rc);
	release(&s);
	release(&d);
}

static void test_blit_convert(void)
{
	struct g2d_surface s = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_surface d = surface(G2D_PIXFMT_RGB565, W, H);
	struct g2d_rect r = { 0, 0, W, H };
	int worst = 0, rc;

	fill_pattern(&s, 0xff);
	memset(d.plane[0], 0, (size_t)W * H * 2);
	sys_cache_data_flush_range(d.plane[0], (size_t)W * H * 2);
	begin();
	rc = g2d_blit(g2d, &s, &r, &d, &r, G2D_ROTATE_0, 0);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint16_t want = to565(*px32(&s, x, y));
			uint16_t got = *px16(&d, x, y);
			int dr_ = abs((int)(want >> 11) - (int)(got >> 11));
			int dg = abs((int)((want >> 5) & 63) - (int)((got >> 5) & 63));
			int db = abs((int)(want & 31) - (int)(got & 31));

			worst = MAX(worst, MAX(dr_, MAX(dg, db)));
		}
	}
	report("blit ARGB8888 -> RGB565", rc == 0 && worst <= 1, worst, rc);
	release(&s);
	release(&d);
}

static void test_scale(void)
{
	struct g2d_surface s = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_surface d = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_rect sr = { 0, 0, W / 2, H / 2 }, dr = { 0, 0, W, H };
	int worst = 0, rc;

	fill_pattern(&s, 0xff);
	memset(d.plane[0], 0, (size_t)W * H * 4);
	sys_cache_data_flush_range(d.plane[0], (size_t)W * H * 4);
	begin();
	rc = g2d_blit(g2d, &s, &sr, &d, &dr, G2D_ROTATE_0, 0);
	/* smooth gradients: every output pixel is close to the source pixel it covers */
	for (int y = 2; y < H - 2; y++) {
		for (int x = 2; x < W - 2; x++) {
			worst = MAX(worst, chan_diff(*px32(&d, x, y), *px32(&s, x / 2, y / 2)));
		}
	}
	report("blit scale 2x", rc == 0 && worst <= 24, worst, rc);
	release(&s);
	release(&d);
}

static void test_rotate(enum g2d_rotation rot, uint32_t flags, const char *name)
{
	bool swap = rot == G2D_ROTATE_90 || rot == G2D_ROTATE_270;
	struct g2d_surface s = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_surface d = surface(G2D_PIXFMT_ARGB8888, swap ? H : W, swap ? W : H);
	struct g2d_rect sr = { 0, 0, W, H }, dr = { 0, 0, d.width, d.height };
	bool ok = true;
	int rc;

	fill_pattern(&s, 0xff);
	memset(d.plane[0], 0, (size_t)W * H * 4);
	sys_cache_data_flush_range(d.plane[0], (size_t)W * H * 4);
	begin();
	rc = g2d_blit(g2d, &s, &sr, &d, &dr, rot, flags);
	for (int y = 0; ok && y < d.height; y++) {
		for (int x = 0; x < d.width; x++) {
			int sx, sy;
			int fx = (flags & G2D_FLIP_H) ? d.width - 1 - x : x;
			int fy = (flags & G2D_FLIP_V) ? d.height - 1 - y : y;

			/* rotate first, then flip: undo the flip, then the rotation */
			switch (rot) {
			case G2D_ROTATE_90:
				sx = fy;
				sy = H - 1 - fx;
				break;
			case G2D_ROTATE_180:
				sx = W - 1 - fx;
				sy = H - 1 - fy;
				break;
			case G2D_ROTATE_270:
				sx = W - 1 - fy;
				sy = fx;
				break;
			default:
				sx = fx;
				sy = fy;
				break;
			}
			if (*px32(&d, x, y) != *px32(&s, sx, sy)) {
				ok = false;
				break;
			}
		}
	}
	report(name, ok && rc == 0, ok ? 0 : 255, rc);
	release(&s);
	release(&d);
}


/* solid colour in limited range BT.601 YCbCr */
struct yuv_color {
	uint8_t y, u, v;
	uint32_t argb;
};

static const struct yuv_color yuv_colors[] = {
	{ 235, 128, 128, 0xffffffff },	/* white */
	{ 16, 128, 128, 0xff000000 },	/* black */
	{ 81, 90, 240, 0xffff0000 },	/* red */
	{ 145, 54, 34, 0xff00ff00 },	/* green */
	{ 41, 240, 110, 0xff0000ff },	/* blue */
};

/* the colours sit in horizontal bands so that a swapped U/V order shows up */
static void test_yuv(enum g2d_format fmt, const char *name)
{
	const int w = 64, h = 40, band = h / ARRAY_SIZE(yuv_colors);
	struct g2d_surface s = { .format = fmt, .width = w, .height = h };
	struct g2d_surface d = surface(G2D_PIXFMT_ARGB8888, w, h);
	struct g2d_rect r = { 0, 0, w, h };
	size_t size = (size_t)w * h * 2;
	uint8_t *buf = alloc_buf(size);
	uint8_t *y = buf, *u = NULL, *v = NULL;
	int worst = 0, rc;

	memset(buf, 0, size);
	s.plane[0] = buf;
	for (int row = 0; row < h; row++) {
		const struct yuv_color *c = &yuv_colors[MIN(row / band, ARRAY_SIZE(yuv_colors) - 1)];

		switch (fmt) {
		case G2D_PIXFMT_YUYV:
			for (int x = 0; x < w; x += 2) {
				uint8_t *p = buf + ((size_t)row * w + x) * 2;

				p[0] = c->y;
				p[1] = c->u;
				p[2] = c->y;
				p[3] = c->v;
			}
			break;
		default:
			memset(y + (size_t)row * w, c->y, w);
			break;
		}
	}
	if (fmt == G2D_PIXFMT_NV12 || fmt == G2D_PIXFMT_NV21) {
		uint8_t *uv = buf + (size_t)w * h;

		s.plane[1] = uv;
		for (int row = 0; row < h / 2; row++) {
			const struct yuv_color *c =
				&yuv_colors[MIN(row * 2 / band, ARRAY_SIZE(yuv_colors) - 1)];

			for (int x = 0; x < w; x += 2) {
				uv[(size_t)row * w + x] = fmt == G2D_PIXFMT_NV12 ? c->u : c->v;
				uv[(size_t)row * w + x + 1] = fmt == G2D_PIXFMT_NV12 ? c->v : c->u;
			}
		}
	} else if (fmt == G2D_PIXFMT_I420) {
		u = buf + (size_t)w * h;
		v = u + (size_t)(w / 2) * (h / 2);
		s.plane[1] = u;
		s.plane[2] = v;
		for (int row = 0; row < h / 2; row++) {
			const struct yuv_color *c =
				&yuv_colors[MIN(row * 2 / band, ARRAY_SIZE(yuv_colors) - 1)];

			memset(u + (size_t)row * (w / 2), c->u, w / 2);
			memset(v + (size_t)row * (w / 2), c->v, w / 2);
		}
	}
	sys_cache_data_flush_range(buf, size);
	memset(d.plane[0], 0, (size_t)w * h * 4);
	sys_cache_data_flush_range(d.plane[0], (size_t)w * h * 4);

	begin();
	rc = g2d_blit(g2d, &s, &r, &d, &r, G2D_ROTATE_0, 0);
	/* skip the rows next to a band edge, where the chroma is shared with the next band */
	for (int row = 0; row < h; row++) {
		const struct yuv_color *c = &yuv_colors[MIN(row / band, ARRAY_SIZE(yuv_colors) - 1)];

		if (row % band < 2 || row % band >= band - 2) {
			continue;
		}
		for (int x = 0; x < w; x++) {
			worst = MAX(worst, chan_diff(*px32(&d, x, row) | 0xff000000, c->argb));
		}
	}
	if (worst > 8) {
		LOG_INF("%s: rows %08x %08x %08x %08x %08x", name, *px32(&d, 3, 4), *px32(&d, 3, 12),
			*px32(&d, 3, 20), *px32(&d, 3, 28), *px32(&d, 3, 36));
	}
	report(name, rc == 0 && worst <= 8, worst, rc);
	free(buf);
	release(&d);
}

static void test_blend(enum g2d_alpha_mode mode, uint8_t alpha, const char *name)
{
	struct g2d_surface fg = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_surface bg = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_surface d = surface(G2D_PIXFMT_ARGB8888, W, H);
	struct g2d_rect r = { 0, 0, W, H };
	struct g2d_blend b = {
		.mode = G2D_BLEND_SRC_OVER,
		.fg_alpha_mode = mode,
		.fg_alpha = alpha,
		.bg_alpha_mode = G2D_ALPHA_PIXEL,
		.bg_alpha = 0xff,
	};
	int worst = 0, rc;

	fill_pattern(&fg, 0x80);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			*px32(&bg, x, y) = 0xff000000 | ((255 - x * 2) << 16) | (y << 8) | 0x40;
		}
	}
	sys_cache_data_flush_range(bg.plane[0], (size_t)W * H * 4);
	memset(d.plane[0], 0, (size_t)W * H * 4);
	sys_cache_data_flush_range(d.plane[0], (size_t)W * H * 4);
	begin();
	rc = g2d_blend(g2d, &fg, &r, &bg, &r, &d, &r, &b, 0);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint32_t f = *px32(&fg, x, y), g = *px32(&bg, x, y), want = 0xff000000;
			unsigned int a = f >> 24;

			if (mode == G2D_ALPHA_GLOBAL) {
				a = alpha;
			} else if (mode == G2D_ALPHA_MIXED) {
				a = a * alpha / 255;
			}
			for (int sh = 0; sh < 24; sh += 8) {
				unsigned int fc = (f >> sh) & 0xff, gc = (g >> sh) & 0xff;

				want |= ((fc * a + gc * (255 - a)) / 255) << sh;
			}
			worst = MAX(worst, chan_diff(*px32(&d, x, y) | 0xff000000, want));
		}
	}
	report(name, rc == 0 && worst <= 3, worst, rc);
	release(&fg);
	release(&bg);
	release(&d);
}

int main(void)
{
	if (g2d == NULL || !device_is_ready(g2d)) {
		LOG_ERR("no G2D device");
		return -ENODEV;
	}

	test_fill(G2D_PIXFMT_ARGB8888, "fill ARGB8888");
	test_fill(G2D_PIXFMT_XRGB8888, "fill XRGB8888");
	test_fill(G2D_PIXFMT_RGB565, "fill RGB565");
	test_blit_copy();
	test_blit_convert();
	test_scale();
	test_rotate(G2D_ROTATE_0, G2D_FLIP_H, "flip horizontal");
	test_rotate(G2D_ROTATE_0, G2D_FLIP_V, "flip vertical");
	test_rotate(G2D_ROTATE_90, 0, "rotate 90");
	test_rotate(G2D_ROTATE_180, 0, "rotate 180");
	test_rotate(G2D_ROTATE_270, 0, "rotate 270");
	test_yuv(G2D_PIXFMT_NV12, "NV12 -> ARGB8888");
	test_yuv(G2D_PIXFMT_NV21, "NV21 -> ARGB8888");
	test_yuv(G2D_PIXFMT_I420, "I420 -> ARGB8888");
	test_yuv(G2D_PIXFMT_YUYV, "YUYV -> ARGB8888");
	test_blend(G2D_ALPHA_PIXEL, 0xff, "blend src-over, pixel alpha");
	test_blend(G2D_ALPHA_GLOBAL, 0x80, "blend src-over, global alpha");
	test_blend(G2D_ALPHA_MIXED, 0x80, "blend src-over, mixed alpha");

	LOG_INF("G2D self test: %s (%d failed)", failures ? "FAIL" : "PASS", failures);
	return 0;
}
