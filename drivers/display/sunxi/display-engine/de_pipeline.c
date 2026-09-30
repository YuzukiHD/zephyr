// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display engine pipelines: instantiate the stages listed in the SoC
 * description, bind them into frontend (per channel) and backend (per
 * display) chains and walk them for check/apply.
 */
#define DPY_LOG_TAG "de-pipe"
#include "de_priv.h"

extern const struct de_stage_ops de_ovl_vi_ops;
extern const struct de_stage_ops de_ovl_ui_ops;
extern const struct de_stage_ops de_vsu_ops;
extern const struct de_stage_ops de_gsu_ops;
extern const struct de_stage_ops de_ccsc_ops;
extern const struct de_stage_ops de_bld_ops;
extern const struct de_stage_ops de_dcsc_ops;
extern const struct de_stage_ops de_gamma_ops;
extern const struct de_stage_ops de_dither_ops;

const struct de_stage_ops *const de_stage_ops_table[DE_STAGE_KIND_NR] = {
	[DE_STAGE_OVL_VI] = &de_ovl_vi_ops,
	[DE_STAGE_OVL_UI] = &de_ovl_ui_ops,
	[DE_STAGE_VSU] = &de_vsu_ops,
	[DE_STAGE_GSU] = &de_gsu_ops,
	[DE_STAGE_CCSC] = &de_ccsc_ops,
	[DE_STAGE_BLD] = &de_bld_ops,
	[DE_STAGE_DCSC] = &de_dcsc_ops,
	[DE_STAGE_GAMMA] = &de_gamma_ops,
	[DE_STAGE_DITHER] = &de_dither_ops,
};

static const char *const de_stage_kind_name[DE_STAGE_KIND_NR] = {
	[DE_STAGE_OVL_VI] = "ovl-vi",
	[DE_STAGE_OVL_UI] = "ovl-ui",
	[DE_STAGE_VSU] = "vsu",
	[DE_STAGE_GSU] = "gsu",
	[DE_STAGE_CCSC] = "ccsc",
	[DE_STAGE_BLD] = "bld",
	[DE_STAGE_DCSC] = "dcsc",
	[DE_STAGE_GAMMA] = "gamma",
	[DE_STAGE_DITHER] = "dither",
};

/* which stages a frontend/backend may contain, in hardware order */
static bool de_stage_allowed(uint8_t pipe_type, uint8_t kind, int prev)
{
	if (pipe_type == DE_PIPE_FRONTEND) {
		switch (kind) {
		case DE_STAGE_OVL_VI:
		case DE_STAGE_OVL_UI:
			return prev < 0;
		case DE_STAGE_VSU:
		case DE_STAGE_GSU:
			return prev == DE_STAGE_OVL_VI || prev == DE_STAGE_OVL_UI;
		case DE_STAGE_CCSC:
			return prev == DE_STAGE_OVL_VI || prev == DE_STAGE_VSU;
		default:
			return false;
		}
	}
	switch (kind) {
	case DE_STAGE_BLD:
		return prev < 0;
	case DE_STAGE_DCSC:
	case DE_STAGE_GAMMA:
	case DE_STAGE_DITHER:
		return prev >= DE_STAGE_BLD && prev < kind;
	default:
		return false;
	}
}

