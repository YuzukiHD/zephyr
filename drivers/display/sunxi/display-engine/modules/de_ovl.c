// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Overlay stages: the first stage of every channel. An overlay fetches up
 * to four layers from memory and stacks them into one window that feeds
 * the channel scaler (or directly the blender).
 *
 *  - video overlay (VI): RGB or YUV layers; the alpha value, the RGB/YUV
 *    selection and (for YUV) the pixel format are shared by all layers and
 *    taken from layer 0; coarse (m/n) decimation before the scaler.
 *  - UI overlay: RGB layers only; the alpha value is shared.
 */
#define DPY_LOG_TAG "de-ovl"
#include "../de_priv.h"

/* ---- layer attribute word (both overlay types) ---- */
#define OVL_ATTR_EN			DPY_BIT(0)
#define OVL_ATTR_ALPHA_MODE		DPY_GENMASK(2, 1)
#define OVL_ATTR_FCOLOR_EN		DPY_BIT(4)
#define OVL_ATTR_FMT			DPY_GENMASK(12, 8)
#define OVL_ATTR_UI_SEL			DPY_BIT(15)	/* VI only */
#define OVL_ATTR_ALPHA_CTL		DPY_GENMASK(17, 16)
#define OVL_ATTR_TOP_DOWN		DPY_BIT(23)
#define OVL_ATTR_ALPHA			DPY_GENMASK(31, 24)

#define OVL_SIZE_W			DPY_GENMASK(12, 0)
#define OVL_SIZE_H			DPY_GENMASK(28, 16)
#define OVL_COOR_X			DPY_GENMASK(15, 0)
#define OVL_COOR_Y			DPY_GENMASK(31, 16)
#define OVL_DS_M			DPY_GENMASK(13, 0)
#define OVL_DS_N			DPY_GENMASK(29, 16)

/* ---- video overlay: 4 layers of 0x30 bytes ---- */
#define VI_LAYER_STRIDE			0x30
#define VI_ATTR				0x00
#define VI_SIZE				0x04
#define VI_COOR				0x08
#define VI_PITCH(p)			(0x0c + (p) * 4)
#define VI_TOP_LADDR(p)			(0x18 + (p) * 4)
#define VI_BOT_LADDR(p)			(0x24 + (p) * 4)
#define VI_FCOLOR_BLOCK			0xc0	/* 4 words */
#define VI_MISC_BLOCK			0xd0	/* haddr, overlay size, decimation */
#define VI_TOP_HADDR(p)			(0x00 + (p) * 4)	/* in misc */
#define VI_BOT_HADDR(p)			(0x0c + (p) * 4)
#define VI_OVL_SIZE(c)			(0x18 + (c) * 4)	/* c: luma, chroma */
#define VI_HORI_DS(c)			(0x20 + (c) * 4)
#define VI_VERT_DS(c)			(0x28 + (c) * 4)

/* ---- UI overlay: 4 layers of 0x20 bytes ---- */
#define UI_LAYER_STRIDE			0x20
#define UI_ATTR				0x00
#define UI_SIZE				0x04
#define UI_COOR				0x08
#define UI_PITCH			0x0c
#define UI_TOP_LADDR			0x10
#define UI_BOT_LADDR			0x14
#define UI_FCOLOR			0x18
#define UI_MISC_BLOCK			0x80
#define UI_TOP_HADDR			0x00	/* in misc */
#define UI_BOT_HADDR			0x04
#define UI_OVL_SIZE			0x08

