// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Output dither: reduces the 8 bit per component output to the depth of
 * the panel bus (RGB666/RGB565) with error diffusion.
 */
#define DPY_LOG_TAG "de-dither"
#include "../de_priv.h"

#define DITHER_CTL			0x00
#define   DITHER_EN			DPY_BIT(0)
#define   DITHER_FMT			DPY_GENMASK(3, 1)
#define   DITHER_MODE			DPY_GENMASK(7, 4)
#define   DITHER_FIFO_3D		DPY_BIT(8)
#define DITHER_SIZE			0x04

static int de_dither_init(struct de_stage *st)
{
	st->blk[0] = de_regblk_create(st->de, "dither", st->desc->offset, 0x08,
				      true, st->pipe->desc->disp);
	if (!st->blk[0])
		return -ENOMEM;
	st->nblks = 1;
	return 0;
}

static int de_dither_check(struct de_stage *st, struct de_crtc_state *s)
{
	(void)st;
	/* ordered dithering cannot produce 565/666 on this block */
	if (s->disp.dither && s->disp.dither_mode == DE_DITHER_ORDERED &&
	    s->disp.dither_fmt != DE_DITHER_FMT_888)
		return -EINVAL;
	return 0;
}

static void de_dither_apply(struct de_stage *st, const struct de_crtc_state *s)
{
	const struct de_disp_plan *d = &s->disp;

	de_rb_write(st->blk[0], DITHER_SIZE,
		    ((uint32_t)(d->h - 1) << 16) | (d->w - 1));
	de_rb_write(st->blk[0], DITHER_CTL,
		    d->dither ? DITHER_EN |
				DPY_FIELD_PREP(DITHER_FMT, d->dither_fmt) |
				DPY_FIELD_PREP(DITHER_MODE, d->dither_mode) : 0);
}

const struct de_stage_ops de_dither_ops = {
	.init = de_dither_init,
	.check = de_dither_check,
	.apply = de_dither_apply,
};