static int de_pipeline_init(struct de_engine *de, struct de_pipeline *pipe,
			    const struct de_pipe_desc *desc)
{
	int prev = -1;
	unsigned int i;
	int ret;

	if (desc->nstages > DE_PIPE_MAX_STAGES)
		return -EINVAL;
	pipe->desc = desc;
	pipe->de = de;

	for (i = 0; i < desc->nstages; i++) {
		const struct de_stage_desc *sd = &desc->stages[i];
		struct de_stage *st = &pipe->stages[i];

		if (sd->kind >= DE_STAGE_KIND_NR ||
		    !de_stage_allowed(desc->type, sd->kind, prev)) {
			dpy_err("%s: stage %s (%u) not allowed after %d\n",
				desc->name, sd->name, sd->kind, prev);
			return -EINVAL;
		}
		st->desc = sd;
		st->ops = de_stage_ops_table[sd->kind];
		st->de = de;
		st->pipe = pipe;
		ret = st->ops->init ? st->ops->init(st) : 0;
		if (ret) {
			dpy_err("%s: stage %s init failed: %d\n", desc->name,
				sd->name, ret);
			return ret;
		}
		pipe->nstages++;
		prev = sd->kind;

		switch (sd->kind) {
		case DE_STAGE_OVL_VI:
			pipe->is_video = true;
			pipe->nlayers = (uint8_t)sd->param;
			break;
		case DE_STAGE_OVL_UI:
			pipe->nlayers = (uint8_t)sd->param;
			break;
		case DE_STAGE_VSU:
		case DE_STAGE_GSU:
			pipe->has_scaler = true;
			pipe->line_buffer = sd->param;
			break;
		default:
			break;
		}
	}
	if (desc->type == DE_PIPE_FRONTEND &&
	    (!pipe->nlayers || pipe->nlayers > DE_MAX_LAYERS)) {
		dpy_err("%s: frontend without overlay\n", desc->name);
		return -EINVAL;
	}
	return 0;
}

int de_pipeline_build(struct de_engine *de)
{
	const struct de_soc_desc *soc = de->soc;
	unsigned int i;
	int ret;

	if (soc->npipes > DE_MAX_PIPES)
		return -EINVAL;

	for (i = 0; i < soc->npipes; i++) {
		const struct de_pipe_desc *desc = &soc->pipes[i];
		struct de_pipeline *pipe = &de->pipes[de->npipes];

		ret = de_pipeline_init(de, pipe, desc);
		if (ret)
			return ret;
		de->npipes++;

		if (desc->type == DE_PIPE_FRONTEND) {
			if (de->nfrontends >= DE_MAX_CHANNELS)
				return -EINVAL;
			pipe->index = de->nfrontends;
			de->frontend[de->nfrontends++] = pipe;
		} else {
			if (desc->id >= DE_MAX_DISPS)
				return -EINVAL;
			pipe->index = desc->id;
			de->backend[desc->id] = pipe;
			de->nbackends = DPY_MAX(de->nbackends, desc->id + 1);
		}
		dpy_dbg("pipeline %s: %u stage(s)\n", desc->name, pipe->nstages);
	}
	if (!de->nbackends || !de->backend[0] ||
	    !de_pipeline_find_stage(de->backend[0], DE_STAGE_BLD)) {
		dpy_err("no backend with a blender\n");
		return -EINVAL;
	}
	return 0;
}

struct de_stage *de_pipeline_find_stage(struct de_pipeline *pipe,
					enum de_stage_kind kind)
{
	unsigned int i;

	for (i = 0; i < pipe->nstages; i++)
		if (pipe->stages[i].desc->kind == kind)
			return &pipe->stages[i];
	return NULL;
}

int de_pipeline_check(struct de_pipeline *pipe, struct de_crtc_state *s)
{
	unsigned int i;
	int ret;

	for (i = 0; i < pipe->nstages; i++) {
		struct de_stage *st = &pipe->stages[i];

		if (!st->ops->check)
			continue;
		ret = st->ops->check(st, s);
		if (ret) {
			dpy_dbg("%s rejected the configuration: %d\n",
				st->desc->name, ret);
			return ret;
		}
	}
	return 0;
}

void de_pipeline_apply(struct de_pipeline *pipe, const struct de_crtc_state *s)
{
	unsigned int i;

	for (i = 0; i < pipe->nstages; i++)
		if (pipe->stages[i].ops->apply)
			pipe->stages[i].ops->apply(&pipe->stages[i], s);
}

const char *de_stage_kind_str(uint8_t kind)
{
	return kind < DE_STAGE_KIND_NR ? de_stage_kind_name[kind] : "?";
}