/* ---- overlay pixel format codes ---- */
static const struct {
	uint8_t rgb;		/* RGB code, 0xff: not RGB */
	uint8_t yuv;		/* video overlay YUV code, 0xff: not YUV */
	uint8_t sub;
} de_ovl_formats[DISPLAY_FORMAT_COUNT] = {
	[DISPLAY_FORMAT_INVALID] = { 0xff, 0xff, 0 },
	[DISPLAY_FORMAT_ARGB8888] = { 0x00, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_ABGR8888] = { 0x01, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_RGBA8888] = { 0x02, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_BGRA8888] = { 0x03, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_XRGB8888] = { 0x04, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_XBGR8888] = { 0x05, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_RGBX8888] = { 0x06, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_BGRX8888] = { 0x07, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_RGB888] = { 0x08, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_BGR888] = { 0x09, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_RGB565] = { 0x0a, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_BGR565] = { 0x0b, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_ARGB4444] = { 0x0c, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_ABGR4444] = { 0x0d, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_RGBA4444] = { 0x0e, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_BGRA4444] = { 0x0f, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_ARGB1555] = { 0x10, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_ABGR1555] = { 0x11, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_RGBA5551] = { 0x12, 0xff, DE_SUB_RGB },
	[DISPLAY_FORMAT_BGRA5551] = { 0x13, 0xff, DE_SUB_RGB },
	/*
	 * Packed 4:2:2: the hardware names the component order of a pixel
	 * word (MSB first) while the API uses memory byte order.
	 */
	[DISPLAY_FORMAT_YUYV] = { 0xff, 0x00, DE_SUB_422 },
	[DISPLAY_FORMAT_UYVY] = { 0xff, 0x01, DE_SUB_422 },
	[DISPLAY_FORMAT_YVYU] = { 0xff, 0x02, DE_SUB_422 },
	[DISPLAY_FORMAT_VYUY] = { 0xff, 0x03, DE_SUB_422 },
	[DISPLAY_FORMAT_NV16] = { 0xff, 0x04, DE_SUB_422 },
	[DISPLAY_FORMAT_NV61] = { 0xff, 0x05, DE_SUB_422 },
	[DISPLAY_FORMAT_YUV422] = { 0xff, 0x06, DE_SUB_422 },
	[DISPLAY_FORMAT_NV12] = { 0xff, 0x08, DE_SUB_420 },
	[DISPLAY_FORMAT_NV21] = { 0xff, 0x09, DE_SUB_420 },
	[DISPLAY_FORMAT_YUV420] = { 0xff, 0x0a, DE_SUB_420 },
	[DISPLAY_FORMAT_YVU420] = { 0xff, 0x0a, DE_SUB_420 },
	[DISPLAY_FORMAT_NV411] = { 0xff, 0x0c, DE_SUB_411 },
	[DISPLAY_FORMAT_NV114] = { 0xff, 0x0d, DE_SUB_411 },
	[DISPLAY_FORMAT_YUV411] = { 0xff, 0x0e, DE_SUB_411 },
};

int de_ovl_format(uint32_t format, bool video, uint8_t *code, bool *ui_sel,
		  uint8_t *sub)
{
	if (format >= DISPLAY_FORMAT_COUNT)
		return -EINVAL;
	if (de_ovl_formats[format].rgb != 0xff) {
		*code = de_ovl_formats[format].rgb;
		*ui_sel = true;
		*sub = DE_SUB_RGB;
		return 0;
	}
	if (!video || de_ovl_formats[format].yuv == 0xff)
		return -EINVAL;
	*code = de_ovl_formats[format].yuv;
	*ui_sel = false;
	*sub = de_ovl_formats[format].sub;
	return 0;
}

struct de_ovl {
	bool video;
	uint8_t nlayers;
	struct de_regblk *layer[DE_MAX_LAYERS];
	struct de_regblk *fcolor;	/* video only */
	struct de_regblk *misc;
};

static uint32_t de_ovl_attr(const struct de_layer_plan *l,
			    const struct de_chn_plan *cp, unsigned int idx,
			    bool video)
{
	uint32_t attr = DPY_FIELD_PREP(OVL_ATTR_ALPHA, cp->alpha);

	if (l->enable)
		attr |= OVL_ATTR_EN |
			DPY_FIELD_PREP(OVL_ATTR_ALPHA_MODE, l->alpha_mode) |
			DPY_FIELD_PREP(OVL_ATTR_ALPHA_CTL, l->alpha_ctl) |
			DPY_FIELD_PREP(OVL_ATTR_FMT, l->hw_fmt);

	/*
	 * Layer 0 carries the channel-wide settings even when disabled:
	 * alpha, RGB/YUV selection and, for YUV, the pixel format.
	 */
	if (video) {
		const struct de_layer_plan *first = NULL;
		unsigned int i;

		for (i = 0; i < cp->nlayers; i++)
			if (cp->layer[i].enable) {
				first = &cp->layer[i];
				break;
			}
		if (first) {
			if (first->ui_sel)
				attr |= OVL_ATTR_UI_SEL;
			if (idx == 0 && !l->enable)
				attr |= DPY_FIELD_PREP(OVL_ATTR_FMT,
						       first->hw_fmt);
		}
	}
	return attr;
}

