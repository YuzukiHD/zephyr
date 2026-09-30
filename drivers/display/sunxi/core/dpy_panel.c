// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - panel, bridge and backlight registries.
 */
#define DPY_LOG_TAG "panel"
#include <dpy/dpy_log.h>
#include <dpy/dpy_panel.h>

static struct dpy_list dpy_panels = DPY_LIST_INIT(dpy_panels);
static struct dpy_list dpy_bridges = DPY_LIST_INIT(dpy_bridges);
static struct dpy_list dpy_backlights = DPY_LIST_INIT(dpy_backlights);

/* ------------------------------------------------------------------ */
/* Backlight                                                           */
/* ------------------------------------------------------------------ */
int dpy_backlight_register(struct dpy_backlight *bl)
{
	if (!bl->ops || !bl->ops->update)
		return -EINVAL;
	if (!bl->max_level)
		bl->max_level = DISPLAY_BACKLIGHT_MAX;
	if (bl->level > bl->max_level)
		bl->level = bl->max_level;
	dpy_list_add_tail(&bl->head, &dpy_backlights);
	return 0;
}

void dpy_backlight_unregister(struct dpy_backlight *bl)
{
	dpy_list_del(&bl->head);
}

struct dpy_backlight *dpy_backlight_find(const struct dpy_gnode *node)
{
	struct dpy_backlight *bl;

	dpy_list_for_each_entry(bl, &dpy_backlights, head)
		if (bl->node == node)
			return bl;
	return NULL;
}

int dpy_backlight_set_level(struct dpy_backlight *bl, uint32_t level)
{
	int ret;

	if (!bl)
		return -ENODEV;
	if (level > bl->max_level)
		level = bl->max_level;
	ret = bl->ops->update(bl, level, bl->on);
	if (!ret)
		bl->level = level;
	return ret;
}

int dpy_backlight_enable(struct dpy_backlight *bl)
{
	int ret;

	if (!bl)
		return 0;
	ret = bl->ops->update(bl, bl->level, true);
	if (!ret)
		bl->on = true;
	return ret;
}

int dpy_backlight_disable(struct dpy_backlight *bl)
{
	int ret;

	if (!bl)
		return 0;
	ret = bl->ops->update(bl, bl->level, false);
	if (!ret)
		bl->on = false;
	return ret;
}

/* ------------------------------------------------------------------ */
/* Panel                                                               */
/* ------------------------------------------------------------------ */
int dpy_panel_add(struct dpy_panel *panel)
{
	if (!panel->funcs)
		return -EINVAL;
	dpy_list_add_tail(&panel->head, &dpy_panels);
	return 0;
}

void dpy_panel_remove(struct dpy_panel *panel)
{
	dpy_list_del(&panel->head);
}

struct dpy_panel *dpy_panel_find(const struct dpy_gnode *node)
{
	struct dpy_panel *panel;

	dpy_list_for_each_entry(panel, &dpy_panels, head)
		if (panel->node == node)
			return panel;
	return NULL;
}

int dpy_panel_prepare(struct dpy_panel *panel)
{
	int ret = 0;

	if (!panel || panel->prepared)
		return 0;
	if (panel->funcs->prepare)
		ret = panel->funcs->prepare(panel);
	if (!ret)
		panel->prepared = true;
	else
		dpy_err("%s: prepare failed: %d\n", panel->name, ret);
	return ret;
}

int dpy_panel_enable(struct dpy_panel *panel)
{
	int ret = 0;

	if (!panel || panel->enabled)
		return 0;
	if (panel->funcs->enable)
		ret = panel->funcs->enable(panel);
	if (!ret)
		ret = dpy_backlight_enable(panel->backlight);
	if (!ret)
		panel->enabled = true;
	else
		dpy_err("%s: enable failed: %d\n", panel->name, ret);
	return ret;
}

