// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Combo D-PHY driver: MIPI D-PHY transmitter for the DSI host and the
 * LVDS serialiser output stage, sharing one analog block and one PLL.
 */
#define DPY_LOG_TAG "dphy"
#include <dpy/dpy_log.h>
#include <dpy/dpy_pdata.h>

#include "combo_dphy.h"
#include "combo_dphy_regs.h"

/*
 * Analog trim, PLL limits and lane timing that differ between phys of
 * different SoCs, selected by the matched compatible.
 */
struct combo_dphy_variant {
	uint32_t ref_hz;		/* PLL reference clock */
	uint32_t vco_min_hz;
	uint8_t max_lanes;
	uint8_t pll_m_max;		/* post divider */
	uint8_t pll_div1_max;		/* low speed divider */

	/* lane timing defaults, in lane byte clock periods */
	uint8_t lpx, hs_prepare, hs_trail;
	uint8_t clk_prepare, clk_zero, clk_pre, clk_post, clk_trail;
	uint8_t hs_delay, ulps_exit;

	/* analog trim, MIPI D-PHY mode */
	uint8_t hstx_ana;		/* HSTX_ANA0 and 1 */
	uint8_t lptx_setc, lptx_setr;
	uint8_t ib, vres_set, vtt_set, vlptx_set, vlv_set;
	uint8_t hs_stop_dly;

	/* analog trim, LVDS mode */
	uint8_t lvds_vref1p6_single, lvds_vref1p6_dual, lvds_vref0p8;
	uint8_t lvds_ib;
};

static const struct combo_dphy_variant combo_dphy_variant_default = {
	.ref_hz = 24000000U,
	.vco_min_hz = 1100000000U,
	.max_lanes = 4,
	.pll_m_max = 16,
	.pll_div1_max = 16,
	.lpx = 14, .hs_prepare = 6, .hs_trail = 4,
	.clk_prepare = 7, .clk_zero = 50, .clk_pre = 3, .clk_post = 10,
	.clk_trail = 30, .hs_delay = 10, .ulps_exit = 3,
	.hstx_ana = 3,
	.lptx_setc = 7, .lptx_setr = 7,
	.ib = 4, .vres_set = 3, .vtt_set = 1, .vlptx_set = 3, .vlv_set = 4,
	.hs_stop_dly = 20,
	.lvds_vref1p6_single = 5, .lvds_vref1p6_dual = 6, .lvds_vref0p8 = 3,
	.lvds_ib = 4,
};

struct combo_dphy {
	struct dpy_dev *dev;
	const struct combo_dphy_variant *var;
	uintptr_t base;
	struct dpy_clk *mod_clk;
	struct dpy_clk *bus_clk;
	const struct dpy_combo_dphy_soc_pdata *soc;
	uint32_t users;
	bool calibrated;
	uint32_t hs_clk;
};

static inline void dphy_write(struct combo_dphy *p, uint32_t reg, uint32_t v)
{
	dpy_writel(v, p->base + reg);
}

static inline uint32_t dphy_read(struct combo_dphy *p, uint32_t reg)
{
	return dpy_readl(p->base + reg);
}

static inline void dphy_update(struct combo_dphy *p, uint32_t reg,
			       uint32_t mask, uint32_t val)
{
	dpy_updatel(p->base + reg, mask, val);
}

struct combo_dphy *combo_dphy_get(const struct dpy_gnode *consumer)
{
	struct dpy_gnode *node = dpy_node_get_ref(consumer, "phy");
	struct dpy_dev *dev = dpy_dev_from_node(node);

	if (!dev || dev->drv->klass != DPY_COMP_PHY)
		return NULL;
	return dev->priv;
}

uint8_t combo_dphy_id(const struct combo_dphy *phy)
{
	return phy->dev->node->desc->id;
}

int combo_dphy_power_on(struct combo_dphy *phy, uint32_t mod_rate)
{
	int ret;

	if (phy->users++)
		return 0;

	dpy_dev_clk_setup(phy->dev, "mod", phy->mod_clk);
	if (mod_rate)
		dpy_os_clk_set_rate(phy->mod_clk, mod_rate);
	ret = dpy_os_clk_enable(phy->bus_clk);
	if (!ret)
		ret = dpy_os_clk_enable(phy->mod_clk);
	if (ret) {
		dpy_os_clk_disable(phy->bus_clk);
		phy->users--;
		return ret;
	}
	if (!phy->calibrated && phy->soc && phy->soc->calibrate) {
		phy->soc->calibrate();
		phy->calibrated = true;
	}
	return 0;
}

