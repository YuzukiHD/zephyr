/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display pipeline framework - panels, bridges and backlights.
 *
 * Power sequencing follows the DRM panel model:
 *   prepare()   - power supplies, reset, init commands (link may be in LP)
 *   enable()    - after video is flowing: display on, backlight on
 *   disable()   - backlight off, display off
 *   unprepare() - exit commands, reset, power off
 */
#ifndef __DPY_PANEL_H__
#define __DPY_PANEL_H__

#include <dpy/dpy_kms.h>

/* ------------------------------------------------------------------ */
/* Backlight                                                           */
/* ------------------------------------------------------------------ */
struct dpy_backlight;

struct dpy_backlight_ops {
	/* apply @level (0..max) and power state */
	int (*update)(struct dpy_backlight *bl, uint32_t level, bool on);
};

struct dpy_backlight {
	struct dpy_list head;
	const struct dpy_gnode *node;
	const struct dpy_backlight_ops *ops;
	void *priv;
	uint32_t max_level;
	uint32_t level;
	bool on;
};

int dpy_backlight_register(struct dpy_backlight *bl);
void dpy_backlight_unregister(struct dpy_backlight *bl);
struct dpy_backlight *dpy_backlight_find(const struct dpy_gnode *node);
int dpy_backlight_set_level(struct dpy_backlight *bl, uint32_t level);
int dpy_backlight_enable(struct dpy_backlight *bl);
int dpy_backlight_disable(struct dpy_backlight *bl);

/* ------------------------------------------------------------------ */
/* Panel                                                               */
/* ------------------------------------------------------------------ */
struct dpy_panel;

struct dpy_panel_funcs {
	int (*prepare)(struct dpy_panel *panel);
	int (*enable)(struct dpy_panel *panel);
	int (*disable)(struct dpy_panel *panel);
	int (*unprepare)(struct dpy_panel *panel);
	/* fill @modes, return the number of modes, first is preferred */
	int (*get_modes)(struct dpy_panel *panel,
			 struct dpy_display_mode *modes, int max);
};

struct dpy_panel {
	struct dpy_list head;
	const struct dpy_gnode *node;
	const struct dpy_panel_funcs *funcs;
	void *priv;
	struct dpy_backlight *backlight;
	struct dpy_display_info info;
	const char *name;
	bool prepared;
	bool enabled;
};

int dpy_panel_add(struct dpy_panel *panel);
void dpy_panel_remove(struct dpy_panel *panel);
struct dpy_panel *dpy_panel_find(const struct dpy_gnode *node);
int dpy_panel_prepare(struct dpy_panel *panel);
int dpy_panel_enable(struct dpy_panel *panel);
int dpy_panel_disable(struct dpy_panel *panel);
int dpy_panel_unprepare(struct dpy_panel *panel);
int dpy_panel_get_modes(struct dpy_panel *panel,
			struct dpy_display_mode *modes, int max);

/* ------------------------------------------------------------------ */
/* Bridge (external converter chips between encoder and panel)          */
/* ------------------------------------------------------------------ */
struct dpy_bridge;

struct dpy_bridge_funcs {
	int (*attach)(struct dpy_bridge *bridge, struct dpy_encoder *encoder);
	int (*mode_valid)(struct dpy_bridge *bridge,
			  const struct dpy_display_mode *mode);
	void (*pre_enable)(struct dpy_bridge *bridge);
	void (*enable)(struct dpy_bridge *bridge);
	void (*disable)(struct dpy_bridge *bridge);
	void (*post_disable)(struct dpy_bridge *bridge);
	int (*get_modes)(struct dpy_bridge *bridge,
			 struct dpy_display_mode *modes, int max);
};

struct dpy_bridge {
	struct dpy_list head;
	const struct dpy_gnode *node;
	const struct dpy_bridge_funcs *funcs;
	void *priv;
	struct dpy_bridge *next;	/* towards the panel */
	struct dpy_encoder *encoder;
	struct dpy_display_info info;
};

int dpy_bridge_add(struct dpy_bridge *bridge);
void dpy_bridge_remove(struct dpy_bridge *bridge);
struct dpy_bridge *dpy_bridge_find(const struct dpy_gnode *node);
/* attach @bridge (and the bridges following it) to @encoder */
int dpy_bridge_attach(struct dpy_encoder *encoder, struct dpy_bridge *bridge,
		      struct dpy_bridge *previous);
void dpy_bridge_chain_pre_enable(struct dpy_bridge *first);
void dpy_bridge_chain_enable(struct dpy_bridge *first);
void dpy_bridge_chain_disable(struct dpy_bridge *first);
void dpy_bridge_chain_post_disable(struct dpy_bridge *first);

/* ------------------------------------------------------------------ */
/* Sink lookup helper for encoders                                      */
/* ------------------------------------------------------------------ */
struct dpy_sink {
	struct dpy_panel *panel;
	struct dpy_bridge *bridge;
};

/*
 * Resolve what is connected to output port @port of @node: a bridge or a
 * panel. Returns -ENODEV if nothing is linked, -EPROBE_DEFER equivalent
 * (-EAGAIN) if the remote driver has not registered its object yet.
 */
int dpy_find_sink(const struct dpy_gnode *node, uint8_t port,
		  struct dpy_sink *sink);

#endif /* __DPY_PANEL_H__ */
