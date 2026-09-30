// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Video scaler (VSU): polyphase scaler of the video channel with separate
 * luma and chroma paths, 32 phases, 8 horizontal and 4 vertical taps.
 */
#define DPY_LOG_TAG "de-vsu"
#include "../de_priv.h"
#include "de_scaler.h"

#define VSU_CTRL			0x000
#define   VSU_CTRL_EN			DPY_BIT(0)
#define   VSU_CTRL_COEF_RDY		DPY_BIT(4)
#define VSU_OUT_SIZE			0x040
#define VSU_Y_SIZE			0x080	/* in the Y block */
#define VSU_Y_HSTEP			0x088
#define VSU_Y_VSTEP			0x08c
#define VSU_Y_HPHASE			0x090
#define VSU_Y_VPHASE0			0x098
#define VSU_C_SIZE			0x0c0	/* in the C block */
#define VSU_C_HSTEP			0x0c8
#define VSU_C_VSTEP			0x0cc
#define VSU_C_HPHASE			0x0d0
#define VSU_C_VPHASE0			0x0d8
#define VSU_Y_HCOEF0			0x200
#define VSU_Y_HCOEF1			0x300
#define VSU_Y_VCOEF			0x400
#define VSU_C_HCOEF0			0x600
#define VSU_C_HCOEF1			0x700
#define VSU_C_VCOEF			0x800

#define VSU_SIZE(w, h)			(((uint32_t)((h) - 1) << 16) | ((w) - 1))
/* steps and phases sit one bit up in the registers */
#define VSU_REG_FIX(v)			((uint32_t)(v) << 1)

enum {
	VSU_BLK_CTRL,
	VSU_BLK_OUT,
	VSU_BLK_Y,
	VSU_BLK_C,
	VSU_BLK_YH0,
	VSU_BLK_YH1,
	VSU_BLK_YV,
	VSU_BLK_CH0,
	VSU_BLK_CH1,
	VSU_BLK_CV,
	VSU_BLK_NR,
};

static const struct {
	uint32_t off;
	uint32_t size;
} vsu_blocks[VSU_BLK_NR] = {
	[VSU_BLK_CTRL] = { VSU_CTRL, 0x10 },
	[VSU_BLK_OUT] = { VSU_OUT_SIZE, 0x04 },
	[VSU_BLK_Y] = { VSU_Y_SIZE, 0x20 },
	[VSU_BLK_C] = { VSU_C_SIZE, 0x20 },
	[VSU_BLK_YH0] = { VSU_Y_HCOEF0, 0x80 },
	[VSU_BLK_YH1] = { VSU_Y_HCOEF1, 0x80 },
	[VSU_BLK_YV] = { VSU_Y_VCOEF, 0x80 },
	[VSU_BLK_CH0] = { VSU_C_HCOEF0, 0x80 },
	[VSU_BLK_CH1] = { VSU_C_HCOEF1, 0x80 },
	[VSU_BLK_CV] = { VSU_C_VCOEF, 0x80 },
};

static int de_vsu_init(struct de_stage *st)
{
	unsigned int i;

	for (i = 0; i < VSU_BLK_NR; i++) {
		st->blk[i] = de_regblk_create(st->de, "vsu",
					      st->desc->offset + vsu_blocks[i].off,
					      vsu_blocks[i].size, true,
					      st->pipe->desc->disp);
		if (!st->blk[i])
			return -ENOMEM;
	}
	st->nblks = VSU_BLK_NR;
	return 0;
}

static int de_vsu_check(struct de_stage *st, struct de_crtc_state *s)
{
	const struct de_chn_plan *cp = &s->chn[st->pipe->index];

	if (!cp->enable || !cp->scale)
		return 0;
	if (cp->in_w > st->desc->param) {
		dpy_dbg("scaler input %u wider than line buffer %u\n",
			cp->in_w, st->desc->param);
		return -ERANGE;
	}
	/* integer part of the step register is 4 bits */
	if ((cp->ys.hstep >> DE_VSU_FRAC) > 15 ||
	    (cp->ys.vstep >> DE_VSU_FRAC) > 15)
		return -ERANGE;
	return 0;
}

static void de_vsu_load_coef(struct de_regblk *blk, const uint32_t *table,
			     unsigned int bucket)
{
	unsigned int i;

	for (i = 0; i < DE_VSU_PHASES; i++)
		de_rb_write(blk, i * 4, table[bucket * DE_VSU_PHASES + i]);
}

