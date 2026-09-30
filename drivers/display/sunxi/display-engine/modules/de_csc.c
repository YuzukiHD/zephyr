// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Colour space conversion maths (fixed point, no FPU needed).
 *
 * Matrices are derived from the luma weights Kr/Kb of the colour
 * encoding (ITU-R BT.601 / BT.709) instead of being tabulated.
 */
#include "../de_priv.h"
#include "de_csc.h"

/* luma weights, Q16 */
static const struct {
	int32_t kr, kb;
} de_csc_weights[] = {
	[DISPLAY_COLOR_BT601] = { 19595, 7471 },	/* 0.299, 0.114 */
	[DISPLAY_COLOR_BT709] = { 13933, 4732 },	/* 0.2126, 0.0722 */
};

static int32_t q16_mul(int32_t a, int32_t b)
{
	int64_t p = (int64_t)a * b;

	return (int32_t)((p + (p >= 0 ? 0x8000 : -0x8000)) / 65536);
}

static int32_t q16_div(int32_t a, int32_t b)
{
	int64_t n = (int64_t)a * 65536;

	return (int32_t)((n + (n >= 0 ? b / 2 : -b / 2)) / b);
}

void de_csc_identity(struct de_csc *c)
{
	int i;

	memset(c, 0, sizeof(*c));
	for (i = 0; i < 3; i++)
		c->m[i][i] = DE_CSC_ONE;
}

bool de_csc_is_identity(const struct de_csc *c)
{
	struct de_csc id;

	de_csc_identity(&id);
	return !memcmp(c, &id, sizeof(id));
}

void de_csc_compose(const struct de_csc *a, const struct de_csc *b,
		    struct de_csc *r)
{
	struct de_csc t;
	int i, j, k;

	/* b(a(x)) = Mb*Ma*(x - da) + Mb*(oa - db) + ob */
	for (i = 0; i < 3; i++) {
		for (j = 0; j < 3; j++) {
			int64_t s = 0;

			for (k = 0; k < 3; k++)
				s += (int64_t)b->m[i][k] * a->m[k][j];
			t.m[i][j] = (int32_t)((s + (s >= 0 ? 0x8000 : -0x8000)) /
					      65536);
		}
		t.in_off[i] = a->in_off[i];
	}
	for (i = 0; i < 3; i++) {
		int64_t s = 0;

		for (k = 0; k < 3; k++)
			s += (int64_t)b->m[i][k] * (a->out_off[k] - b->in_off[k]);
		t.out_off[i] = (int32_t)((s + (s >= 0 ? 0x8000 : -0x8000)) /
					 65536) + b->out_off[i];
	}
	*r = t;
}

void de_csc_rgb2yuv(uint8_t encoding, struct de_csc *c)
{
	int32_t kr, kb, kg;

	if (encoding >= DPY_ARRAY_SIZE(de_csc_weights))
		encoding = DISPLAY_COLOR_BT601;
	kr = de_csc_weights[encoding].kr;
	kb = de_csc_weights[encoding].kb;
	kg = DE_CSC_ONE - kr - kb;

	memset(c, 0, sizeof(*c));
	/* Y = Kr R + Kg G + Kb B */
	c->m[0][0] = kr;
	c->m[0][1] = kg;
	c->m[0][2] = kb;
	/* Cb = (B - Y) / (2 (1 - Kb)) */
	c->m[1][0] = -q16_div(kr, 2 * (DE_CSC_ONE - kb));
	c->m[1][1] = -q16_div(kg, 2 * (DE_CSC_ONE - kb));
	c->m[1][2] = DE_CSC_ONE / 2;
	/* Cr = (R - Y) / (2 (1 - Kr)) */
	c->m[2][0] = DE_CSC_ONE / 2;
	c->m[2][1] = -q16_div(kg, 2 * (DE_CSC_ONE - kr));
	c->m[2][2] = -q16_div(kb, 2 * (DE_CSC_ONE - kr));
	c->out_off[1] = 512;
	c->out_off[2] = 512;
}

