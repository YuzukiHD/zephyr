// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display engine frontend (channel) planning.
 *
 * Turns the plane states of one channel into a channel plan: per layer
 * crop, addresses and position inside the overlay window, the shared
 * scaler configuration (including coarse decimation) and the blender
 * input window. All hardware constraints of a channel are enforced here:
 *
 *  - the layers share one scaler: same scaling factor for all of them;
 *  - the layers share one alpha value and, on the video channel, one
 *    pixel format class (all RGB, or all the same YUV format);
 *  - the layers of a channel are stacked in layer order: plane z order
 *    has to follow the layer index;
 *  - a channel without scaler cannot scale.
 */
#define DPY_LOG_TAG "de-chn"
#include "de_priv.h"
#include "modules/de_scaler.h"

static uint32_t de_step(uint32_t src_fp, uint32_t dst, unsigned int frac)
{
	return (uint32_t)(((uint64_t)src_fp << frac) / ((uint64_t)dst << 16));
}

/* fraction of a 16.16 value expressed with @frac bits */
static uint32_t de_phase(int32_t v_fp, unsigned int frac)
{
	return ((uint32_t)v_fp & 0xffff) << (frac - 16);
}

/*
 * Chroma subsampled data must start on a chroma sample: move the crop
 * start down to the sample and compensate with the initial phase.
 */
static void de_align_axis(int32_t *pos, uint32_t *len, uint32_t *phase,
			  uint32_t a, unsigned int frac)
{
	uint32_t off = (uint32_t)*pos % a;

	if (!off) {
		*len -= *len % a;
		return;
	}
	*pos -= (int32_t)off;
	*phase += off << frac;
	*len = (*len + off) - (*len + off) % a;
}

static void de_layer_geometry(const struct dpy_plane_state *ps,
			      unsigned int frac, uint8_t sub,
			      struct de_layer_plan *l)
{
	const struct dpy_format_info *fi = ps->fb.info;
	const struct dpy_rect_fp *src = &ps->src_clip;
	uint32_t fx = (uint32_t)src->x & 0xffff;
	uint32_t fy = (uint32_t)src->y & 0xffff;
	unsigned int p;

	l->frame = ps->dst_clip;
	l->ys.hstep = de_step(src->w, l->frame.w, frac);
	l->ys.vstep = de_step(src->h, l->frame.h, frac);
	l->ys.hphase = de_phase(src->x, frac);
	l->ys.vphase = de_phase(src->y, frac);

	l->crop.x = src->x >> 16;
	l->crop.y = src->y >> 16;
	/* every source pixel the window touches, partly covered ones too */
	l->crop.w = (fx + src->w + 0xffff) >> 16;
	l->crop.h = (fy + src->h + 0xffff) >> 16;

	switch (sub) {
	case DE_SUB_422:
		de_align_axis(&l->crop.x, &l->crop.w, &l->ys.hphase, 2, frac);
		break;
	case DE_SUB_420:
		de_align_axis(&l->crop.x, &l->crop.w, &l->ys.hphase, 2, frac);
		de_align_axis(&l->crop.y, &l->crop.h, &l->ys.vphase, 2, frac);
		break;
	case DE_SUB_411:
		de_align_axis(&l->crop.x, &l->crop.w, &l->ys.hphase, 4, frac);
		break;
	default:
		break;
	}

	/* chroma steps and phases count chroma samples */
	l->cs = l->ys;
	switch (sub) {
	case DE_SUB_422:
		l->cs.hstep = l->ys.hstep >> 1;
		l->cs.hphase = l->ys.hphase >> 1;
		break;
	case DE_SUB_420:
		l->cs.hstep = l->ys.hstep >> 1;
		l->cs.vstep = l->ys.vstep >> 1;
		/* MPEG-2 siting: chroma is centred between two luma lines */
		l->cs.hphase = l->ys.hphase >> 1;
		l->cs.vphase = (l->ys.vphase >> 1) - (1U << (frac - 2));
		break;
	case DE_SUB_411:
		l->cs.hstep = l->ys.hstep >> 2;
		l->cs.hphase = l->ys.hphase >> 2;
		break;
	default:
		break;
	}

