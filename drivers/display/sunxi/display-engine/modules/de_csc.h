/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Colour space conversion maths shared by the CSC stages.
 *
 * A conversion is an affine transform on 10 bit components:
 *     out = M * (in - in_off) + out_off
 * with M in Q16 fixed point. The hardware blocks take M in Q10 and the
 * offsets as 10 bit values.
 */
#ifndef __DE_CSC_H__
#define __DE_CSC_H__

#include <dpy/dpy_kms.h>

#define DE_CSC_ONE	(1 << 16)

struct de_csc {
	int32_t m[3][3];	/* Q16 */
	int32_t in_off[3];	/* 10 bit */
	int32_t out_off[3];	/* 10 bit */
};

void de_csc_identity(struct de_csc *c);
bool de_csc_is_identity(const struct de_csc *c);
/* r = b(a(x)) */
void de_csc_compose(const struct de_csc *a, const struct de_csc *b,
		    struct de_csc *r);
/* YCbCr (@encoding, @range) -> full range RGB */
void de_csc_yuv2rgb(uint8_t encoding, uint8_t range, struct de_csc *c);
/* full range RGB -> full range YCbCr (@encoding) */
void de_csc_rgb2yuv(uint8_t encoding, struct de_csc *c);
/* brightness/contrast/saturation/hue on full range YCbCr */
void de_csc_bcsh(const struct dpy_color_adjust *adj, struct de_csc *c);
bool de_csc_adjust_neutral(const struct dpy_color_adjust *adj);

/* register images of a standard CSC block (ctl, d0..2, c00..c23) */
#define DE_CSC_REG_CTL		0x00
#define DE_CSC_REG_D(i)		(0x04 + (i) * 4)
#define DE_CSC_REG_C(r, c)	(0x10 + (r) * 0x10 + (c) * 4)
#define DE_CSC_BLOCK_SIZE	0x40

struct de_regblk;
void de_csc_write(struct de_regblk *blk, const struct de_csc *c, bool enable);

#endif /* __DE_CSC_H__ */