void combo_dphy_power_off(struct combo_dphy *phy)
{
	if (!phy->users || --phy->users)
		return;
	dpy_os_clk_disable(phy->mod_clk);
	dpy_os_clk_disable(phy->bus_clk);
}

/* ------------------------------------------------------------------ */
/* LVDS                                                                */
/* ------------------------------------------------------------------ */
int combo_dphy_lvds_enable(struct combo_dphy *phy, bool dual_link)
{
	/* reference voltages of the LVDS driver */
	dphy_write(phy, COMBO_PHY_REG1,
		   DPY_FIELD_PREP(COMBO_PHY_VREF1P6,
				  dual_link ? phy->var->lvds_vref1p6_dual :
					      phy->var->lvds_vref1p6_single) |
		   DPY_FIELD_PREP(COMBO_PHY_VREF0P8, phy->var->lvds_vref0p8));
	/* charge pump, then LVDS mode, then the LDO */
	dphy_write(phy, COMBO_PHY_REG0, COMBO_PHY_EN_CP);
	dpy_os_udelay(5);
	dphy_write(phy, COMBO_PHY_REG0, COMBO_PHY_EN_CP | COMBO_PHY_EN_LVDS);
	dpy_os_udelay(5);
	dphy_write(phy, COMBO_PHY_REG0,
		   COMBO_PHY_EN_CP | COMBO_PHY_EN_LVDS | COMBO_PHY_EN_LDO);
	dpy_os_udelay(5);

	dphy_write(phy, DPHY_ANA4, DPHY_ANA4_EN_MIPI |
		   DPY_FIELD_PREP(DPHY_ANA4_IB, phy->var->lvds_ib));
	dphy_write(phy, DPHY_ANA3, DPHY_ANA3_ENLDOD | DPHY_ANA3_ENLDOR);
	dphy_write(phy, DPHY_ANA2, 0);
	dphy_write(phy, DPHY_ANA1, 0);
	return 0;
}

void combo_dphy_lvds_disable(struct combo_dphy *phy)
{
	dphy_write(phy, COMBO_PHY_REG1, 0);
	dphy_write(phy, COMBO_PHY_REG0, 0);
	dphy_write(phy, DPHY_ANA4, 0);
	dphy_write(phy, DPHY_ANA3, 0);
	dphy_write(phy, DPHY_ANA1, 0);
}

/* ------------------------------------------------------------------ */
/* MIPI D-PHY                                                          */
/* ------------------------------------------------------------------ */
struct dphy_pll_plan {
	uint32_t n, m, div0, div1;
};

/*
 * The PLL runs at the per-lane bit rate times the post divider m; the low
 * speed output (div0 * div1 = m * bits per lane per pixel) is the pixel
 * clock that drives the TCON in DSI mode. Only integer bits per lane per
 * pixel can be produced.
 */
static int dphy_pll_plan(const struct combo_dphy_variant *v,
			 uint32_t pixclk_hz, uint32_t bpp, uint32_t lanes,
			 struct dphy_pll_plan *p)
{
	uint32_t coef;
	uint64_t rate;

	if (!lanes || !pixclk_hz || bpp % lanes)
		return -EINVAL;
	coef = bpp / lanes;
	rate = (uint64_t)pixclk_hz * coef;

	/* smallest post divider keeping the VCO in range */
	p->m = 1;
	while (rate * p->m < v->vco_min_hz && p->m < v->pll_m_max)
		p->m++;
	p->n = (uint32_t)(rate * p->m / v->ref_hz);
	p->div0 = (coef % 3 == 0) ? 3 : (coef % 2 == 0) ? 2 : 1;
	p->div1 = p->m * coef / p->div0;

	if (rate * p->m < v->vco_min_hz || p->div1 > v->pll_div1_max ||
	    p->n > DPY_FIELD_GET(DPHY_PLL0_N, DPHY_PLL0_N))
		return -ERANGE;
	return 0;
}

int combo_dphy_dsi_check(const struct combo_dphy *phy,
			 const struct combo_dphy_dsi_cfg *cfg)
{
	struct dphy_pll_plan p;

	return dphy_pll_plan(phy->var, cfg->pixclk_hz, cfg->bpp, cfg->lanes,
			     &p);
}