	l->nplanes = fi->num_planes;
	for (p = 0; p < fi->num_planes; p++) {
		uint32_t hs = p ? fi->hsub : 1, vs = p ? fi->vsub : 1;
		uint32_t x = (uint32_t)l->crop.x / hs, y = (uint32_t)l->crop.y / vs;

		l->pitch[p] = ps->fb.pitch[p];
		l->addr[p] = ps->fb.addr[p] + (dpy_dma_addr_t)y * l->pitch[p] +
			     (dpy_dma_addr_t)x * fi->cpp[p];
	}
	/* YV12 stores V before U; the hardware wants U in plane 1 */
	if (ps->fb.format == DISPLAY_FORMAT_YVU420) {
		dpy_dma_addr_t a = l->addr[1];
		uint32_t pt = l->pitch[1];

		l->addr[1] = l->addr[2];
		l->pitch[1] = l->pitch[2];
		l->addr[2] = a;
		l->pitch[2] = pt;
	}

	l->ovl.w = l->crop.w;
	l->ovl.h = l->crop.h;
}

static int32_t de_ovl_coord(uint32_t v, uint32_t step, unsigned int frac,
			    bool even)
{
	uint32_t r = (uint32_t)(((uint64_t)v * step + (1U << (frac - 1))) >> frac);

	return (int32_t)(even ? (r + 1) & ~1U : r);
}

static void de_chroma_size(uint8_t sub, uint32_t w, uint32_t h,
			   uint32_t *cw, uint32_t *ch)
{
	switch (sub) {
	case DE_SUB_422:
		*cw = (w + 1) >> 1;
		*ch = h;
		break;
	case DE_SUB_420:
		*cw = (w + 1) >> 1;
		*ch = (h + 1) >> 1;
		break;
	case DE_SUB_411:
		*cw = (w + 3) >> 2;
		*ch = h;
		break;
	default:
		*cw = w;
		*ch = h;
		break;
	}
}

/* keep scaler windows above the minimum size the scalers support */
static void de_fix_min_size(struct de_chn_plan *cp, unsigned int frac,
			    const struct dpy_display_mode *mode)
{
	uint32_t one = 1U << frac;
	uint32_t org;
	unsigned int i;

	if (cp->ovl_w < DE_SCALER_MIN_W || cp->bld.w < DE_SCALER_MIN_W) {
		org = cp->bld.w;
		if (cp->ys.hstep > one) {
			cp->bld.w = DE_SCALER_MIN_W;
			cp->ovl_w = (cp->ys.hstep * DE_SCALER_MIN_W) >> frac;
		} else {
			cp->ovl_w = DE_SCALER_MIN_W;
			cp->bld.w = DE_SCALER_MIN_W * one / cp->ys.hstep;
		}
		if (cp->bld.x + (int32_t)cp->bld.w > mode->hdisplay) {
			cp->bld.x -= (int32_t)(cp->bld.w - org);
			for (i = 0; i < cp->nlayers; i++)
				if (cp->layer[i].enable)
					cp->layer[i].ovl.x += (int32_t)((cp->ys.hstep *
						(cp->bld.w - org)) >> frac);
		}
	}
	if (cp->ovl_h < DE_SCALER_MIN_H || cp->bld.h < DE_SCALER_MIN_H) {
		org = cp->bld.h;
		if (cp->ys.vstep > one) {
			cp->bld.h = DE_SCALER_MIN_H;
			cp->ovl_h = (cp->ys.vstep * DE_SCALER_MIN_H) >> frac;
		} else {
			cp->ovl_h = DE_SCALER_MIN_H;
			cp->bld.h = DE_SCALER_MIN_H * one / cp->ys.vstep;
		}
		if (cp->bld.y + (int32_t)cp->bld.h > mode->vdisplay) {
			cp->bld.y -= (int32_t)(cp->bld.h - org);
			for (i = 0; i < cp->nlayers; i++)
				if (cp->layer[i].enable)
					cp->layer[i].ovl.y += (int32_t)((cp->ys.vstep *
						(cp->bld.h - org)) >> frac);
		}
	}
}

/*
 * Coarse decimation in front of the video scaler: keep the scaler input
 * within the line buffer and the fetch rate within what the engine can
 * read during one output line (estimated at 80 % DRAM efficiency).
 */