static int de_ovl_init(struct de_stage *st)
{
	struct de_ovl *ovl;
	uint32_t base = st->desc->offset;
	uint8_t disp = st->pipe->desc->disp;
	unsigned int i;

	ovl = dpy_os_zalloc(sizeof(*ovl));
	if (!ovl)
		return -ENOMEM;
	ovl->video = st->desc->kind == DE_STAGE_OVL_VI;
	ovl->nlayers = (uint8_t)DPY_MIN(st->desc->param, DE_MAX_LAYERS);
	st->priv = ovl;

	for (i = 0; i < ovl->nlayers; i++) {
		ovl->layer[i] = ovl->video ?
			de_regblk_create(st->de, "vi-layer",
					 base + i * VI_LAYER_STRIDE,
					 VI_LAYER_STRIDE, true, disp) :
			de_regblk_create(st->de, "ui-layer",
					 base + i * UI_LAYER_STRIDE,
					 UI_LAYER_STRIDE, true, disp);
		if (!ovl->layer[i])
			return -ENOMEM;
		st->blk[st->nblks++] = ovl->layer[i];
	}
	if (ovl->video) {
		ovl->fcolor = de_regblk_create(st->de, "vi-fcolor",
					       base + VI_FCOLOR_BLOCK, 0x10,
					       true, disp);
		ovl->misc = de_regblk_create(st->de, "vi-misc",
					     base + VI_MISC_BLOCK, 0x30, true,
					     disp);
		if (!ovl->fcolor || !ovl->misc)
			return -ENOMEM;
		st->blk[st->nblks++] = ovl->fcolor;
	} else {
		ovl->misc = de_regblk_create(st->de, "ui-misc",
					     base + UI_MISC_BLOCK, 0x10, true,
					     disp);
		if (!ovl->misc)
			return -ENOMEM;
	}
	st->blk[st->nblks++] = ovl->misc;
	return 0;
}

static void de_ovl_apply_vi(struct de_ovl *ovl, const struct de_chn_plan *cp)
{
	uint32_t haddr[3] = { 0 };
	unsigned int i, p;

	for (i = 0; i < ovl->nlayers; i++) {
		const struct de_layer_plan *l = &cp->layer[i];
		struct de_regblk *b = ovl->layer[i];

		de_rb_write(b, VI_ATTR, de_ovl_attr(l, cp, i, true));
		if (!l->enable)
			continue;
		de_rb_write(b, VI_SIZE,
			    DPY_FIELD_PREP(OVL_SIZE_W, l->ovl.w - 1) |
			    DPY_FIELD_PREP(OVL_SIZE_H, l->ovl.h - 1));
		de_rb_write(b, VI_COOR,
			    DPY_FIELD_PREP(OVL_COOR_X, l->ovl.x) |
			    DPY_FIELD_PREP(OVL_COOR_Y, l->ovl.y));
		for (p = 0; p < 3; p++) {
			uint64_t a = p < l->nplanes ? (uint64_t)l->addr[p] : 0;

			de_rb_write(b, VI_PITCH(p), p < l->nplanes ? l->pitch[p] : 0);
			de_rb_write(b, VI_TOP_LADDR(p), (uint32_t)a);
			de_rb_write(b, VI_BOT_LADDR(p), 0);
			haddr[p] |= (uint32_t)((a >> 32) & 0xff) << (8 * i);
		}
	}
	for (p = 0; p < 3; p++) {
		de_rb_write(ovl->misc, VI_TOP_HADDR(p), haddr[p]);
		de_rb_write(ovl->misc, VI_BOT_HADDR(p), 0);
	}
	de_rb_write(ovl->misc, VI_OVL_SIZE(0),
		    cp->enable ? DPY_FIELD_PREP(OVL_SIZE_W, cp->ovl_w - 1) |
				 DPY_FIELD_PREP(OVL_SIZE_H, cp->ovl_h - 1) : 0);
	de_rb_write(ovl->misc, VI_HORI_DS(0),
		    DPY_FIELD_PREP(OVL_DS_M, cp->yhm) | DPY_FIELD_PREP(OVL_DS_N, cp->yhn));
	de_rb_write(ovl->misc, VI_HORI_DS(1),
		    DPY_FIELD_PREP(OVL_DS_M, cp->chm) | DPY_FIELD_PREP(OVL_DS_N, cp->chn));
	de_rb_write(ovl->misc, VI_VERT_DS(0),
		    DPY_FIELD_PREP(OVL_DS_M, cp->yvm) | DPY_FIELD_PREP(OVL_DS_N, cp->yvn));
	de_rb_write(ovl->misc, VI_VERT_DS(1),
		    DPY_FIELD_PREP(OVL_DS_M, cp->cvm) | DPY_FIELD_PREP(OVL_DS_N, cp->cvn));
	/* fill colours are unused: layers always come from memory */
	for (i = 0; i < 4; i++)
		de_rb_write(ovl->fcolor, i * 4, 0);
}

