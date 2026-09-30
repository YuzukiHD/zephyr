/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Combo D-PHY (MIPI D-PHY TX / LVDS) provider interface.
 */
#ifndef __COMBO_DPHY_H__
#define __COMBO_DPHY_H__

#include <dpy/dpy_device.h>

struct combo_dphy;

struct combo_dphy_dsi_cfg {
	uint32_t lanes;
	uint32_t pixclk_hz;
	uint32_t bpp;
	uint8_t hs_trail;	/* 0: default */
	uint8_t clk_trail;	/* 0: default */
};

/* provider referenced by @consumer's "phy" reference */
struct combo_dphy *combo_dphy_get(const struct dpy_gnode *consumer);
/* hardware instance number of the phy (for the TCON top routing) */
uint8_t combo_dphy_id(const struct combo_dphy *phy);

int combo_dphy_power_on(struct combo_dphy *phy, uint32_t mod_rate);
void combo_dphy_power_off(struct combo_dphy *phy);

int combo_dphy_lvds_enable(struct combo_dphy *phy, bool dual_link);
void combo_dphy_lvds_disable(struct combo_dphy *phy);

/* program lane timing and start the PLL; returns the HS bit clock */
/* 0 when the PLL can produce the configuration (usable in atomic_check) */
int combo_dphy_dsi_check(const struct combo_dphy *phy,
			 const struct combo_dphy_dsi_cfg *cfg);
int combo_dphy_dsi_enable(struct combo_dphy *phy,
			  const struct combo_dphy_dsi_cfg *cfg,
			  uint32_t *hs_clk_hz);
void combo_dphy_dsi_disable(struct combo_dphy *phy);

void combo_dphy_dump(struct combo_dphy *phy,
		     void (*print)(const char *fmt, ...));

#endif /* __COMBO_DPHY_H__ */