static void de_coarse(struct de_pipeline *pipe, struct de_chn_plan *cp,
		      const struct dpy_display_mode *mode)
{
	uint32_t wshift = 0, hshift = 0;
	uint32_t out_w = cp->bld.w, out_h = cp->bld.h;
	uint32_t in_w, in_h, n;
	uint32_t fps = (dpy_mode_vrefresh_mhz(mode) + 500) / 1000;
	uint32_t de_mhz = pipe->de->soc->core_clk_hz / 1000000;
	uint32_t linebuf = pipe->line_buffer;
	uint64_t need, ability;

	switch (cp->sub) {
	case DE_SUB_422:
		wshift = 1;
		break;
	case DE_SUB_420:
		wshift = 1;
		hshift = 1;
		break;
	case DE_SUB_411:
		wshift = 2;
		break;
	default:
		break;
	}

	in_w = cp->ovl_w & ~((1U << wshift) - 1);
	in_h = cp->ovl_h & ~((1U << hshift) - 1);

	n = 0;
	if (in_w > linebuf && in_w > 8 * out_w)
		n = DPY_MIN(linebuf, 8 * out_w);
	else if (in_w > linebuf)
		n = linebuf;
	else if (in_w > 8 * out_w)
		n = 8 * out_w;
	if (n) {
		n &= ~((1U << wshift) - 1);
		cp->yhm = cp->chm = in_w;
		cp->yhn = cp->chn = n;
		in_w = n;
		cp->ys.hstep = (uint32_t)(((uint64_t)in_w << DE_VSU_FRAC) / out_w);
		cp->cs.hstep = (uint32_t)(((uint64_t)(in_w >> wshift) <<
					   DE_VSU_FRAC) / out_w);
		cp->ys.hphase = 0;
		cp->cs.hphase = 0;
	}

	/* the vertical filter shrinks by 4 at most, bandwidth may ask for less */
	need = (uint64_t)mode->vdisplay * fps * DPY_MAX(in_w, out_w);
	ability = need ? (uint64_t)de_mhz * 80000000ULL / need : 0;
	n = in_h > 4 * out_h ? 4 * out_h : 0;
	if (need && ability < (uint64_t)in_h * 100 / out_h) {
		uint32_t bw = (uint32_t)(ability * out_h / 100);

		if (bw && (!n || bw < n))
			n = bw;
	}
	if (n) {
		n = DPY_MAX(n & ~((1U << hshift) - 1), 1U << hshift);
		cp->yvm = cp->cvm = in_h;
		cp->yvn = cp->cvn = n;
		in_h = n;
		cp->ys.vstep = (uint32_t)(((uint64_t)in_h << DE_VSU_FRAC) / out_h);
		cp->cs.vstep = (uint32_t)(((uint64_t)(in_h >> hshift) <<
					   DE_VSU_FRAC) / out_h);
		cp->ys.vphase = 0;
		cp->cs.vphase = 0;
	}

	cp->in_w = in_w;
	cp->in_h = in_h;
	de_chroma_size(cp->sub, in_w, in_h, &cp->in_cw, &cp->in_ch);
}

