// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display engine CRTC: one display output of the engine (mixer).
 *
 * check: plan every frontend of the display from its plane states, plan
 *        the backend, let every stage validate its part;
 * enable: power the engine, program the whole state directly (the output
 *         is not running yet);
 * flush: translate the plans into shadow registers and hand the changed
 *        blocks to the RCQ, which loads them at the next frame start;
 * vblank: completes the flip once the RCQ load has been observed.
 */
#define DPY_LOG_TAG "de-crtc"
#include "de_priv.h"

#define DE_FLIP_TIMEOUT_MS	100

static struct dpy_crtc_state *de_crtc_duplicate_state(struct dpy_crtc *crtc)
{
	struct de_crtc_state *st = dpy_os_zalloc(sizeof(*st));

	if (!st)
		return NULL;
	if (crtc->state) {
		*st = *to_de_crtc_state(crtc->state);
		dpy_crtc_state_copy(&st->base, crtc->state);
	}
	st->wb_job = NULL;
	return &st->base;
}

static void de_crtc_destroy_state(struct dpy_crtc *crtc,
				  struct dpy_crtc_state *state)
{
	(void)crtc;
	dpy_os_free(to_de_crtc_state(state));
}

static struct dpy_plane_state *de_layer_state(struct dpy_atomic_state *state,
					      struct de_plane *p,
					      struct dpy_crtc *crtc)
{
	struct dpy_plane_state *ps;

	if (!p)
		return NULL;
	ps = dpy_atomic_new_plane_state(state, &p->base);
	if (!ps)
		ps = p->base.state;
	return ps->crtc == crtc && ps->visible ? ps : NULL;
}

static int de_crtc_atomic_check(struct dpy_crtc *crtc,
				struct dpy_atomic_state *state)
{
	struct de_crtc *dc = to_de_crtc(crtc);
	struct de_engine *de = dc->de;
	struct dpy_crtc_state *cs = dpy_atomic_new_crtc_state(state, crtc);
	struct de_crtc_state *s = to_de_crtc_state(cs);
	struct dpy_atomic_connector *ac;
	unsigned int c, l;
	int ret;

	s->wb_job = NULL;
	if (!cs->active) {
		memset(s->chn, 0, sizeof(s->chn));
		return 0;
	}

	for (c = 0; c < de->nfrontends; c++) {
		struct de_pipeline *pipe = de->frontend[c];
		struct dpy_plane_state *layers[DE_MAX_LAYERS] = { NULL };

		if (pipe->desc->disp != dc->disp)
			continue;
		for (l = 0; l < pipe->nlayers; l++)
			layers[l] = de_layer_state(state, de->plane_map[c][l], crtc);
		ret = de_channel_plan(pipe, s, layers, &cs->mode);
		if (ret)
			return ret;
		ret = de_pipeline_check(pipe, s);
		if (ret)
			return ret;
	}

	ret = de_disp_plan(de, s);
	if (ret)
		return ret;
	ret = de_pipeline_check(dc->backend, s);
	if (ret)
		return ret;

	dpy_for_each_connector_in_state(state, c, ac) {
		if (ac->ptr->type == DPY_CONNECTOR_WRITEBACK &&
		    ac->new_state->crtc == crtc && ac->new_state->wb_job)
			s->wb_job = ac->new_state->wb_job;
	}
	return 0;
}

static void de_crtc_apply_all(struct de_crtc *dc, const struct de_crtc_state *s)
{
	struct de_engine *de = dc->de;
	unsigned int c;

	for (c = 0; c < de->nfrontends; c++)
		if (de->frontend[c]->desc->disp == dc->disp)
			de_pipeline_apply(de->frontend[c], s);
	de_pipeline_apply(dc->backend, s);
}

static void de_crtc_atomic_enable(struct dpy_crtc *crtc,
				  struct dpy_atomic_state *state)
{
	struct de_crtc *dc = to_de_crtc(crtc);
	struct de_engine *de = dc->de;
	const struct de_crtc_state *s = to_de_crtc_state(crtc->state);