int dpy_panel_disable(struct dpy_panel *panel)
{
	int ret = 0;

	if (!panel || !panel->enabled)
		return 0;
	dpy_backlight_disable(panel->backlight);
	if (panel->funcs->disable)
		ret = panel->funcs->disable(panel);
	panel->enabled = false;
	return ret;
}

int dpy_panel_unprepare(struct dpy_panel *panel)
{
	int ret = 0;

	if (!panel || !panel->prepared)
		return 0;
	if (panel->funcs->unprepare)
		ret = panel->funcs->unprepare(panel);
	panel->prepared = false;
	return ret;
}

int dpy_panel_get_modes(struct dpy_panel *panel,
			struct dpy_display_mode *modes, int max)
{
	if (!panel || !panel->funcs->get_modes)
		return 0;
	return panel->funcs->get_modes(panel, modes, max);
}

/* ------------------------------------------------------------------ */
/* Bridge                                                              */
/* ------------------------------------------------------------------ */
int dpy_bridge_add(struct dpy_bridge *bridge)
{
	if (!bridge->funcs)
		return -EINVAL;
	dpy_list_add_tail(&bridge->head, &dpy_bridges);
	return 0;
}

void dpy_bridge_remove(struct dpy_bridge *bridge)
{
	dpy_list_del(&bridge->head);
}

struct dpy_bridge *dpy_bridge_find(const struct dpy_gnode *node)
{
	struct dpy_bridge *bridge;

	dpy_list_for_each_entry(bridge, &dpy_bridges, head)
		if (bridge->node == node)
			return bridge;
	return NULL;
}

int dpy_bridge_attach(struct dpy_encoder *encoder, struct dpy_bridge *bridge,
		      struct dpy_bridge *previous)
{
	int ret = 0;

	if (!bridge)
		return -EINVAL;
	if (bridge->encoder)
		return -EBUSY;
	bridge->encoder = encoder;
	if (bridge->funcs->attach)
		ret = bridge->funcs->attach(bridge, encoder);
	if (ret) {
		bridge->encoder = NULL;
		return ret;
	}
	if (previous)
		previous->next = bridge;
	else
		encoder->bridge = bridge;
	return 0;
}

void dpy_bridge_chain_pre_enable(struct dpy_bridge *first)
{
	/* from the panel side towards the encoder */
	if (!first)
		return;
	dpy_bridge_chain_pre_enable(first->next);
	if (first->funcs->pre_enable)
		first->funcs->pre_enable(first);
}

void dpy_bridge_chain_enable(struct dpy_bridge *first)
{
	struct dpy_bridge *b;

	for (b = first; b; b = b->next)
		if (b->funcs->enable)
			b->funcs->enable(b);
}

void dpy_bridge_chain_disable(struct dpy_bridge *first)
{
	if (!first)
		return;
	dpy_bridge_chain_disable(first->next);
	if (first->funcs->disable)
		first->funcs->disable(first);
}

void dpy_bridge_chain_post_disable(struct dpy_bridge *first)
{
	struct dpy_bridge *b;

	for (b = first; b; b = b->next)
		if (b->funcs->post_disable)
			b->funcs->post_disable(b);
}

/* ------------------------------------------------------------------ */
/* Sink lookup                                                         */
/* ------------------------------------------------------------------ */
int dpy_find_sink(const struct dpy_gnode *node, uint8_t port,
		  struct dpy_sink *sink)
{
	struct dpy_gnode *remote;

	memset(sink, 0, sizeof(*sink));
	remote = dpy_node_remote(node, port, DPY_EP_ANY, NULL, NULL);
	if (!remote)
		return -ENODEV;
	sink->bridge = dpy_bridge_find(remote);
	if (sink->bridge)
		return 0;
	sink->panel = dpy_panel_find(remote);
	if (sink->panel)
		return 0;
	dpy_err("%s: %s is neither a bound panel nor a bridge\n",
		node->desc->name, remote->desc->name);
	return -EAGAIN;
}
