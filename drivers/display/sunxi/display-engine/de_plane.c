// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display engine planes: one plane per overlay layer. A plane only
 * validates its own state; the channel it belongs to is planned by the
 * CRTC check and programmed by the CRTC flush.
 */
#define DPY_LOG_TAG "de-plane"
#include <stdio.h>

#include "de_priv.h"

static int de_plane_atomic_check(struct dpy_plane *plane,
				 struct dpy_atomic_state *state)
{
	struct de_plane *dp = to_de_plane(plane);
	struct dpy_plane_state *ps = dpy_atomic_new_plane_state(state, plane);
	const struct dpy_crtc_state *cs;
	int32_t min_scale, max_scale;
	int ret;

	if (!ps->crtc) {
		ps->visible = false;
		return 0;
	}
	cs = dpy_atomic_new_crtc_state(state, ps->crtc);

	if (dp->chn->has_scaler) {
		min_scale = DPY_FP16(1) / 16;	/* 16x up */
		max_scale = DPY_FP16(16);	/* 16x down, with decimation */
	} else {
		min_scale = DPY_FP16(1);
		max_scale = DPY_FP16(1);
	}
	ret = dpy_plane_helper_check_state(ps, cs, min_scale, max_scale);
	if (ret) {
		dpy_dbg("%s: invalid geometry: %d\n", plane->name, ret);
		return ret;
	}
	if (ps->fb.width > plane->max_width || ps->fb.height > plane->max_height)
		return -ERANGE;
	/* the fetch unit works on 32 bit words */
	if (ps->fb.pitch[0] & 3)
		return -EINVAL;
	return 0;
}

static const struct dpy_plane_funcs de_plane_funcs = {
	.atomic_check = de_plane_atomic_check,
};

int de_planes_create(struct de_engine *de, struct dpy_device *ddev)
{
	bool primary_done = false;
	unsigned int c, l;
	int ret;

	for (c = 0; c < de->nfrontends; c++) {
		struct de_pipeline *pipe = de->frontend[c];
		const struct de_pipe_desc *pd = pipe->desc;

		for (l = 0; l < pipe->nlayers; l++) {
			struct de_plane *p = &de->planes[de->nplanes];
			enum dpy_plane_type type = DPY_PLANE_OVERLAY;

			/* the first UI layer is the natural primary plane */
			if (!primary_done && !pipe->is_video && l == 0) {
				type = DPY_PLANE_PRIMARY;
				primary_done = true;
			}
			p->de = de;
			p->chn = pipe;
			p->layer = (uint8_t)l;
			snprintf(p->name, sizeof(p->name), "%s-l%u", pd->name, l);
			p->base.priv = p;
			ret = dpy_plane_init(ddev, &p->base, &de_plane_funcs, type,
					     pd->formats, pd->nformats,
					     DPY_BIT(de->crtc[pd->disp].base.index),
					     p->name);
			if (ret)
				return ret;
			p->base.group = pipe->index;
			p->base.group_index = l;
			p->base.max_width = pd->max_width;
			p->base.max_height = pd->max_height;
			p->base.caps = DISPLAY_PLANE_CAP_ALPHA |
				       (pipe->has_scaler ? DISPLAY_PLANE_CAP_SCALE : 0) |
				       (pipe->is_video ? DISPLAY_PLANE_CAP_YUV : 0);
			de->plane_map[c][l] = p;
			de->nplanes++;
		}
	}
	return 0;
}

void de_planes_destroy(struct de_engine *de)
{
	unsigned int i;

	for (i = 0; i < de->nplanes; i++)
		dpy_plane_cleanup(&de->planes[i].base);
	de->nplanes = 0;
	memset(de->plane_map, 0, sizeof(de->plane_map));
}