	(void)state;
	if (de_top_power_on(de)) {
		dpy_err("%s: cannot power up\n", crtc->name);
		return;
	}
	de_top_disp_enable(de, dc->disp, s->base.mode.hdisplay,
			   s->base.mode.vdisplay, true);
	de_top_rcq_setup(de, dc->disp, dc->rcq.hdrs_dma,
			 dc->rcq.nhdrs * sizeof(struct de_rcq_hdr));

	/* the output is not running yet: program everything directly */
	de_crtc_apply_all(dc, s);
	de_regs_flush_direct(de, dc->disp, true);
	dc->rcq.pending = false;
	dc->hw_enabled = true;
}

static void de_crtc_atomic_disable(struct dpy_crtc *crtc,
				   struct dpy_atomic_state *state)
{
	struct de_crtc *dc = to_de_crtc(crtc);
	struct de_engine *de = dc->de;

	(void)state;
	if (!dc->hw_enabled)
		return;
	dc->rcq.pending = false;
	dpy_crtc_flip_done(crtc);
	de_top_disp_enable(de, dc->disp, 0, 0, false);
	de_regs_mark_all_dirty(de, dc->disp);
	de_top_power_off(de);
	dc->hw_enabled = false;
}

static void de_crtc_atomic_begin(struct dpy_crtc *crtc,
				 struct dpy_atomic_state *state)
{
	struct de_crtc *dc = to_de_crtc(crtc);

	(void)state;
	/* a non-blocking flip may still be in flight */
	if (dc->rcq.pending && dpy_crtc_wait_flip(crtc, DE_FLIP_TIMEOUT_MS)) {
		dpy_warn("%s: previous flip lost\n", crtc->name);
		de_rcq_abort(dc->de, dc->disp);
	}
}

static void de_crtc_clear_wb_job(struct dpy_atomic_state *state,
				 struct dpy_crtc *crtc)
{
	struct dpy_atomic_connector *ac;
	unsigned int i;

	dpy_for_each_connector_in_state(state, i, ac)
		if (ac->ptr->type == DPY_CONNECTOR_WRITEBACK &&
		    ac->new_state->crtc == crtc)
			ac->new_state->wb_job = NULL;
}

static void de_crtc_atomic_flush(struct dpy_crtc *crtc,
				 struct dpy_atomic_state *state)
{
	struct de_crtc *dc = to_de_crtc(crtc);
	struct de_engine *de = dc->de;
	struct de_crtc_state *s = to_de_crtc_state(crtc->state);

	s->base.flip_needed = false;
	if (!dc->hw_enabled) {
		/* the engine failed to power up: fail a capture, do not hang it */
		if (s->wb_job) {
			s->wb_job->status = -EIO;
			if (s->wb_job->done)
				dpy_os_sem_post(s->wb_job->done);
			s->wb_job = NULL;
			de_crtc_clear_wb_job(state, crtc);
		}
		return;
	}

	de_crtc_apply_all(dc, s);
	if (de_rcq_prepare(de, dc->disp)) {
		de_rcq_trigger(de, dc->disp);
		s->base.flip_needed = true;
	}

	if (s->wb_job) {
		/* capture the frame this commit produces */
		if (s->base.flip_needed) {
			dpy_crtc_wait_flip(crtc, DE_FLIP_TIMEOUT_MS);
			s->base.flip_needed = false;
		}
		de_wb_run(de, dc->disp, &s->base.mode, s->wb_job, crtc);
		s->wb_job = NULL;
		de_crtc_clear_wb_job(state, crtc);
	}
}

static void de_crtc_vblank(struct dpy_crtc *crtc)
{
	struct de_crtc *dc = to_de_crtc(crtc);

	if (de_rcq_check_done(dc->de, dc->disp, true))
		dpy_crtc_flip_done(crtc);
}

