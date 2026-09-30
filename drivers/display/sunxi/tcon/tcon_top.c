// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * TCON top (display interface top) driver.
 *
 * Owns the glue registers shared by the timing controllers: which TCON is
 * fed by which display engine output, who drives the LCD pads, where the
 * TCON pixel clock comes from and the DSI source/clock gate.
 */
#define DPY_LOG_TAG "tcon-top"
#include <dpy/dpy_log.h>

#include "tcon_priv.h"
#include "tcon_regs.h"

struct tcon_top *tcon_top_from_node(const struct dpy_gnode *node)
{
	struct dpy_dev *dev = dpy_dev_from_node(node);

	if (!dev || dev->drv->klass != DPY_COMP_TOP)
		return NULL;
	return dev->priv;
}

int tcon_top_get(struct tcon_top *top)
{
	int ret;

	if (top->users++)
		return 0;
	ret = dpy_os_reset_deassert(top->rst);
	if (!ret) {
		ret = dpy_os_clk_enable(top->bus_clk);
		if (ret)
			dpy_os_reset_assert(top->rst);
	}
	if (ret) {
		dpy_err("cannot power up: %d\n", ret);
		top->users--;
	}
	return ret;
}

void tcon_top_put(struct tcon_top *top)
{
	if (!top->users || --top->users)
		return;
	dpy_os_clk_disable(top->bus_clk);
	dpy_os_reset_assert(top->rst);
}

void tcon_top_route_de(struct tcon_top *top, uint32_t de_port,
		       uint32_t tcon_id)
{
	uintptr_t reg = top->base + TCON_TOP_DE_PERH;
	uint32_t shift = de_port ? 4 : 0;
	uint32_t other_shift = de_port ? 0 : 4;
	uint32_t val = dpy_readl(reg);
	uint32_t old = (val >> shift) & 0x3;

	if (de_port >= top->var->n_de_ports || tcon_id >= top->var->n_tcons) {
		dpy_warn("no route from DE port %u to TCON %u\n", de_port,
			 tcon_id);
		return;
	}

	/* two display engine ports must never feed the same TCON: swap */
	if (((val >> other_shift) & 0x3) == tcon_id)
		val = (val & ~(0x3U << other_shift)) | (old << other_shift);
	val = (val & ~(0x3U << shift)) | ((tcon_id & 0x3) << shift);
	dpy_writel(val, reg);
}

void tcon_top_lcd_to_pads(struct tcon_top *top, uint32_t tcon_id)
{
	if (tcon_id >= top->var->n_tcons)
		return;
	dpy_updatel(top->base + TCON_TOP_TV_SETUP,
		    tcon_id ? TCON_TOP_TV1_OUT : TCON_TOP_TV0_OUT, 0);
}

void tcon_top_lcd_clk_from_phy(struct tcon_top *top, uint32_t tcon_id,
			       uint32_t phy_id, bool from_phy)
{
	uint32_t mask = TCON_TOP_LCD_CLK_SRC(tcon_id) |
			TCON_TOP_PHY_CLK_SRC(phy_id);

	dpy_updatel(top->base + TCON_TOP_CLK_SRC, mask, from_phy ? mask : 0);
}

void tcon_top_dsi_route(struct tcon_top *top, uint32_t dsi_id,
			uint32_t tcon_id, bool enable)
{
	dpy_updatel(top->base + TCON_TOP_DSI_SRC, TCON_TOP_DSI_SRC_SEL(dsi_id),
		    tcon_id != dsi_id ? TCON_TOP_DSI_SRC_SEL(dsi_id) : 0);
	if (dsi_id < top->var->n_dsi_clk_gates)
		dpy_updatel(top->base + TCON_TOP_CLK_GATE, TCON_TOP_DSI_CLK_GATE,
			    enable ? TCON_TOP_DSI_CLK_GATE : 0);
}

void tcon_top_dump(struct tcon_top *top, void (*print)(const char *fmt, ...))
{
	print("tcon_top: tv_setup %08x dsi_src %08x clk_src %08x de_perh %08x clk_gate %08x users %u\n",
	      dpy_readl(top->base + TCON_TOP_TV_SETUP),
	      dpy_readl(top->base + TCON_TOP_DSI_SRC),
	      dpy_readl(top->base + TCON_TOP_CLK_SRC),
	      dpy_readl(top->base + TCON_TOP_DE_PERH),
	      dpy_readl(top->base + TCON_TOP_CLK_GATE), top->users);
}

static int tcon_top_probe(struct dpy_dev *dev)
{
	struct tcon_top *top;

	top = dpy_os_zalloc(sizeof(*top));
	if (!top)
		return -ENOMEM;
	top->dev = dev;
	top->var = dpy_dev_match_data(dev);
	top->base = dpy_dev_ioremap(dev, "reg", NULL);
	top->bus_clk = dpy_dev_clk_get(dev, "bus");
	top->rst = dpy_dev_reset_get(dev, "bus");
	if (!top->base || !top->var) {
		dpy_os_free(top);
		return -ENODEV;
	}
	dev->priv = top;
	return dpy_component_add(dev);
}

static void tcon_top_remove(struct dpy_dev *dev)
{
	struct tcon_top *top = dev->priv;

	dpy_component_del(dev);
	dpy_os_clk_put(top->bus_clk);
	dpy_os_reset_put(top->rst);
	dpy_os_free(top);
	dev->priv = NULL;
}

/* two display engine outputs, two TCONs, one DSI clock gate */
static const struct tcon_top_variant tcon_top_variant_default = {
	.n_de_ports = 2,
	.n_tcons = 2,
	.n_dsi_clk_gates = 1,
};

static const struct dpy_match tcon_top_match[] = {
	{ "allwinner,sunxi-tcon-top", &tcon_top_variant_default },
	{ NULL },
};

const struct dpy_driver dpy_tcon_top_driver = {
	.name = "tcon-top",
	.match = tcon_top_match,
	.klass = DPY_COMP_TOP,
	.probe = tcon_top_probe,
	.remove = tcon_top_remove,
};