void de_csc_yuv2rgb(uint8_t encoding, uint8_t range, struct de_csc *c)
{
	int32_t kr, kb, kg, ys, cs;

	if (encoding >= DPY_ARRAY_SIZE(de_csc_weights))
		encoding = DISPLAY_COLOR_BT601;
	kr = de_csc_weights[encoding].kr;
	kb = de_csc_weights[encoding].kb;
	kg = DE_CSC_ONE - kr - kb;

	memset(c, 0, sizeof(*c));
	c->in_off[1] = 512;
	c->in_off[2] = 512;
	if (range == DISPLAY_RANGE_FULL) {
		ys = DE_CSC_ONE;
		cs = DE_CSC_ONE;
	} else {
		/* expand 64..940 luma and 64..960 chroma to full range */
		ys = q16_div(1023, 876);
		cs = q16_div(1023, 896);
		c->in_off[0] = 64;
	}

	/* R = Y + 2 (1 - Kr) Cr */
	c->m[0][0] = ys;
	c->m[0][2] = q16_mul(2 * (DE_CSC_ONE - kr), cs);
	/* G = Y - 2 Kb (1 - Kb) / Kg Cb - 2 Kr (1 - Kr) / Kg Cr */
	c->m[1][0] = ys;
	c->m[1][1] = -q16_mul(q16_div(q16_mul(2 * kb, DE_CSC_ONE - kb), kg), cs);
	c->m[1][2] = -q16_mul(q16_div(q16_mul(2 * kr, DE_CSC_ONE - kr), kg), cs);
	/* B = Y + 2 (1 - Kb) Cb */
	c->m[2][0] = ys;
	c->m[2][1] = q16_mul(2 * (DE_CSC_ONE - kb), cs);
}

bool de_csc_adjust_neutral(const struct dpy_color_adjust *adj)
{
	return adj->brightness == 50 && adj->contrast == 50 &&
	       adj->saturation == 50 && adj->hue == 50;
}

/* sin(deg) in Q16 for 0 <= deg <= 180 (Bhaskara I approximation) */
static int32_t de_sin_q16(int32_t deg)
{
	int64_t p = (int64_t)deg * (180 - deg);

	return (int32_t)(4 * p * 65536 / (40500 - p));
}

static void de_sincos_q16(int32_t deg, int32_t *s, int32_t *c)
{
	int32_t sign = 1;

	/* bring to 0..180, sin(-x) = -sin(x) */
	if (deg < 0) {
		deg = -deg;
		sign = -1;
	}
	*s = sign * de_sin_q16(deg);
	/* cos(x) = sin(x + 90) for x <= 90, -sin(x - 90) above */
	*c = deg <= 90 ? de_sin_q16(deg + 90) : -de_sin_q16(deg - 90);
}

void de_csc_bcsh(const struct dpy_color_adjust *adj, struct de_csc *c)
{
	/* 0..100 with 50 neutral: gain 0..2, offset +-1/4, hue +-180 deg */
	int32_t contrast = adj->contrast * DE_CSC_ONE / 50;
	int32_t sat = q16_mul(adj->saturation * DE_CSC_ONE / 50, contrast);
	int32_t bright = ((int32_t)adj->brightness - 50) * 256 / 50;
	int32_t hue = ((int32_t)adj->hue - 50) * 180 / 50;
	int32_t s, co;

	de_sincos_q16(hue, &s, &co);
	memset(c, 0, sizeof(*c));
	c->m[0][0] = contrast;
	c->m[1][1] = q16_mul(sat, co);
	c->m[1][2] = q16_mul(sat, s);
	c->m[2][1] = -q16_mul(sat, s);
	c->m[2][2] = q16_mul(sat, co);
	c->in_off[1] = 512;
	c->in_off[2] = 512;
	c->out_off[0] = bright;
	c->out_off[1] = 512;
	c->out_off[2] = 512;
}

/* Q16 -> Q10 with rounding away from zero like the hardware expects */
static uint32_t de_csc_q10(int32_t v)
{
	int32_t r = v >= 0 ? (v + 32) >> 6 : -((-v + 32) >> 6);

	return (uint32_t)r;
}

void de_csc_write(struct de_regblk *blk, const struct de_csc *c, bool enable)
{
	int r, k;

	de_rb_write(blk, DE_CSC_REG_CTL, enable ? 1 : 0);
	for (r = 0; r < 3; r++) {
		de_rb_write(blk, DE_CSC_REG_D(r), (uint32_t)c->in_off[r]);
		for (k = 0; k < 3; k++)
			de_rb_write(blk, DE_CSC_REG_C(r, k), de_csc_q10(c->m[r][k]));
		de_rb_write(blk, DE_CSC_REG_C(r, 3), (uint32_t)c->out_off[r]);
	}
}
