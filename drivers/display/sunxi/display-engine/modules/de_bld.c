// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Blender: stacks the channel outputs ("pipes") bottom to top with
 * Porter-Duff source-over on a background colour.
 *
 * Each pipe has an input window on the screen and is fed by one channel
 * (route). Pipe 0 is the bottom one; its fill colour provides an opaque
 * base so that translucent pixels of the bottom channel blend with the
 * background colour instead of black.
 */
#define DPY_LOG_TAG "de-bld"
#include "../de_priv.h"

/* attribute block: pipe enables and per pipe fill colour/window */
#define BLD_FILL_CTL			0x000
#define   BLD_PIPE_FILL_EN(p)		DPY_BIT(p)
#define   BLD_PIPE_EN(p)		DPY_BIT(8 + (p))
#define BLD_PIPE_FCOLOR(p)		(0x004 + (p) * 0x10)
#define BLD_PIPE_SIZE(p)		(0x008 + (p) * 0x10)
#define BLD_PIPE_OFFSET(p)		(0x00c + (p) * 0x10)
/* control block at +0x80 */
#define BLD_CTL_BLOCK			0x080
#define BLD_ROUTE			0x000
#define   BLD_ROUTE_PIPE(p)		DPY_GENMASK((p) * 4 + 3, (p) * 4)
#define BLD_PREMUL			0x004
#define BLD_BKCOLOR			0x008
#define BLD_OUT_SIZE			0x00c
#define BLD_MODE(n)			(0x010 + (n) * 4)
/* colour key / output block at +0xb0 */
#define BLD_CK_BLOCK			0x0b0
#define BLD_CK_CTL			0x000
#define BLD_OUT_CTL			0x04c

#define BLD_SIZE(w, h)			(((uint32_t)((h) - 1) << 16) | ((w) - 1))
#define BLD_POS(x, y)			(((uint32_t)(y) << 16) | ((uint32_t)(x) & 0xffff))

/*
 * Blend equation word: colour/alpha source and destination factors.
 * Source over: Cs * 1 + Cd * (1 - As) for colour and alpha.
 */
#define BLD_FACTOR_ZERO			0
#define BLD_FACTOR_ONE			1
#define BLD_FACTOR_ONE_MINUS_SRC_A	3
#define BLD_EQ(cs, cd, as, ad) \
	((uint32_t)(cs) | ((uint32_t)(cd) << 8) | ((uint32_t)(as) << 16) | \
	 ((uint32_t)(ad) << 24))
#define BLD_SRC_OVER \
	BLD_EQ(BLD_FACTOR_ONE, BLD_FACTOR_ONE_MINUS_SRC_A, \
	       BLD_FACTOR_ONE, BLD_FACTOR_ONE_MINUS_SRC_A)

enum {
	BLD_BLK_ATTR,
	BLD_BLK_CTL,
	BLD_BLK_CK,
	BLD_BLK_NR,
};

static int de_bld_init(struct de_stage *st)
{
	uint8_t disp = st->pipe->desc->disp;
	uint32_t base = st->desc->offset;

	st->blk[BLD_BLK_ATTR] = de_regblk_create(st->de, "bld-attr", base,
						 0x50, true, disp);
	st->blk[BLD_BLK_CTL] = de_regblk_create(st->de, "bld-ctl",
						base + BLD_CTL_BLOCK, 0x20,
						true, disp);
	st->blk[BLD_BLK_CK] = de_regblk_create(st->de, "bld-ck",
					       base + BLD_CK_BLOCK, 0x50, true,
					       disp);
	if (!st->blk[BLD_BLK_ATTR] || !st->blk[BLD_BLK_CTL] ||
	    !st->blk[BLD_BLK_CK])
		return -ENOMEM;
	st->nblks = BLD_BLK_NR;
	return 0;
}

static int de_bld_check(struct de_stage *st, struct de_crtc_state *s)
{
	/* the blender has as many pipes as the engine has channels */
	if (s->disp.npipes > st->desc->param)
		return -ERANGE;
	return 0;
}

static void de_bld_apply(struct de_stage *st, const struct de_crtc_state *s)
{
	const struct de_disp_plan *d = &s->disp;
	struct de_regblk *attr = st->blk[BLD_BLK_ATTR];
	struct de_regblk *ctl = st->blk[BLD_BLK_CTL];
	uint32_t fill = BLD_PIPE_FILL_EN(0), route = 0, premul = 0;
	uint32_t bg = 0xff000000 | (d->background & 0xffffff);
	unsigned int p;

	for (p = 0; p < st->desc->param; p++) {
		const struct de_pipe_plan *pp = &d->pipe[p];

		de_rb_write(attr, BLD_PIPE_FCOLOR(p), p == 0 ? bg : 0xff000000);
		if (p >= d->npipes || !pp->enable)
			continue;
		fill |= BLD_PIPE_EN(p);
		route |= DPY_FIELD_PREP(BLD_ROUTE_PIPE(p), pp->port);
		if (pp->premul)
			premul |= DPY_BIT(p);
		de_rb_write(attr, BLD_PIPE_SIZE(p), BLD_SIZE(pp->rect.w, pp->rect.h));
		de_rb_write(attr, BLD_PIPE_OFFSET(p), BLD_POS(pp->rect.x, pp->rect.y));
	}
	de_rb_write(attr, BLD_FILL_CTL, fill);

	de_rb_write(ctl, BLD_ROUTE, route);
	de_rb_write(ctl, BLD_PREMUL, premul);
	de_rb_write(ctl, BLD_BKCOLOR, bg);
	de_rb_write(ctl, BLD_OUT_SIZE, BLD_SIZE(d->w, d->h));
	for (p = 0; p < 4; p++)
		de_rb_write(ctl, BLD_MODE(p), BLD_SRC_OVER);

	/* no colour key, progressive, non pre-multiplied output */
	de_rb_write(st->blk[BLD_BLK_CK], BLD_CK_CTL, 0);
	de_rb_write(st->blk[BLD_BLK_CK], BLD_OUT_CTL, 0);
}

static void de_bld_dump(struct de_stage *st, void (*print)(const char *fmt, ...))
{
	struct de_regblk *attr = st->blk[BLD_BLK_ATTR];
	struct de_regblk *ctl = st->blk[BLD_BLK_CTL];
	unsigned int p;

	print("    fill %08x route %08x premul %08x bk %08x out %08x\n",
	      de_rb_read(attr, BLD_FILL_CTL), de_rb_read(ctl, BLD_ROUTE),
	      de_rb_read(ctl, BLD_PREMUL), de_rb_read(ctl, BLD_BKCOLOR),
	      de_rb_read(ctl, BLD_OUT_SIZE));
	for (p = 0; p < st->desc->param; p++)
		print("    pipe%u: size %08x offset %08x\n", p,
		      de_rb_read(attr, BLD_PIPE_SIZE(p)),
		      de_rb_read(attr, BLD_PIPE_OFFSET(p)));
}

const struct de_stage_ops de_bld_ops = {
	.init = de_bld_init,
	.check = de_bld_check,
	.apply = de_bld_apply,
	.dump = de_bld_dump,
};