/* program the PLL, returns the high speed clock */
static uint32_t dphy_set_pll(struct combo_dphy *phy,
			     const struct dphy_pll_plan *p)
{
	uint32_t n = p->n, m = p->m, div0 = p->div0, div1 = p->div1;

	dphy_update(phy, DPHY_PLL1, DPHY_PLL1_LS_GATING | DPHY_PLL1_HS_GATING,
		    DPHY_PLL1_LS_GATING | DPHY_PLL1_HS_GATING);
	dphy_write(phy, DPHY_PLL2, 0);	/* no spread spectrum */
	dphy_update(phy, DPHY_PLL0,
		    DPHY_PLL0_N | DPHY_PLL0_P | DPHY_PLL0_M0 | DPHY_PLL0_M1 |
		    DPHY_PLL0_LS_DIV0 | DPHY_PLL0_LS_DIV1 |
		    DPHY_PLL0_PLL_EN | DPHY_PLL0_LDO_EN,
		    DPY_FIELD_PREP(DPHY_PLL0_N, n) |
		    DPY_FIELD_PREP(DPHY_PLL0_P, 0) |
		    DPY_FIELD_PREP(DPHY_PLL0_M0, 0) |
		    DPY_FIELD_PREP(DPHY_PLL0_M1, m - 1) |
		    DPY_FIELD_PREP(DPHY_PLL0_LS_DIV0, div0 - 1) |
		    DPY_FIELD_PREP(DPHY_PLL0_LS_DIV1, div1 - 1) |
		    DPHY_PLL0_PLL_EN | DPHY_PLL0_LDO_EN);
	dphy_update(phy, DPHY_PLL1, DPHY_PLL1_LOCKDET_EN, DPHY_PLL1_LOCKDET_EN);
	dphy_update(phy, DPHY_PLL0, DPHY_PLL0_REG_UPDATE, DPHY_PLL0_REG_UPDATE);

	return (uint32_t)((uint64_t)phy->var->ref_hz * n / m);
}

static void dphy_set_timing(struct combo_dphy *phy,
			    const struct combo_dphy_dsi_cfg *cfg)
{
	const struct combo_dphy_variant *v = phy->var;
	uint8_t hs_trail = cfg->hs_trail ? cfg->hs_trail : v->hs_trail;
	uint8_t clk_trail = cfg->clk_trail ? cfg->clk_trail : v->clk_trail;

	dphy_update(phy, DPHY_GCTL, DPHY_GCTL_MODULE_EN | DPHY_GCTL_LANE_NUM,
		    DPY_FIELD_PREP(DPHY_GCTL_LANE_NUM, cfg->lanes - 1));
	dphy_update(phy, DPHY_TX_CTL, DPHY_TX_HSTX_CLK_CONT,
		    DPHY_TX_HSTX_CLK_CONT);
	dphy_write(phy, DPHY_TX_TIME0,
		   DPY_FIELD_PREP(DPHY_TX_LPX, v->lpx) |
		   DPY_FIELD_PREP(DPHY_TX_HS_PRE, v->hs_prepare) |
		   DPY_FIELD_PREP(DPHY_TX_HS_TRAIL, hs_trail));
	dphy_write(phy, DPHY_TX_TIME1,
		   DPY_FIELD_PREP(DPHY_TX_CK_PREP, v->clk_prepare) |
		   DPY_FIELD_PREP(DPHY_TX_CK_ZERO, v->clk_zero) |
		   DPY_FIELD_PREP(DPHY_TX_CK_PRE, v->clk_pre) |
		   DPY_FIELD_PREP(DPHY_TX_CK_POST, v->clk_post));
	dphy_write(phy, DPHY_TX_TIME2,
		   DPY_FIELD_PREP(DPHY_TX_CK_TRAIL, clk_trail) |
		   DPY_FIELD_PREP(DPHY_TX_HS_DLY, v->hs_delay));
	dphy_write(phy, DPHY_TX_TIME3,
		   DPY_FIELD_PREP(DPHY_TX_ULPS_EXIT, v->ulps_exit));
	dphy_write(phy, DPHY_TX_TIME4,
		   DPY_FIELD_PREP(DPHY_TX_HSTX_ANA0, v->hstx_ana) |
		   DPY_FIELD_PREP(DPHY_TX_HSTX_ANA1, v->hstx_ana));
}