int de_channel_plan(struct de_pipeline *pipe, struct de_crtc_state *s,
		    struct dpy_plane_state *const *layers,
		    const struct dpy_display_mode *mode)
{
	struct de_chn_plan *cp = &s->chn[pipe->index];
	unsigned int frac = pipe->is_video ? DE_VSU_FRAC : DE_GSU_FRAC;
	const struct de_layer_plan *ref = NULL;
	uint32_t first_format = 0, last_z = 0;
	int32_t minx = INT32_MAX, miny = INT32_MAX;
	bool any_premul = false;
	struct dpy_rect ovl = { 0, 0, 0, 0 };
	unsigned int i;
	int ret;

	memset(cp, 0, sizeof(*cp));
	cp->nlayers = pipe->nlayers;

	for (i = 0; i < pipe->nlayers; i++) {
		const struct dpy_plane_state *ps = layers[i];
		struct de_layer_plan *l = &cp->layer[i];
		uint8_t code, sub;
		bool ui_sel;

		if (!ps || !ps->visible)
			continue;
		ret = de_ovl_format(ps->fb.format, pipe->is_video, &code,
				    &ui_sel, &sub);
		if (ret) {
			dpy_dbg("%s: format %s unsupported\n", pipe->desc->name,
				display_format_name(ps->fb.format));
			return ret;
		}

		if (!ref) {
			cp->sub = sub;
			cp->yuv = ps->fb.info->is_yuv;
			cp->alpha = ps->alpha;
			cp->color_encoding = ps->color_encoding;
			cp->color_range = ps->color_range;
			cp->zmin = ps->normalized_zpos;
			first_format = ps->fb.format;
		} else {
			if (sub != cp->sub ||
			    (cp->yuv && ps->fb.format != first_format)) {
				dpy_dbg("%s: layers mix pixel formats\n",
					pipe->desc->name);
				return -EINVAL;
			}
			if (ps->alpha != cp->alpha) {
				dpy_dbg("%s: layers must share the plane alpha\n",
					pipe->desc->name);
				return -EINVAL;
			}
			if (ps->normalized_zpos <= last_z) {
				dpy_dbg("%s: z order must follow the layer order\n",
					pipe->desc->name);
				return -EINVAL;
			}
		}
		last_z = ps->normalized_zpos;

		l->enable = true;
		l->format = ps->fb.format;
		l->hw_fmt = code;
		l->ui_sel = ui_sel;
		de_layer_geometry(ps, frac, sub, l);
		/* a sliver narrower than one chroma sample shows nothing */
		if (!l->crop.w || !l->crop.h) {
			memset(l, 0, sizeof(*l));
			continue;
		}

		if (ps->fb.info->has_alpha &&
		    ps->blend_mode != DISPLAY_BLEND_NONE) {
			l->alpha_mode = ps->alpha == 0xff ? 0 : 2;
			if (ps->blend_mode == DISPLAY_BLEND_PREMULTIPLIED)
				any_premul = true;
		} else {
			l->alpha_mode = 1;
		}

		if (!ref) {
			ref = l;
		} else if (l->ys.hstep != ref->ys.hstep ||
			   l->ys.vstep != ref->ys.vstep) {
			dpy_dbg("%s: layers must share the scaling factor\n",
				pipe->desc->name);
			return -EINVAL;
		}
		minx = DPY_MIN(minx, l->frame.x);
		miny = DPY_MIN(miny, l->frame.y);
	}
	if (!ref)
		return 0;
	cp->zmax = last_z;

	/* pre-multiplication: if one layer is, the whole channel output is */
	for (i = 0; i < cp->nlayers; i++) {
		struct de_layer_plan *l = &cp->layer[i];
		const struct dpy_plane_state *ps = layers[i];

		if (!l->enable)
			continue;
		if (!any_premul)
			l->alpha_ctl = 0;
		else if (ps->blend_mode == DISPLAY_BLEND_PREMULTIPLIED)
			l->alpha_ctl = 2;
		else
			l->alpha_ctl = 1;
	}
	cp->premul_out = any_premul;

	/* channel scaling: shared step, phase of the first layer that has one */
	cp->ys = ref->ys;
	cp->cs = ref->cs;
	for (i = 0; i < cp->nlayers; i++) {
		if (cp->layer[i].enable && cp->layer[i].ys.hphase) {
			cp->ys.hphase = cp->layer[i].ys.hphase;
			cp->cs.hphase = cp->layer[i].cs.hphase;
			break;
		}
	}
	for (i = 0; i < cp->nlayers; i++) {
		if (cp->layer[i].enable && cp->layer[i].ys.vphase) {
			cp->ys.vphase = cp->layer[i].ys.vphase;
			cp->cs.vphase = cp->layer[i].cs.vphase;
			break;
		}
	}

	/* place the layers in the overlay window, in source pixels */
	for (i = 0; i < cp->nlayers; i++) {
		struct de_layer_plan *l = &cp->layer[i];

		if (!l->enable)
			continue;
		l->ovl.x = de_ovl_coord((uint32_t)(l->frame.x - minx),
					cp->ys.hstep, frac, cp->yuv);
		l->ovl.y = de_ovl_coord((uint32_t)(l->frame.y - miny),
					cp->ys.vstep, frac, cp->yuv);
		ovl = dpy_rect_union(ovl, l->ovl);
		cp->bld = dpy_rect_union(cp->bld, l->frame);
	}
	cp->ovl_w = (uint32_t)(ovl.x + (int32_t)ovl.w);
	cp->ovl_h = (uint32_t)(ovl.y + (int32_t)ovl.h);

	if (!pipe->has_scaler) {
		if (cp->ovl_w != cp->bld.w || cp->ovl_h != cp->bld.h ||
		    cp->ys.hstep != (1U << frac) || cp->ys.vstep != (1U << frac)) {
			dpy_dbg("%s: channel cannot scale\n", pipe->desc->name);
			return -ERANGE;
		}
		cp->in_w = cp->ovl_w;
		cp->in_h = cp->ovl_h;
	} else {
		cp->scale = cp->yuv || cp->ovl_w != cp->bld.w ||
			    cp->ovl_h != cp->bld.h;
		if (cp->scale) {
			de_fix_min_size(cp, frac, mode);
			if (pipe->is_video) {
				de_coarse(pipe, cp, mode);
			} else {
				cp->in_w = cp->ovl_w;
				cp->in_h = cp->ovl_h;
				cp->in_cw = cp->ovl_w;
				cp->in_ch = cp->ovl_h;
			}
		} else {
			cp->in_w = cp->ovl_w;
			cp->in_h = cp->ovl_h;
		}
	}

	cp->enable = true;
	return 0;
}
