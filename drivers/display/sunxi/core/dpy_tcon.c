// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - timing controller registry and the CRTC <->
 * TCON glue.
 */
#define DPY_LOG_TAG "tcon"
#include <dpy/dpy_log.h>
#include <dpy/dpy_tcon.h>

static struct dpy_list dpy_tcons = DPY_LIST_INIT(dpy_tcons);

int dpy_timing_ctrl_register(struct dpy_timing_ctrl *tc)
{
	if (!tc->ops || !tc->ops->prepare || !tc->ops->enable)
		return -EINVAL;
	dpy_list_add_tail(&tc->head, &dpy_tcons);
	return 0;
}

void dpy_timing_ctrl_unregister(struct dpy_timing_ctrl *tc)
{
	dpy_list_del(&tc->head);
}

struct dpy_timing_ctrl *dpy_timing_ctrl_find(const struct dpy_gnode *node)
{
	struct dpy_timing_ctrl *tc;

	dpy_list_for_each_entry(tc, &dpy_tcons, head)
		if (tc->node == node)
			return tc;
	return NULL;
}

int dpy_timing_ctrl_attach(struct dpy_timing_ctrl *tc, struct dpy_device *ddev)
{
	const struct dpy_gnode *engine;
	struct dpy_crtc *crtc;
	uint8_t port;

	engine = dpy_node_remote(tc->node, 0, DPY_EP_ANY, &port, NULL);
	if (!engine) {
		dpy_err("%s: input port not connected\n", tc->name);
		return -ENODEV;
	}
	crtc = dpy_crtc_find_by_port(ddev, engine, port);
	if (!crtc) {
		dpy_err("%s: no crtc for %s port %u\n", tc->name,
			engine->desc->name, port);
		return -ENODEV;
	}
	if (crtc->tc) {
		dpy_err("%s: %s already driven by %s\n", tc->name, crtc->name,
			crtc->tc->name);
		return -EBUSY;
	}
	crtc->tc = tc;
	tc->crtc = crtc;
	dpy_info("%s attached to %s\n", tc->name, crtc->name);
	return 0;
}

void dpy_timing_ctrl_detach(struct dpy_timing_ctrl *tc)
{
	if (tc->crtc)
		tc->crtc->tc = NULL;
	tc->crtc = NULL;
}

void dpy_timing_ctrl_set_vblank_source(struct dpy_timing_ctrl *tc,
				       struct dpy_vblank_source *src)
{
	tc->vsrc = src;
}

void dpy_timing_ctrl_vblank(struct dpy_timing_ctrl *tc)
{
	if (tc->ops->check_underflow && tc->ops->check_underflow(tc))
		tc->underflow_count++;
	if (tc->crtc)
		dpy_crtc_handle_vblank(tc->crtc);
}

void dpy_timing_ctrl_line_irq(struct dpy_timing_ctrl *tc)
{
	if (tc->crtc)
		dpy_crtc_handle_frame_start(tc->crtc);
}

int dpy_timing_ctrl_enable_vblank(struct dpy_timing_ctrl *tc, bool on)
{
	if (tc->vsrc && tc->vsrc->set_enable)
		return tc->vsrc->set_enable(tc->vsrc, on);
	return tc->ops->set_vblank ? tc->ops->set_vblank(tc, on) : -ENOTSUP;
}

uint32_t dpy_timing_ctrl_get_line(struct dpy_timing_ctrl *tc)
{
	if (tc->vsrc && tc->vsrc->get_line)
		return tc->vsrc->get_line(tc->vsrc);
	return tc->ops->get_line ? tc->ops->get_line(tc) : 0;
}

bool dpy_timing_ctrl_in_safe_window(struct dpy_timing_ctrl *tc)
{
	uint32_t line, vtotal;

	if (!tc || !tc->enabled)
		return true;
	vtotal = tc->out.mode.vtotal;
	if (!vtotal)
		return true;
	line = dpy_timing_ctrl_get_line(tc);
	/* stay well away from the start and the end of the frame */
	return line >= vtotal / 8 && line <= vtotal - vtotal / 8;
}

int dpy_timing_ctrl_prepare(struct dpy_timing_ctrl *tc,
			    const struct dpy_tcon_output *out)
{
	int ret;

	ret = tc->ops->prepare(tc, out);
	if (ret) {
		dpy_err("%s: prepare failed: %d\n", tc->name, ret);
		return ret;
	}
	tc->out = *out;
	tc->prepared = true;
	return 0;
}

int dpy_timing_ctrl_enable(struct dpy_timing_ctrl *tc)
{
	int ret;

	if (!tc->prepared)
		return -EINVAL;
	ret = tc->ops->enable(tc);
	if (!ret)
		tc->enabled = true;
	return ret;
}

void dpy_timing_ctrl_disable(struct dpy_timing_ctrl *tc)
{
	if (!tc->enabled)
		return;
	if (tc->ops->disable)
		tc->ops->disable(tc);
	tc->enabled = false;
}

void dpy_timing_ctrl_unprepare(struct dpy_timing_ctrl *tc)
{
	if (!tc->prepared)
		return;
	if (tc->ops->unprepare)
		tc->ops->unprepare(tc);
	tc->prepared = false;
}
