// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display engine backend (display) planning: stacking order of the
 * channels in the blender, background, picture adjustment, gamma and the
 * output dither derived from the panel bus format.
 */
#define DPY_LOG_TAG "de-disp"
#include "de_priv.h"

int de_disp_plan(struct de_engine *de, struct de_crtc_state *s)
{
	struct de_disp_plan *d = &s->disp;
	struct de_pipeline *order[DE_MAX_CHANNELS];
	unsigned int n = 0, i, j;
	uint32_t bpc;

	memset(d, 0, sizeof(*d));
	d->w = s->base.mode.hdisplay;
	d->h = s->base.mode.vdisplay;
	d->background = s->base.background;
	d->adjust = s->base.adjust;
	d->gamma_enable = s->base.gamma_enable;
	d->gamma_seq = s->base.gamma_seq;

	/* channels in use, sorted bottom to top by their lowest plane */
	for (i = 0; i < de->nfrontends; i++) {
		struct de_pipeline *p = de->frontend[i];

		if (!s->chn[p->index].enable)
			continue;
		for (j = n; j > 0; j--) {
			if (s->chn[order[j - 1]->index].zmin <
			    s->chn[p->index].zmin)
				break;
			order[j] = order[j - 1];
		}
		order[j] = p;
		n++;
	}

	for (i = 0; i < n; i++) {
		struct de_chn_plan *cp = &s->chn[order[i]->index];

		/* a channel is one blender input: its planes cannot interleave */
		if (i && s->chn[order[i - 1]->index].zmax > cp->zmin) {
			dpy_dbg("planes of %s and %s are interleaved in z\n",
				order[i - 1]->desc->name, order[i]->desc->name);
			return -EINVAL;
		}
		cp->pipe = (uint8_t)i;
		d->pipe[i].enable = true;
		d->pipe[i].port = order[i]->desc->id;
		d->pipe[i].rect = cp->bld;
		d->pipe[i].premul = cp->premul_out;
	}
	d->npipes = (uint8_t)n;

	/* dither down to the panel depth; the TCON FRM may do it instead */
	bpc = dpy_bus_format_bpc(s->base.bus_format);
	if (bpc == 6 || bpc == 5) {
		d->dither = true;
		d->dither_fmt = bpc == 6 ? DE_DITHER_FMT_666 : DE_DITHER_FMT_565;
		d->dither_mode = DE_DITHER_SIERRA_LITE;
	}
	return 0;
}
