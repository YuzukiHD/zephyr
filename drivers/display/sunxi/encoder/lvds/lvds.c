// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * LVDS encoder: TCON serialiser + combo D-PHY output stage.
 */
#define DPY_LOG_TAG "lvds"
#include <dpy/dpy_log.h>

#include "phy/combo_dphy.h"
#include "encoder/encoder_common.h"

struct lvds_enc {
	struct dpy_encoder_dev edev;
	const struct dpy_lvds_pdata *pdata;
	struct combo_dphy *phy;
	struct dpy_reset *rst;
};

static const struct dpy_lvds_pdata lvds_enc_default_pdata;

#define to_lvds_enc(e) dpy_container_of(e, struct lvds_enc, edev.encoder)

static bool lvds_enc_bus_is_6bit(uint32_t bus_format)
{
	return bus_format == DPY_BUS_FMT_RGB666_1X7X3_SPWG;
}

static int lvds_enc_atomic_check(struct dpy_encoder *encoder,
				 struct dpy_crtc_state *cs,
				 struct dpy_connector_state *conn_state)
{
	struct lvds_enc *lvds = to_lvds_enc(encoder);
	uint32_t fmt = lvds->edev.connector.info.bus_format;

	(void)conn_state;
	switch (fmt) {
	case DPY_BUS_FMT_NONE:
		fmt = DPY_BUS_FMT_RGB888_1X7X4_SPWG;
		break;
	case DPY_BUS_FMT_RGB666_1X7X3_SPWG:
	case DPY_BUS_FMT_RGB888_1X7X4_SPWG:
	case DPY_BUS_FMT_RGB888_1X7X4_JEIDA:
		break;
	default:
		dpy_err("unsupported LVDS bus format %u\n", fmt);
		return -EINVAL;
	}
	cs->bus_format = fmt;
	cs->bus_flags = lvds->edev.connector.info.bus_flags;
	if (lvds->pdata->use_tcon_frm)
		cs->bus_format = DPY_BUS_FMT_RGB888_1X7X4_SPWG;
	return 0;
}

static void lvds_enc_atomic_enable(struct dpy_encoder *encoder,
				   struct dpy_atomic_state *state)
{
	struct lvds_enc *lvds = to_lvds_enc(encoder);
	const struct dpy_lvds_pdata *pd = lvds->pdata;
	const struct dpy_crtc_state *cs = dpy_encoder_dev_crtc_state(&lvds->edev);
	uint32_t fmt = lvds->edev.connector.info.bus_format;
	struct dpy_tcon_output cfg;

	(void)state;
	if (!cs)
		return;

	memset(&cfg, 0, sizeof(cfg));
	cfg.iface = DPY_TCON_IF_LVDS;
	cfg.mode = cs->mode;
	cfg.bus_flags = lvds->edev.connector.info.bus_flags;
	cfg.lvds.dual_link = pd->dual_link;
	cfg.lvds.jeida = fmt == DPY_BUS_FMT_RGB888_1X7X4_JEIDA;
	cfg.lvds.bpc6 = lvds_enc_bus_is_6bit(fmt);
	if (pd->use_tcon_frm && cfg.lvds.bpc6)
		cfg.lvds.frm = DPY_TCON_FRM_RGB666;

	dpy_pins_apply(&pd->pins);
	dpy_os_reset_deassert(lvds->rst);
	if (dpy_timing_ctrl_prepare(lvds->edev.tc, &cfg))
		return;
	if (combo_dphy_power_on(lvds->phy, 0) == 0)
		combo_dphy_lvds_enable(lvds->phy, pd->dual_link);
	dpy_encoder_dev_sink_prepare(&lvds->edev);
	dpy_timing_ctrl_enable(lvds->edev.tc);
	dpy_encoder_dev_sink_enable(&lvds->edev);
}

static void lvds_enc_atomic_disable(struct dpy_encoder *encoder,
				    struct dpy_atomic_state *state)
{
	struct lvds_enc *lvds = to_lvds_enc(encoder);

	(void)state;
	dpy_encoder_dev_sink_disable(&lvds->edev);
	dpy_timing_ctrl_disable(lvds->edev.tc);
	combo_dphy_lvds_disable(lvds->phy);
	combo_dphy_power_off(lvds->phy);
	dpy_encoder_dev_sink_unprepare(&lvds->edev);
	dpy_timing_ctrl_unprepare(lvds->edev.tc);
	dpy_os_reset_assert(lvds->rst);
	dpy_pins_release(&lvds->pdata->pins);
}

static const struct dpy_encoder_funcs lvds_enc_funcs = {
	.atomic_check = lvds_enc_atomic_check,
	.atomic_enable = lvds_enc_atomic_enable,
	.atomic_disable = lvds_enc_atomic_disable,
};

static int lvds_enc_bind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct lvds_enc *lvds = dev->priv;

	lvds->phy = combo_dphy_get(dev->node);
	if (!lvds->phy) {
		dpy_err("%s: no combo phy\n", dpy_dev_name(dev));
		return -ENODEV;
	}
	return dpy_encoder_dev_bind(&lvds->edev, ddev, &lvds_enc_funcs,
			       DPY_ENCODER_LVDS, DPY_CONNECTOR_LVDS);
}

static void lvds_enc_unbind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct lvds_enc *lvds = dev->priv;

	(void)ddev;
	dpy_encoder_dev_unbind(&lvds->edev);
}

static const struct dpy_component_ops lvds_enc_component_ops = {
	.bind = lvds_enc_bind,
	.unbind = lvds_enc_unbind,
};

static int lvds_enc_probe(struct dpy_dev *dev)
{
	struct lvds_enc *lvds;

	lvds = dpy_os_zalloc(sizeof(*lvds));
	if (!lvds)
		return -ENOMEM;
	lvds->edev.dev = dev;
	lvds->pdata = dpy_dev_pdata(dev);
	if (!lvds->pdata)
		lvds->pdata = &lvds_enc_default_pdata;
	lvds->rst = dpy_dev_reset_get(dev, "bus");
	dev->priv = lvds;
	return dpy_component_add(dev);
}

static void lvds_enc_remove(struct dpy_dev *dev)
{
	struct lvds_enc *lvds = dev->priv;

	dpy_component_del(dev);
	dpy_os_reset_put(lvds->rst);
	dpy_os_free(lvds);
	dev->priv = NULL;
}

static const struct dpy_match lvds_enc_match[] = {
	{ "allwinner,sunxi-lvds", NULL },
	{ NULL },
};

const struct dpy_driver dpy_lvds_driver = {
	.name = "lvds",
	.match = lvds_enc_match,
	.klass = DPY_COMP_ENCODER,
	.probe = lvds_enc_probe,
	.remove = lvds_enc_remove,
	.ops = &lvds_enc_component_ops,
};