static void de_ovl_apply_ui(struct de_ovl *ovl, const struct de_chn_plan *cp)
{
	uint32_t haddr = 0;
	unsigned int i;

	for (i = 0; i < ovl->nlayers; i++) {
		const struct de_layer_plan *l = &cp->layer[i];
		struct de_regblk *b = ovl->layer[i];
		uint64_t a = l->addr[0];

		de_rb_write(b, UI_ATTR, de_ovl_attr(l, cp, i, false));
		if (!l->enable)
			continue;
		de_rb_write(b, UI_SIZE,
			    DPY_FIELD_PREP(OVL_SIZE_W, l->ovl.w - 1) |
			    DPY_FIELD_PREP(OVL_SIZE_H, l->ovl.h - 1));
		de_rb_write(b, UI_COOR,
			    DPY_FIELD_PREP(OVL_COOR_X, l->ovl.x) |
			    DPY_FIELD_PREP(OVL_COOR_Y, l->ovl.y));
		de_rb_write(b, UI_PITCH, l->pitch[0]);
		de_rb_write(b, UI_TOP_LADDR, (uint32_t)a);
		de_rb_write(b, UI_BOT_LADDR, 0);
		de_rb_write(b, UI_FCOLOR, 0);
		haddr |= (uint32_t)((a >> 32) & 0xff) << (8 * i);
	}
	de_rb_write(ovl->misc, UI_TOP_HADDR, haddr);
	de_rb_write(ovl->misc, UI_BOT_HADDR, 0);
	de_rb_write(ovl->misc, UI_OVL_SIZE,
		    cp->enable ? DPY_FIELD_PREP(OVL_SIZE_W, cp->ovl_w - 1) |
				 DPY_FIELD_PREP(OVL_SIZE_H, cp->ovl_h - 1) : 0);
}

static void de_ovl_apply(struct de_stage *st, const struct de_crtc_state *s)
{
	struct de_ovl *ovl = st->priv;
	const struct de_chn_plan *cp = &s->chn[st->pipe->index];

	if (ovl->video)
		de_ovl_apply_vi(ovl, cp);
	else
		de_ovl_apply_ui(ovl, cp);
}

static void de_ovl_dump(struct de_stage *st, void (*print)(const char *fmt, ...))
{
	struct de_ovl *ovl = st->priv;
	unsigned int i;

	for (i = 0; i < ovl->nlayers; i++) {
		const struct de_regblk *b = ovl->layer[i];

		print("    layer%u: attr %08x size %08x coor %08x pitch %08x addr %08x\n",
		      i, de_rb_read(b, 0), de_rb_read(b, 4), de_rb_read(b, 8),
		      de_rb_read(b, 0xc),
		      de_rb_read(b, ovl->video ? VI_TOP_LADDR(0) : UI_TOP_LADDR));
	}
	print("    ovl size %08x\n",
	      de_rb_read(ovl->misc, ovl->video ? VI_OVL_SIZE(0) : UI_OVL_SIZE));
}

const struct de_stage_ops de_ovl_vi_ops = {
	.init = de_ovl_init,
	.apply = de_ovl_apply,
	.dump = de_ovl_dump,
};

const struct de_stage_ops de_ovl_ui_ops = {
	.init = de_ovl_init,
	.apply = de_ovl_apply,
	.dump = de_ovl_dump,
};
