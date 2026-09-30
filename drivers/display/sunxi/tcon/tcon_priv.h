/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * TCON package private interfaces (tcon_top <-> tcon_lcd).
 */
#ifndef __TCON_PRIV_H__
#define __TCON_PRIV_H__

#include <dpy/dpy_device.h>
#include <dpy/dpy_tcon.h>

/* differences between TCON top blocks of different SoCs */
struct tcon_top_variant {
	uint8_t n_de_ports;	/* display engine outputs that can be routed */
	uint8_t n_tcons;	/* timing controllers behind them */
	uint8_t n_dsi_clk_gates; /* DSI hosts with a clock gate here */
};

struct tcon_top {
	struct dpy_dev *dev;
	const struct tcon_top_variant *var;
	uintptr_t base;
	struct dpy_clk *bus_clk;
	struct dpy_reset *rst;
	uint32_t users;
};

struct tcon_top *tcon_top_from_node(const struct dpy_gnode *node);
int tcon_top_get(struct tcon_top *top);
void tcon_top_put(struct tcon_top *top);
/* feed TCON @tcon_id from display engine output @de_port */
void tcon_top_route_de(struct tcon_top *top, uint32_t de_port,
		       uint32_t tcon_id);
/* hand the LCD pads to TCON LCD @tcon_id */
void tcon_top_lcd_to_pads(struct tcon_top *top, uint32_t tcon_id);
/* clock TCON LCD @tcon_id from combo phy @phy_id instead of the CCU */
void tcon_top_lcd_clk_from_phy(struct tcon_top *top, uint32_t tcon_id,
			       uint32_t phy_id, bool from_phy);
/* connect DSI @dsi_id to TCON @tcon_id and gate its clock */
void tcon_top_dsi_route(struct tcon_top *top, uint32_t dsi_id,
			uint32_t tcon_id, bool enable);
void tcon_top_dump(struct tcon_top *top, void (*print)(const char *fmt, ...));

/* pixel clock plan */
struct tcon_clk_plan {
	uint32_t mod_rate;	/* rate of the TCON module clock */
	uint32_t div;		/* dclk divider */
	uint32_t pixclk;	/* resulting pixel clock */
};

/* @max_rate: highest module clock the TCON is specified for */
int tcon_clk_plan(struct dpy_clk *mod, uint32_t pixclk_hz, uint32_t min_div,
		  uint32_t max_div, uint32_t max_rate,
		  struct tcon_clk_plan *plan);

#endif /* __TCON_PRIV_H__ */
