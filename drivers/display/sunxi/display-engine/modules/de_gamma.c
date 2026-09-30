// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display gamma: 1024 entry, 10 bit per component look-up table on the
 * blended output. The block also has a colour matrix and a colour
 * temperature (gain/offset) stage; both are left disabled.
 */
#define DPY_LOG_TAG "de-gamma"
#include "../de_priv.h"

#define GAMMA_CM_EN			0x000	/* cm block */
#define GAMMA_CTL			0x000	/* ctl block, at +0x40 */
#define   GAMMA_CTL_EN			DPY_BIT(0)
#define GAMMA_CTC_CTL			0x000	/* ctc block, at +0x50 */
#define GAMMA_TABLE_ENTRIES		1024
#define GAMMA_ENTRY(r, g, b) \
	(((uint32_t)(r) << 20) | ((uint32_t)(g) << 10) | (uint32_t)(b))

enum {
	GAMMA_BLK_CM,
	GAMMA_BLK_CTL,
	GAMMA_BLK_CTC,
	GAMMA_BLK_TABLE,
	GAMMA_BLK_NR,
};

struct de_gamma {
	uint32_t loaded_seq;
	bool loaded;
};

static int de_gamma_init(struct de_stage *st)
{
	static const struct {
		uint32_t off, size;
	} blocks[GAMMA_BLK_NR] = {
		[GAMMA_BLK_CM] = { 0x000, 0x40 },
		[GAMMA_BLK_CTL] = { 0x040, 0x04 },
		[GAMMA_BLK_CTC] = { 0x050, 0x1c },
		[GAMMA_BLK_TABLE] = { 0x100, GAMMA_TABLE_ENTRIES * 4 },
	};
	unsigned int i;

	st->priv = dpy_os_zalloc(sizeof(struct de_gamma));
	if (!st->priv)
		return -ENOMEM;
	for (i = 0; i < GAMMA_BLK_NR; i++) {
		st->blk[i] = de_regblk_create(st->de, "gamma",
					      st->desc->offset + blocks[i].off,
					      blocks[i].size, true,
					      st->pipe->desc->disp);
		if (!st->blk[i])
			return -ENOMEM;
	}
	st->nblks = GAMMA_BLK_NR;
	return 0;
}

/* linear interpolation of a 256 entry 16 bit curve at 10 bit resolution */
static uint32_t de_gamma_sample(const uint16_t *curve, uint32_t i)
{
	uint32_t pos = i * (DPY_GAMMA_SIZE - 1) * 16 / (GAMMA_TABLE_ENTRIES - 1);
	uint32_t idx = pos / 16, frac = pos % 16;
	uint32_t a = curve[idx];
	uint32_t b = curve[idx + 1 < DPY_GAMMA_SIZE ? idx + 1 : idx];
	uint32_t v = (a * (16 - frac) + b * frac) / 16;

	return v >> 6;	/* 16 -> 10 bit */
}

static void de_gamma_apply(struct de_stage *st, const struct de_crtc_state *s)
{
	struct de_gamma *g = st->priv;
	const struct de_disp_plan *d = &s->disp;
	const struct dpy_gamma_lut *lut;
	struct de_regblk *tab = st->blk[GAMMA_BLK_TABLE];
	uint32_t i;

	de_rb_write(st->blk[GAMMA_BLK_CM], GAMMA_CM_EN, 0);
	de_rb_write(st->blk[GAMMA_BLK_CTC], GAMMA_CTC_CTL, 0);

	if (!d->gamma_enable) {
		de_rb_write(st->blk[GAMMA_BLK_CTL], GAMMA_CTL, 0);
		return;
	}

	/* regenerating 1024 entries is only needed when the curve changed */
	if (!g->loaded || g->loaded_seq != d->gamma_seq) {
		lut = st->de->crtc[st->pipe->index].base.gamma;
		for (i = 0; i < GAMMA_TABLE_ENTRIES; i++)
			de_rb_write(tab, i * 4,
				    GAMMA_ENTRY(de_gamma_sample(lut->r, i),
						de_gamma_sample(lut->g, i),
						de_gamma_sample(lut->b, i)));
		g->loaded = true;
		g->loaded_seq = d->gamma_seq;
	}
	de_rb_write(st->blk[GAMMA_BLK_CTL], GAMMA_CTL, GAMMA_CTL_EN);
}

const struct de_stage_ops de_gamma_ops = {
	.init = de_gamma_init,
	.apply = de_gamma_apply,
};
