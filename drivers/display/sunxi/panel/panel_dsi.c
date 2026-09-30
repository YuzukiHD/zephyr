// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Generic MIPI DSI panel driver: timings, lane/format/mode flags and the
 * DCS/generic command sequences come from board data.
 *
 * On bind the panel follows its input port to the DSI host and attaches
 * its DSI device; the DSI encoder binds afterwards and finds it attached.
 */
#define DPY_LOG_TAG "panel-dsi"
#include <dpy/dpy_log.h>
#include <dpy/dpy_panel.h>

#include "panel_cmdseq.h"

struct panel_dsi {
	struct dpy_panel panel;
	struct dpy_dsi_device dsi;
	const struct dpy_panel_dsi_pdata *pd;
	struct dpy_cmd_ctx ctx;
};

#define to_panel_dsi(p) dpy_container_of(p, struct panel_dsi, panel)

static int panel_dsi_prepare(struct dpy_panel *panel)
{
	struct panel_dsi *pd = to_panel_dsi(panel);
	int ret;

	ret = dpy_cmdseq_run(&pd->pd->power_on, &pd->ctx);
	if (ret)
		return ret;
	return dpy_cmdseq_run(&pd->pd->init, &pd->ctx);
}

static int panel_dsi_enable(struct dpy_panel *panel)
{
	struct panel_dsi *pd = to_panel_dsi(panel);

	if (pd->pd->enable_delay_ms)
		dpy_os_msleep(pd->pd->enable_delay_ms);
	return 0;
}

static int panel_dsi_disable(struct dpy_panel *panel)
{
	struct panel_dsi *pd = to_panel_dsi(panel);

	if (pd->pd->disable_delay_ms)
		dpy_os_msleep(pd->pd->disable_delay_ms);
	return 0;
}

static int panel_dsi_unprepare(struct dpy_panel *panel)
{
	struct panel_dsi *pd = to_panel_dsi(panel);

	dpy_cmdseq_run(&pd->pd->exit, &pd->ctx);
	return dpy_cmdseq_run(&pd->pd->power_off, &pd->ctx);
}

static int panel_dsi_get_modes(struct dpy_panel *panel,
			       struct dpy_display_mode *modes, int max)
{
	struct panel_dsi *pd = to_panel_dsi(panel);
	int i, n = DPY_MIN((int)pd->pd->num_modes, max);

	for (i = 0; i < n; i++)
		modes[i] = pd->pd->modes[i];
	if (n && !(modes[0].flags & DISPLAY_MODE_FLAG_PREFERRED))
		modes[0].flags |= DISPLAY_MODE_FLAG_PREFERRED;
	return n;
}

static const struct dpy_panel_funcs panel_dsi_funcs = {
	.prepare = panel_dsi_prepare,
	.enable = panel_dsi_enable,
	.disable = panel_dsi_disable,
	.unprepare = panel_dsi_unprepare,
	.get_modes = panel_dsi_get_modes,
};

static int panel_dsi_bind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct panel_dsi *pd = dev->priv;
	struct dpy_dsi_host *host;
	struct dpy_gnode *remote, *bl;
	int ret;

	(void)ddev;
	remote = dpy_node_remote(dev->node, 0, DPY_EP_ANY, NULL, NULL);
	host = remote ? dpy_dsi_host_find(remote) : NULL;
	if (!host) {
		dpy_err("%s: not linked to a DSI host\n", dpy_dev_name(dev));
		return -ENODEV;
	}
	ret = dpy_dsi_attach(&pd->dsi, host);
	if (ret)
		return ret;

	bl = dpy_node_get_ref(dev->node, "backlight");
	if (bl)
		pd->panel.backlight = dpy_backlight_find(bl);

	ret = dpy_panel_add(&pd->panel);
	if (ret)
		dpy_dsi_detach(&pd->dsi);
	return ret;
}

static void panel_dsi_unbind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct panel_dsi *pd = dev->priv;

	(void)ddev;
	dpy_panel_remove(&pd->panel);
	dpy_dsi_detach(&pd->dsi);
}

static const struct dpy_component_ops panel_dsi_component_ops = {
	.bind = panel_dsi_bind,
	.unbind = panel_dsi_unbind,
};

static int panel_dsi_probe(struct dpy_dev *dev)
{
	const struct dpy_panel_dsi_pdata *pdata = dpy_dev_pdata(dev);
	struct panel_dsi *pd;

	if (!pdata || !pdata->num_modes || !pdata->lanes) {
		dpy_err("%s: no panel description\n", dpy_dev_name(dev));
		return -EINVAL;
	}
	pd = dpy_os_zalloc(sizeof(*pd));
	if (!pd)
		return -ENOMEM;
	pd->pd = pdata;
	pd->dsi.node = dev->node;
	pd->dsi.lanes = pdata->lanes;
	pd->dsi.format = pdata->format;
	pd->dsi.mode_flags = pdata->mode_flags;
	pd->dsi.hs_trail = pdata->hs_trail;
	pd->dsi.clk_trail = pdata->clk_trail;
	pd->ctx.dsi = &pd->dsi;
	pd->panel.node = dev->node;
	pd->panel.funcs = &panel_dsi_funcs;
	pd->panel.priv = pd;
	pd->panel.name = pdata->name ? pdata->name : dpy_dev_name(dev);
	pd->panel.info.width_mm = pdata->width_mm;
	pd->panel.info.height_mm = pdata->height_mm;
	dev->priv = pd;
	return dpy_component_add(dev);
}

static void panel_dsi_remove(struct dpy_dev *dev)
{
	dpy_component_del(dev);
	dpy_os_free(dev->priv);
	dev->priv = NULL;
}

static const struct dpy_match panel_dsi_match[] = {
	{ "panel-dsi", NULL },
	{ NULL },
};

const struct dpy_driver dpy_panel_dsi_driver = {
	.name = "panel-dsi",
	.match = panel_dsi_match,
	.klass = DPY_COMP_PANEL,
	.probe = panel_dsi_probe,
	.remove = panel_dsi_remove,
	.ops = &panel_dsi_component_ops,
};