static void de_crtc_frame_start(struct dpy_crtc *crtc)
{
	struct de_crtc *dc = to_de_crtc(crtc);

	if (de_rcq_check_done(dc->de, dc->disp, false))
		dpy_crtc_flip_done(crtc);
}

static void de_crtc_dump(struct dpy_crtc *crtc,
			 void (*print)(const char *fmt, ...))
{
	struct de_crtc *dc = to_de_crtc(crtc);
	struct de_engine *de = dc->de;
	const struct de_crtc_state *s = to_de_crtc_state(crtc->state);
	unsigned int p, i;

	de_top_dump(de, print);
	print("rcq: %u blocks, %u commits (%u early), %u lost, %s\n",
	      dc->rcq.nhdrs, dc->rcq.commits, dc->rcq.early_done,
	      dc->rcq.timeouts,
	      dc->rcq.pending ? "pending" : "idle");
	for (p = 0; p < de->npipes; p++) {
		struct de_pipeline *pipe = &de->pipes[p];

		if (pipe->desc->type == DE_PIPE_FRONTEND) {
			const struct de_chn_plan *cp = &s->chn[pipe->index];

			print("%s: %s", pipe->desc->name,
			      cp->enable ? "on" : "off");
			if (cp->enable)
				print(" pipe %u ovl %ux%u -> [%d,%d %ux%u] %s%s",
				      cp->pipe, cp->ovl_w, cp->ovl_h, cp->bld.x,
				      cp->bld.y, cp->bld.w, cp->bld.h,
				      cp->scale ? "scaled " : "",
				      cp->yuv ? "yuv" : "rgb");
			print("\n");
		} else {
			print("%s: %ux%u, %u pipe(s), bg %08x, dither %s\n",
			      pipe->desc->name, s->disp.w, s->disp.h,
			      s->disp.npipes, s->disp.background,
			      s->disp.dither ? "on" : "off");
		}
		for (i = 0; i < pipe->nstages; i++) {
			struct de_stage *st = &pipe->stages[i];

			print("  %s (%s @%06x)\n", st->desc->name,
			      de_stage_kind_str(st->desc->kind), st->desc->offset);
			if (st->ops->dump)
				st->ops->dump(st, print);
		}
	}
}

static const struct dpy_crtc_funcs de_crtc_funcs = {
	.duplicate_state = de_crtc_duplicate_state,
	.destroy_state = de_crtc_destroy_state,
	.atomic_check = de_crtc_atomic_check,
	.atomic_enable = de_crtc_atomic_enable,
	.atomic_disable = de_crtc_atomic_disable,
	.atomic_begin = de_crtc_atomic_begin,
	.atomic_flush = de_crtc_atomic_flush,
	.vblank = de_crtc_vblank,
	.frame_start = de_crtc_frame_start,
	.dump = de_crtc_dump,
};

int de_crtc_create(struct de_engine *de, struct dpy_device *ddev, uint8_t disp)
{
	struct de_crtc *dc = &de->crtc[disp];
	int ret;

	dc->de = de;
	dc->disp = disp;
	dc->backend = de->backend[disp];

	ret = de_rcq_init(de, disp);
	if (ret)
		return ret;

	dc->base.node = de->dev->node;
	dc->base.port = disp;
	dc->base.priv = dc;
	ret = dpy_crtc_init(ddev, &dc->base, &de_crtc_funcs,
			    dc->backend->desc->name);
	if (ret) {
		de_rcq_exit(de, disp);
		return ret;
	}

	if (de->board) {
		dc->base.state->adjust = de->board->adjust;
		dc->base.state->background = 0xff000000 |
					     (de->board->background & 0xffffff);
	}
	return 0;
}

void de_crtc_destroy(struct de_engine *de, uint8_t disp)
{
	struct de_crtc *dc = &de->crtc[disp];

	dpy_crtc_cleanup(&dc->base);
	de_rcq_exit(de, disp);
}
