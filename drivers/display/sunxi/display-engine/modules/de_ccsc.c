// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Channel CSC: converts the YUV output of a video channel to the full
 * range RGB the blender works in. RGB channels bypass it.
 *
 * The video channel also has an input CSC in front of its enhancement
 * block; the enhancement block is not used, so that CSC is kept off.
 */
#define DPY_LOG_TAG "de-ccsc"
#include "../de_priv.h"
#include "de_csc.h"

struct de_ccsc {
	struct de_regblk *csc;
	struct de_regblk *icsc;
};

static int de_ccsc_init(struct de_stage *st)
{
	uint8_t disp = st->pipe->desc->disp;
	struct de_ccsc *c;

	c = dpy_os_zalloc(sizeof(*c));
	if (!c)
		return -ENOMEM;
	st->priv = c;
	c->csc = de_regblk_create(st->de, "ccsc", st->desc->offset,
				  DE_CSC_BLOCK_SIZE, true, disp);
	if (!c->csc)
		return -ENOMEM;
	st->blk[st->nblks++] = c->csc;
	if (st->desc->offset2) {
		c->icsc = de_regblk_create(st->de, "icsc", st->desc->offset2,
					   DE_CSC_BLOCK_SIZE, true, disp);
		if (!c->icsc)
			return -ENOMEM;
		st->blk[st->nblks++] = c->icsc;
	}
	return 0;
}

static void de_ccsc_apply(struct de_stage *st, const struct de_crtc_state *s)
{
	const struct de_chn_plan *cp = &s->chn[st->pipe->index];
	struct de_ccsc *c = st->priv;
	struct de_csc m;

	if (c->icsc) {
		de_csc_identity(&m);
		de_csc_write(c->icsc, &m, false);
	}
	if (!cp->enable || !cp->yuv) {
		de_csc_identity(&m);
		de_csc_write(c->csc, &m, false);
		return;
	}
	de_csc_yuv2rgb(cp->color_encoding, cp->color_range, &m);
	de_csc_write(c->csc, &m, true);
}

const struct de_stage_ops de_ccsc_ops = {
	.init = de_ccsc_init,
	.apply = de_ccsc_apply,
};