int combo_dphy_dsi_enable(struct combo_dphy *phy,
			  const struct combo_dphy_dsi_cfg *cfg,
			  uint32_t *hs_clk_hz)
{
	const struct combo_dphy_variant *v = phy->var;
	struct dphy_pll_plan pll;
	uint32_t lanes_mask;
	int ret;

	if (!cfg->lanes || cfg->lanes > v->max_lanes || !cfg->bpp)
		return -EINVAL;
	ret = dphy_pll_plan(v, cfg->pixclk_hz, cfg->bpp, cfg->lanes, &pll);
	if (ret) {
		dpy_err("no PLL setting for %u Hz, %u bpp on %u lane(s)\n",
			cfg->pixclk_hz, cfg->bpp, cfg->lanes);
		return ret;
	}
	lanes_mask = (1U << cfg->lanes) - 1;

	dphy_set_timing(phy, cfg);

	/* analog trim: bias, LP/HS drive levels, termination calibration */
	dphy_write(phy, DPHY_ANA4,
		   DPHY_ANA4_EN_MIPI | DPY_FIELD_PREP(DPHY_ANA4_IB, v->ib) |
		   DPY_FIELD_PREP(DPHY_ANA4_VRES_SET, v->vres_set) |
		   DPY_FIELD_PREP(DPHY_ANA4_VTT_SET, v->vtt_set) |
		   DPY_FIELD_PREP(DPHY_ANA4_VLPTX_SET, v->vlptx_set) |
		   DPY_FIELD_PREP(DPHY_ANA4_VLV_SET, v->vlv_set) |
		   DPHY_ANA4_EN_RESCAL);
	dphy_update(phy, DPHY_ANA2, DPHY_ANA2_ENCK_CPU | DPHY_ANA2_ENIB,
		    DPHY_ANA2_ENCK_CPU | DPHY_ANA2_ENIB);
	dphy_update(phy, DPHY_ANA3,
		    DPHY_ANA3_ENLDOR | DPHY_ANA3_ENLDOC | DPHY_ANA3_ENLDOD,
		    DPHY_ANA3_ENLDOR | DPHY_ANA3_ENLDOC | DPHY_ANA3_ENLDOD);
	dphy_update(phy, DPHY_ANA0, DPHY_ANA0_LPTX_SETC | DPHY_ANA0_LPTX_SETR,
		    DPY_FIELD_PREP(DPHY_ANA0_LPTX_SETC, v->lptx_setc) |
		    DPY_FIELD_PREP(DPHY_ANA0_LPTX_SETR, v->lptx_setr));
	dphy_update(phy, COMBO_PHY_REG0, COMBO_PHY_EN_CP, COMBO_PHY_EN_CP);

	phy->hs_clk = dphy_set_pll(phy, &pll);
	dpy_os_udelay(20);

	dphy_update(phy, COMBO_PHY_REG0, COMBO_PHY_EN_MIPI | COMBO_PHY_EN_LDO,
		    COMBO_PHY_EN_MIPI | COMBO_PHY_EN_LDO);
	dphy_update(phy, COMBO_PHY_REG2, COMBO_PHY_HS_STOP_DLY,
		    DPY_FIELD_PREP(COMBO_PHY_HS_STOP_DLY, v->hs_stop_dly));
	dpy_os_udelay(1);

	dphy_update(phy, DPHY_ANA3,
		    DPHY_ANA3_ENVTTC | DPHY_ANA3_ENVTTD | DPHY_ANA3_ENDIV,
		    DPHY_ANA3_ENVTTC | DPHY_ANA3_ENDIV |
		    DPY_FIELD_PREP(DPHY_ANA3_ENVTTD, lanes_mask));
	dphy_update(phy, DPHY_ANA1, DPHY_ANA1_VTTMODE, DPHY_ANA1_VTTMODE);
	dphy_update(phy, DPHY_ANA2, DPHY_ANA2_ENP2S_CPU,
		    DPY_FIELD_PREP(DPHY_ANA2_ENP2S_CPU, lanes_mask));
	dphy_update(phy, DPHY_GCTL, DPHY_GCTL_MODULE_EN, DPHY_GCTL_MODULE_EN);

	if (hs_clk_hz)
		*hs_clk_hz = phy->hs_clk;
	dpy_info("D-PHY %u lane(s), HS clock %u Hz\n", cfg->lanes, phy->hs_clk);
	return 0;
}