static void de_vsu_apply(struct de_stage *st, const struct de_crtc_state *s)
{
	const struct de_chn_plan *cp = &s->chn[st->pipe->index];
	bool rgb = cp->sub == DE_SUB_RGB;
	unsigned int i;

	if (!cp->enable || !cp->scale) {
		de_rb_write(st->blk[VSU_BLK_CTRL], VSU_CTRL, 0);
		return;
	}

	de_rb_write(st->blk[VSU_BLK_OUT], 0, VSU_SIZE(cp->bld.w, cp->bld.h));

	de_rb_write(st->blk[VSU_BLK_Y], VSU_Y_SIZE - VSU_Y_SIZE,
		    VSU_SIZE(cp->in_w, cp->in_h));
	de_rb_write(st->blk[VSU_BLK_Y], VSU_Y_HSTEP - VSU_Y_SIZE,
		    VSU_REG_FIX(cp->ys.hstep));
	de_rb_write(st->blk[VSU_BLK_Y], VSU_Y_VSTEP - VSU_Y_SIZE,
		    VSU_REG_FIX(cp->ys.vstep));
	de_rb_write(st->blk[VSU_BLK_Y], VSU_Y_HPHASE - VSU_Y_SIZE,
		    VSU_REG_FIX(cp->ys.hphase));
	de_rb_write(st->blk[VSU_BLK_Y], VSU_Y_VPHASE0 - VSU_Y_SIZE,
		    VSU_REG_FIX(cp->ys.vphase));

	de_rb_write(st->blk[VSU_BLK_C], VSU_C_SIZE - VSU_C_SIZE,
		    VSU_SIZE(cp->in_cw, cp->in_ch));
	de_rb_write(st->blk[VSU_BLK_C], VSU_C_HSTEP - VSU_C_SIZE,
		    VSU_REG_FIX(cp->cs.hstep));
	de_rb_write(st->blk[VSU_BLK_C], VSU_C_VSTEP - VSU_C_SIZE,
		    VSU_REG_FIX(cp->cs.vstep));
	de_rb_write(st->blk[VSU_BLK_C], VSU_C_HPHASE - VSU_C_SIZE,
		    VSU_REG_FIX(cp->cs.hphase));
	de_rb_write(st->blk[VSU_BLK_C], VSU_C_VPHASE0 - VSU_C_SIZE,
		    VSU_REG_FIX(cp->cs.vphase));

	de_vsu_load_coef(st->blk[VSU_BLK_YH0], de_vsu_coef_y_h0,
			 de_scaler_bucket(cp->ys.hstep, DE_VSU_FRAC));
	de_vsu_load_coef(st->blk[VSU_BLK_YH1], de_vsu_coef_y_h1,
			 de_scaler_bucket(cp->ys.hstep, DE_VSU_FRAC));
	de_vsu_load_coef(st->blk[VSU_BLK_YV], de_vsu_coef_y_v,
			 de_scaler_bucket(cp->ys.vstep, DE_VSU_FRAC));
	/* RGB goes through the chroma path too: use the sharp luma filter */
	de_vsu_load_coef(st->blk[VSU_BLK_CH0],
			 rgb ? de_vsu_coef_y_h0 : de_vsu_coef_c_h0,
			 de_scaler_bucket(cp->cs.hstep, DE_VSU_FRAC));
	de_vsu_load_coef(st->blk[VSU_BLK_CH1],
			 rgb ? de_vsu_coef_y_h1 : de_vsu_coef_c_h1,
			 de_scaler_bucket(cp->cs.hstep, DE_VSU_FRAC));
	de_vsu_load_coef(st->blk[VSU_BLK_CV],
			 rgb ? de_vsu_coef_y_v : de_vsu_coef_c_v,
			 de_scaler_bucket(cp->cs.vstep, DE_VSU_FRAC));

	de_rb_write(st->blk[VSU_BLK_CTRL], VSU_CTRL,
		    VSU_CTRL_EN | VSU_CTRL_COEF_RDY);
	/* new taps are only taken over with COEF_RDY: resend it with them */
	for (i = VSU_BLK_YH0; i <= VSU_BLK_CV; i++)
		if (st->blk[i]->dirty)
			st->blk[VSU_BLK_CTRL]->dirty = true;
}

static void de_vsu_dump(struct de_stage *st, void (*print)(const char *fmt, ...))
{
	print("    ctrl %08x out %08x y %08x ystep %08x/%08x c %08x\n",
	      de_rb_read(st->blk[VSU_BLK_CTRL], 0),
	      de_rb_read(st->blk[VSU_BLK_OUT], 0),
	      de_rb_read(st->blk[VSU_BLK_Y], 0),
	      de_rb_read(st->blk[VSU_BLK_Y], VSU_Y_HSTEP - VSU_Y_SIZE),
	      de_rb_read(st->blk[VSU_BLK_Y], VSU_Y_VSTEP - VSU_Y_SIZE),
	      de_rb_read(st->blk[VSU_BLK_C], 0));
}

const struct de_stage_ops de_vsu_ops = {
	.init = de_vsu_init,
	.check = de_vsu_check,
	.apply = de_vsu_apply,
	.dump = de_vsu_dump,
};
