// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Graphic scaler (GSU): RGB polyphase scaler of a UI channel, 16 phases,
 * 4 taps, one coefficient set for both directions.
 */
#define DPY_LOG_TAG "de-gsu"
#include "../de_priv.h"
#include "de_scaler.h"

#define GSU_CTRL			0x000
#define   GSU_CTRL_EN			DPY_BIT(0)
#define   GSU_CTRL_COEF_RDY		DPY_BIT(4)
#define GSU_OUT_SIZE			0x040
#define GSU_IN_SIZE			0x080	/* scale block */
#define GSU_HSTEP			0x088
#define GSU_VSTEP			0x08c
#define GSU_HPHASE			0x090
#define GSU_VPHASE0			0x098
#define GSU_COEF			0x200

#define GSU_SIZE(w, h)			(((uint32_t)((h) - 1) << 16) | ((w) - 1))
/* steps and phases sit two bits up in the registers */
#define GSU_REG_FIX(v)			((uint32_t)(v) << 2)

enum {
	GSU_BLK_CTRL,
	GSU_BLK_OUT,
	GSU_BLK_SCALE,
	GSU_BLK_COEF,
	GSU_BLK_NR,
};

static int de_gsu_init(struct de_stage *st)
{
	static const struct {
		uint32_t off, size;
	} blocks[GSU_BLK_NR] = {
		[GSU_BLK_CTRL] = { GSU_CTRL, 0x14 },
		[GSU_BLK_OUT] = { GSU_OUT_SIZE, 0x04 },
		[GSU_BLK_SCALE] = { GSU_IN_SIZE, 0x20 },
		[GSU_BLK_COEF] = { GSU_COEF, 0x40 },
	};
	unsigned int i;

	for (i = 0; i < GSU_BLK_NR; i++) {
		st->blk[i] = de_regblk_create(st->de, "gsu",
					      st->desc->offset + blocks[i].off,
					      blocks[i].size, true,
					      st->pipe->desc->disp);
		if (!st->blk[i])
			return -ENOMEM;
	}
	st->nblks = GSU_BLK_NR;
	return 0;
}

static int de_gsu_check(struct de_stage *st, struct de_crtc_state *s)
{
	const struct de_chn_plan *cp = &s->chn[st->pipe->index];

	if (!cp->enable || !cp->scale)
		return 0;
	if (cp->in_w > st->desc->param)
		return -ERANGE;
	/* integer part of the step register is 5 bits */
	if ((cp->ys.hstep >> DE_GSU_FRAC) > 31 ||
	    (cp->ys.vstep >> DE_GSU_FRAC) > 31)
		return -ERANGE;
	return 0;
}

static void de_gsu_apply(struct de_stage *st, const struct de_crtc_state *s)
{
	const struct de_chn_plan *cp = &s->chn[st->pipe->index];
	struct de_regblk *scale = st->blk[GSU_BLK_SCALE];
	unsigned int bucket, i;

	if (!cp->enable || !cp->scale) {
		de_rb_write(st->blk[GSU_BLK_CTRL], GSU_CTRL, 0);
		return;
	}

	de_rb_write(st->blk[GSU_BLK_OUT], 0, GSU_SIZE(cp->bld.w, cp->bld.h));
	de_rb_write(scale, GSU_IN_SIZE - GSU_IN_SIZE,
		    GSU_SIZE(cp->in_w, cp->in_h));
	de_rb_write(scale, GSU_HSTEP - GSU_IN_SIZE, GSU_REG_FIX(cp->ys.hstep));
	de_rb_write(scale, GSU_VSTEP - GSU_IN_SIZE, GSU_REG_FIX(cp->ys.vstep));
	de_rb_write(scale, GSU_HPHASE - GSU_IN_SIZE, GSU_REG_FIX(cp->ys.hphase));
	de_rb_write(scale, GSU_VPHASE0 - GSU_IN_SIZE, GSU_REG_FIX(cp->ys.vphase));

	/* one filter for both axes: size it for the stronger reduction */
	bucket = de_scaler_bucket(DPY_MAX(cp->ys.hstep, cp->ys.vstep),
				  DE_GSU_FRAC);
	for (i = 0; i < DE_GSU_PHASES; i++)
		de_rb_write(st->blk[GSU_BLK_COEF], i * 4,
			    de_gsu_coef[bucket * DE_GSU_PHASES + i]);

	de_rb_write(st->blk[GSU_BLK_CTRL], GSU_CTRL,
		    GSU_CTRL_EN | GSU_CTRL_COEF_RDY);
	/* new taps are only taken over with COEF_RDY: resend it with them */
	if (st->blk[GSU_BLK_COEF]->dirty)
		st->blk[GSU_BLK_CTRL]->dirty = true;
}

static void de_gsu_dump(struct de_stage *st, void (*print)(const char *fmt, ...))
{
	print("    ctrl %08x out %08x in %08x step %08x/%08x\n",
	      de_rb_read(st->blk[GSU_BLK_CTRL], 0),
	      de_rb_read(st->blk[GSU_BLK_OUT], 0),
	      de_rb_read(st->blk[GSU_BLK_SCALE], 0),
	      de_rb_read(st->blk[GSU_BLK_SCALE], GSU_HSTEP - GSU_IN_SIZE),
	      de_rb_read(st->blk[GSU_BLK_SCALE], GSU_VSTEP - GSU_IN_SIZE));
}

const struct de_stage_ops de_gsu_ops = {
	.init = de_gsu_init,
	.check = de_gsu_check,
	.apply = de_gsu_apply,
	.dump = de_gsu_dump,
};