void combo_dphy_dsi_disable(struct combo_dphy *phy)
{
	dphy_update(phy, DPHY_ANA2, DPHY_ANA2_ENP2S_CPU, 0);
	dphy_update(phy, DPHY_ANA1, DPHY_ANA1_VTTMODE, 0);
	dpy_os_udelay(1);
	dphy_update(phy, DPHY_ANA2, DPHY_ANA2_ENCK_CPU, 0);
	dpy_os_udelay(1);
	dphy_update(phy, DPHY_ANA3, DPHY_ANA3_ENDIV, 0);
	dpy_os_udelay(1);
	dphy_update(phy, DPHY_ANA3, DPHY_ANA3_ENVTTD | DPHY_ANA3_ENVTTC, 0);
	dpy_os_udelay(1);
	dphy_update(phy, DPHY_ANA3,
		    DPHY_ANA3_ENLDOD | DPHY_ANA3_ENLDOC | DPHY_ANA3_ENLDOR, 0);
	dpy_os_udelay(5);
	dphy_update(phy, DPHY_ANA2, DPHY_ANA2_ENIB, 0);
	dphy_write(phy, DPHY_ANA4, 0);
	dphy_write(phy, DPHY_ANA0, 0);
	dphy_update(phy, DPHY_ANA1, DPHY_ANA1_SVTT | DPHY_ANA1_CSMPS, 0);
	dphy_update(phy, DPHY_PLL0, DPHY_PLL0_PLL_EN, 0);
	dphy_write(phy, COMBO_PHY_REG0, 0);
	dphy_update(phy, DPHY_GCTL, DPHY_GCTL_MODULE_EN, 0);
}

void combo_dphy_dump(struct combo_dphy *phy,
		     void (*print)(const char *fmt, ...))
{
	static const uint16_t regs[] = {
		DPHY_GCTL, DPHY_TX_CTL, DPHY_TX_TIME0, DPHY_TX_TIME1,
		DPHY_TX_TIME2, DPHY_ANA0, DPHY_ANA1, DPHY_ANA2, DPHY_ANA3,
		DPHY_ANA4, DPHY_PLL0, DPHY_PLL1, DPHY_PLL2, COMBO_PHY_REG0,
		COMBO_PHY_REG1, COMBO_PHY_REG2,
	};
	unsigned int i;

	print("dphy: users %u, hs clock %u Hz\n", phy->users, phy->hs_clk);
	if (!phy->users)
		return;
	for (i = 0; i < DPY_ARRAY_SIZE(regs); i++)
		print("  +%03x: %08x\n", regs[i], dphy_read(phy, regs[i]));
}

/* ------------------------------------------------------------------ */
/* Component                                                           */
/* ------------------------------------------------------------------ */
static int combo_dphy_probe(struct dpy_dev *dev)
{
	struct combo_dphy *phy;

	phy = dpy_os_zalloc(sizeof(*phy));
	if (!phy)
		return -ENOMEM;
	phy->dev = dev;
	phy->var = dpy_dev_match_data(dev);
	phy->base = dpy_dev_ioremap(dev, "reg", NULL);
	phy->mod_clk = dpy_dev_clk_get(dev, "mod");
	phy->bus_clk = dpy_dev_clk_get(dev, "bus");
	phy->soc = dpy_dev_pdata(dev);
	if (!phy->base || !phy->var) {
		dpy_os_free(phy);
		return -ENODEV;
	}
	dev->priv = phy;
	return dpy_component_add(dev);
}

static void combo_dphy_remove(struct dpy_dev *dev)
{
	struct combo_dphy *phy = dev->priv;

	dpy_component_del(dev);
	dpy_os_clk_put(phy->mod_clk);
	dpy_os_clk_put(phy->bus_clk);
	dpy_os_free(phy);
	dev->priv = NULL;
}

static const struct dpy_match combo_dphy_match[] = {
	{ "allwinner,sunxi-combo-dphy", &combo_dphy_variant_default },
	{ NULL },
};

const struct dpy_driver dpy_combo_dphy_driver = {
	.name = "combo-dphy",
	.match = combo_dphy_match,
	.klass = DPY_COMP_PHY,
	.probe = combo_dphy_probe,
	.remove = combo_dphy_remove,
};
