// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display CSC: picture adjustment (brightness, contrast, saturation, hue)
 * on the blended RGB output. The adjustment is done in full range BT.601
 * YCbCr: RGB -> YCbCr -> BCSH -> RGB, folded into one matrix.
 */
#define DPY_LOG_TAG "de-dcsc"
#include "../de_priv.h"
#include "de_csc.h"

static int de_dcsc_init(struct de_stage *st)
{
	st->blk[0] = de_regblk_create(st->de, "dcsc", st->desc->offset,
				      DE_CSC_BLOCK_SIZE, true,
				      st->pipe->desc->disp);
	if (!st->blk[0])
		return -ENOMEM;
	st->nblks = 1;
	return 0;
}

static void de_dcsc_apply(struct de_stage *st, const struct de_crtc_state *s)
{
	const struct dpy_color_adjust *adj = &s->disp.adjust;
	struct de_csc to_yuv, bcsh, to_rgb, t, m;

	if (de_csc_adjust_neutral(adj)) {
		de_csc_identity(&m);
		de_csc_write(st->blk[0], &m, false);
		return;
	}
	de_csc_rgb2yuv(DISPLAY_COLOR_BT601, &to_yuv);
	de_csc_bcsh(adj, &bcsh);
	de_csc_yuv2rgb(DISPLAY_COLOR_BT601, DISPLAY_RANGE_FULL, &to_rgb);
	de_csc_compose(&to_yuv, &bcsh, &t);
	de_csc_compose(&t, &to_rgb, &m);
	de_csc_write(st->blk[0], &m, true);
}

const struct de_stage_ops de_dcsc_ops = {
	.init = de_dcsc_init,
	.apply = de_dcsc_apply,
};
