// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Simple panel driver for parallel RGB and LVDS panels described entirely
 * by board data: timings, bus format, power/reset sequences and optional
 * SPI register initialisation.
 */
#define DPY_LOG_TAG "panel-simple"
#include <dpy/dpy_log.h>
#include <dpy/dpy_panel.h>

#include "panel_cmdseq.h"

struct panel_simple {
	struct dpy_panel panel;
	const struct dpy_panel_simple_pdata *pd;
	struct dpy_cmd_ctx ctx;
};

#define to_panel_simple(p) dpy_container_of(p, struct panel_simple, panel)

static int panel_simple_prepare(struct dpy_panel *panel)
{
	struct panel_simple *ps = to_panel_simple(panel);
	int ret;

	ret = dpy_cmdseq_run(&ps->pd->power_on, &ps->ctx);
	if (ret)
		return ret;
	if (ps->pd->init.count) {
		dpy_spi_gpio_setup(&ps->pd->spi);
		ret = dpy_cmdseq_run(&ps->pd->init, &ps->ctx);
	}
	return ret;
}

static int panel_simple_enable(struct dpy_panel *panel)
{
	struct panel_simple *ps = to_panel_simple(panel);

	/* give the panel a few frames of valid video before lighting it */
	if (ps->pd->enable_delay_ms)
		dpy_os_msleep(ps->pd->enable_delay_ms);
	return 0;
}

static int panel_simple_disable(struct dpy_panel *panel)
{
	struct panel_simple *ps = to_panel_simple(panel);

	if (ps->pd->disable_delay_ms)
		dpy_os_msleep(ps->pd->disable_delay_ms);
	return 0;
}

static int panel_simple_unprepare(struct dpy_panel *panel)
{
	struct panel_simple *ps = to_panel_simple(panel);

	if (ps->pd->exit.count)
		dpy_cmdseq_run(&ps->pd->exit, &ps->ctx);
	return dpy_cmdseq_run(&ps->pd->power_off, &ps->ctx);
}

static int panel_simple_get_modes(struct dpy_panel *panel,
				  struct dpy_display_mode *modes, int max)
{
	struct panel_simple *ps = to_panel_simple(panel);
	int i, n = DPY_MIN((int)ps->pd->num_modes, max);

	for (i = 0; i < n; i++)
		modes[i] = ps->pd->modes[i];
	/* the first mode is preferred unless one is flagged */
	for (i = 0; i < n; i++)
		if (modes[i].flags & DISPLAY_MODE_FLAG_PREFERRED)
			return n;
	if (n)
		modes[0].flags |= DISPLAY_MODE_FLAG_PREFERRED;
	return n;
}

static const struct dpy_panel_funcs panel_simple_funcs = {
	.prepare = panel_simple_prepare,
	.enable = panel_simple_enable,
	.disable = panel_simple_disable,
	.unprepare = panel_simple_unprepare,
	.get_modes = panel_simple_get_modes,
};

static int panel_simple_bind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct panel_simple *ps = dev->priv;
	struct dpy_gnode *bl;

	(void)ddev;
	bl = dpy_node_get_ref(dev->node, "backlight");
	if (bl) {
		ps->panel.backlight = dpy_backlight_find(bl);
		if (!ps->panel.backlight)
			dpy_warn("%s: backlight %s not bound\n",
				 dpy_dev_name(dev), dpy_node_name(bl));
	}
	return dpy_panel_add(&ps->panel);
}

static void panel_simple_unbind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct panel_simple *ps = dev->priv;

	(void)ddev;
	dpy_panel_remove(&ps->panel);
}

static const struct dpy_component_ops panel_simple_component_ops = {
	.bind = panel_simple_bind,
	.unbind = panel_simple_unbind,
};

static int panel_simple_probe(struct dpy_dev *dev)
{
	const struct dpy_panel_simple_pdata *pd = dpy_dev_pdata(dev);
	struct panel_simple *ps;

	if (!pd || !pd->num_modes) {
		dpy_err("%s: no panel description\n", dpy_dev_name(dev));
		return -EINVAL;
	}
	ps = dpy_os_zalloc(sizeof(*ps));
	if (!ps)
		return -ENOMEM;
	ps->pd = pd;
	ps->ctx.spi = &pd->spi;
	ps->panel.node = dev->node;
	ps->panel.funcs = &panel_simple_funcs;
	ps->panel.priv = ps;
	ps->panel.name = pd->name ? pd->name : dpy_dev_name(dev);
	ps->panel.info.width_mm = pd->width_mm;
	ps->panel.info.height_mm = pd->height_mm;
	ps->panel.info.bus_format = pd->bus_format;
	ps->panel.info.bus_flags = pd->bus_flags;
	dev->priv = ps;
	return dpy_component_add(dev);
}

static void panel_simple_remove(struct dpy_dev *dev)
{
	dpy_component_del(dev);
	dpy_os_free(dev->priv);
	dev->priv = NULL;
}

static const struct dpy_match panel_simple_match[] = {
	{ "panel-simple", NULL },
	{ NULL },
};

const struct dpy_driver dpy_panel_simple_driver = {
	.name = "panel-simple",
	.match = panel_simple_match,
	.klass = DPY_COMP_PANEL,
	.probe = panel_simple_probe,
	.remove = panel_simple_remove,
	.ops = &panel_simple_component_ops,
};
