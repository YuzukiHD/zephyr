// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * MIPI DSI encoder and DSI host.
 *
 * Probe registers the DSI host so that the panel driver can attach its
 * DSI device while it binds (panels bind before encoders). The encoder
 * then sequences TCON, combo D-PHY, DSI host and panel:
 *
 *   TCON prepare -> D-PHY on -> host config -> HS clock
 *   -> panel prepare (init commands in LP) -> TCON enable
 *   -> video stream (host owns the vblank interrupt) -> panel enable
 */
#define DPY_LOG_TAG "dsi"
#include <dpy/dpy_dsi.h>
#include <dpy/dpy_log.h>

#include "phy/combo_dphy.h"
#include "dsi_host.h"
#include "encoder/encoder_common.h"

struct dsi_enc {
	struct dpy_encoder_dev edev;
	struct dpy_dsi_host host;
	struct dpy_vblank_source vsrc;
	struct dsi_hw hw;
	struct combo_dphy *phy;
	const struct dpy_dsi_pdata *pdata;
	struct dpy_clk *mod_clk;
	struct dpy_clk *bus_clk;
	struct dpy_reset *rst;
	struct dpy_mutex *xfer_lock;
	/* command mode: a transfer owns the link, frame_start stays away */
	dpy_spinlock_t link_lock;
	bool xfer_busy;
	int irq;
	bool powered;
	bool video_running;
	uint32_t dsi_mode;		/* enum dpy_dsi_mode */
};

static const struct dpy_dsi_pdata dsi_enc_default_pdata;

#define to_dsi_enc(e) dpy_container_of(e, struct dsi_enc, edev.encoder)

static uint32_t dsi_enc_mode(const struct dpy_dsi_device *d)
{
	if (!(d->mode_flags & DPY_DSI_MODE_VIDEO))
		return DPY_DSI_COMMAND;
	if (d->mode_flags & DPY_DSI_MODE_VIDEO_BURST)
		return DPY_DSI_VIDEO_BURST;
	return DPY_DSI_VIDEO_SYNC_PULSE;
}

/* ------------------------------------------------------------------ */
/* DSI host                                                            */
/* ------------------------------------------------------------------ */
static int dsi_enc_host_attach(struct dpy_dsi_host *host,
			       struct dpy_dsi_device *dev)
{
	if (!dev->lanes || dev->lanes > 4) {
		dpy_err("invalid lane count %u\n", dev->lanes);
		return -EINVAL;
	}
	dpy_info("device attached: %u lane(s), format %u, flags %#x\n",
		 dev->lanes, dev->format, dev->mode_flags);
	(void)host;
	return 0;
}

static int dsi_enc_host_transfer(struct dpy_dsi_host *host,
				 const struct dpy_dsi_msg *msg)
{
	struct dsi_enc *dsi = host->priv;
	unsigned long flags;
	bool hold;
	int ret;

	if (!dsi->powered)
		return -EINVAL;

	dpy_os_mutex_lock(dsi->xfer_lock);
	/*
	 * The instruction engine loops over the video sequence while a
	 * stream runs: pause it at the end of the frame for any command.
	 */
	hold = dsi->video_running;
	if (hold) {
		dsi_hw_video_hold(&dsi->hw, true);
		dpy_os_msleep(20);
	}
	flags = dpy_os_spin_lock_irqsave(&dsi->link_lock);
	dsi->xfer_busy = true;
	dpy_os_spin_unlock_irqrestore(&dsi->link_lock, flags);

	ret = dsi_hw_transfer(&dsi->hw, msg);

	flags = dpy_os_spin_lock_irqsave(&dsi->link_lock);
	dsi->xfer_busy = false;
	dpy_os_spin_unlock_irqrestore(&dsi->link_lock, flags);
	if (hold)
		dsi_hw_video_hold(&dsi->hw, false);
	dpy_os_mutex_unlock(dsi->xfer_lock);
	return ret;
}

static const struct dpy_dsi_host_ops dsi_enc_host_ops = {
	.attach = dsi_enc_host_attach,
	.transfer = dsi_enc_host_transfer,
};

/* ------------------------------------------------------------------ */
/* Vblank source (video mode) and frame trigger (command mode)          */
/* ------------------------------------------------------------------ */
static int dsi_enc_vblank_enable(struct dpy_vblank_source *src, bool on)
{
	struct dsi_enc *dsi = src->priv;

	if (!dsi->powered)
		return on ? -EINVAL : 0;
	dsi_hw_irq_enable(&dsi->hw, DSI_HW_IRQ_VIDEO_VBLK, on);
	return 0;
}

static uint32_t dsi_enc_get_line(struct dpy_vblank_source *src)
{
	struct dsi_enc *dsi = src->priv;

	return dsi->powered ? dsi_hw_get_line(&dsi->hw) : 0;
}

/* TCON interrupt, command mode: send the next frame unless a command owns the link */
static bool dsi_enc_frame_start(struct dpy_vblank_source *src)
{
	struct dsi_enc *dsi = src->priv;
	unsigned long flags;
	bool run;

	flags = dpy_os_spin_lock_irqsave(&dsi->link_lock);
	run = !dsi->xfer_busy && !dsi_hw_wait_idle(&dsi->hw, 50);
	if (run)
		dsi_hw_run(&dsi->hw, DSI_SEQ_HS_VIDEO);
	dpy_os_spin_unlock_irqrestore(&dsi->link_lock, flags);
	return run;
}

static void dsi_enc_irq_handler(void *data)
{
	struct dsi_enc *dsi = data;
	uint32_t pending = dsi_hw_irq_ack(&dsi->hw);

	if (pending & DPY_BIT(DSI_HW_IRQ_VIDEO_VBLK))
		dpy_timing_ctrl_vblank(dsi->edev.tc);
}

/* ------------------------------------------------------------------ */
/* Encoder                                                             */
/* ------------------------------------------------------------------ */
static int dsi_enc_atomic_check(struct dpy_encoder *encoder,
				struct dpy_crtc_state *cs,
				struct dpy_connector_state *conn_state)
{
	struct dsi_enc *dsi = to_dsi_enc(encoder);
	struct dpy_dsi_device *d = dsi->host.device;
	struct combo_dphy_dsi_cfg phy_cfg;
	struct dsi_hw_cfg hw_cfg;

	(void)conn_state;
	if (!d)
		return -ENODEV;

	memset(&hw_cfg, 0, sizeof(hw_cfg));
	hw_cfg.mode = cs->mode;
	hw_cfg.format = d->format;
	hw_cfg.dsi_mode = dsi_enc_mode(d);
	if (dsi_hw_check_mode(&hw_cfg)) {
		dpy_err("%s: horizontal blanking too short for DSI sync packets\n",
			encoder->name);
		return -EINVAL;
	}

	memset(&phy_cfg, 0, sizeof(phy_cfg));
	phy_cfg.lanes = d->lanes;
	phy_cfg.pixclk_hz = cs->mode.clock * 1000;
	phy_cfg.bpp = dpy_dsi_format_bpp(d->format);
	if (combo_dphy_dsi_check(dsi->phy, &phy_cfg)) {
		dpy_err("%s: %u kHz, %u bpp on %u lane(s) not possible\n",
			encoder->name, cs->mode.clock, phy_cfg.bpp, d->lanes);
		return -EINVAL;
	}

	switch (d->format) {
	case DPY_DSI_FMT_RGB666:
	case DPY_DSI_FMT_RGB666_PACKED:
		cs->bus_format = DPY_BUS_FMT_RGB666_1X18;
		break;
	case DPY_DSI_FMT_RGB565:
		cs->bus_format = DPY_BUS_FMT_RGB565_1X16;
		break;
	default:
		cs->bus_format = DPY_BUS_FMT_RGB888_1X24;
		break;
	}
	cs->bus_flags = 0;
	return 0;
}

static int dsi_enc_power_on(struct dsi_enc *dsi)
{
	int ret;

	ret = dpy_os_reset_deassert(dsi->rst);
	if (ret)
		return ret;
	ret = dpy_os_clk_enable(dsi->bus_clk);
	if (ret)
		goto err_reset;
	dpy_dev_clk_setup(dsi->edev.dev, "mod", dsi->mod_clk);
	ret = dpy_os_clk_enable(dsi->mod_clk);
	if (ret)
		goto err_bus;
	dsi->powered = true;
	return 0;

err_bus:
	dpy_os_clk_disable(dsi->bus_clk);
err_reset:
	dpy_os_reset_assert(dsi->rst);
	return ret;
}

static void dsi_enc_power_off(struct dsi_enc *dsi)
{
	dsi->powered = false;
	dpy_os_clk_disable(dsi->mod_clk);
	dpy_os_clk_disable(dsi->bus_clk);
	dpy_os_reset_assert(dsi->rst);
}

static void dsi_enc_atomic_enable(struct dpy_encoder *encoder,
				  struct dpy_atomic_state *state)
{
	struct dsi_enc *dsi = to_dsi_enc(encoder);
	struct dpy_dsi_device *d = dsi->host.device;
	const struct dpy_crtc_state *cs = dpy_encoder_dev_crtc_state(&dsi->edev);
	struct combo_dphy_dsi_cfg phy_cfg;
	struct dpy_tcon_output cfg;
	struct dsi_hw_cfg hw_cfg;
	uint32_t bpp, pixclk;

	(void)state;
	if (!cs || !d)
		return;

	bpp = dpy_dsi_format_bpp(d->format);
	pixclk = cs->mode.clock * 1000;
	dsi->dsi_mode = dsi_enc_mode(d);

	dpy_pins_apply(&dsi->pdata->pins);
	if (dsi_enc_power_on(dsi))
		return;

	memset(&cfg, 0, sizeof(cfg));
	cfg.iface = DPY_TCON_IF_DSI;
	cfg.mode = cs->mode;
	cfg.dsi.mode = (uint8_t)dsi->dsi_mode;
	cfg.dsi.lanes = d->lanes;
	cfg.dsi.bpp = (uint8_t)bpp;
	cfg.dsi.dsi_id = dsi->edev.dev->node->desc->id;
	cfg.dsi.phy_id = combo_dphy_id(dsi->phy);
	if (dpy_timing_ctrl_prepare(dsi->edev.tc, &cfg))
		goto err_power;

	if (combo_dphy_power_on(dsi->phy, pixclk / d->lanes * bpp))
		goto err_tcon;

	memset(&hw_cfg, 0, sizeof(hw_cfg));
	hw_cfg.mode = cs->mode;
	hw_cfg.lanes = d->lanes;
	hw_cfg.format = d->format;
	hw_cfg.bpp = bpp;
	hw_cfg.dsi_mode = dsi->dsi_mode;
	hw_cfg.channel = d->channel;
	hw_cfg.mod_clk_hz = dpy_os_clk_get_rate(dsi->mod_clk);
	dsi_hw_config(&dsi->hw, &hw_cfg);

	memset(&phy_cfg, 0, sizeof(phy_cfg));
	phy_cfg.lanes = d->lanes;
	phy_cfg.pixclk_hz = pixclk;
	phy_cfg.bpp = bpp;
	phy_cfg.hs_trail = d->hs_trail;
	phy_cfg.clk_trail = d->clk_trail;
	if (combo_dphy_dsi_enable(dsi->phy, &phy_cfg, NULL))
		goto err_phy;

	/* clock lane in HS before talking to the panel */
	dsi_hw_run(&dsi->hw, DSI_SEQ_HS_CLOCK);
	dsi_hw_wait_idle(&dsi->hw, 1000);

	dpy_encoder_dev_sink_prepare(&dsi->edev);

	dsi->vsrc.priv = dsi;
	dsi->vsrc.set_enable = NULL;
	dsi->vsrc.get_line = NULL;
	dsi->vsrc.frame_start = NULL;
	if (dsi->dsi_mode == DPY_DSI_COMMAND) {
		dsi->vsrc.frame_start = dsi_enc_frame_start;
	} else {
		dsi->vsrc.set_enable = dsi_enc_vblank_enable;
		dsi->vsrc.get_line = dsi_enc_get_line;
	}
	dpy_timing_ctrl_set_vblank_source(dsi->edev.tc, &dsi->vsrc);

	dpy_timing_ctrl_enable(dsi->edev.tc);
	if (dsi->dsi_mode != DPY_DSI_COMMAND) {
		dsi_hw_run(&dsi->hw, DSI_SEQ_HS_DATA);
		dsi->video_running = true;
	}
	dpy_encoder_dev_sink_enable(&dsi->edev);
	return;

err_phy:
	combo_dphy_power_off(dsi->phy);
err_tcon:
	dpy_timing_ctrl_unprepare(dsi->edev.tc);
err_power:
	dsi_enc_power_off(dsi);
}

static void dsi_enc_atomic_disable(struct dpy_encoder *encoder,
				   struct dpy_atomic_state *state)
{
	struct dsi_enc *dsi = to_dsi_enc(encoder);

	(void)state;
	/* nothing to undo when the enable failed half way */
	if (!dsi->powered)
		return;
	dpy_encoder_dev_sink_disable(&dsi->edev);

	dsi_hw_irq_enable(&dsi->hw, DSI_HW_IRQ_VIDEO_VBLK, false);
	if (dsi->video_running) {
		/* finish the current frame and fall back to LP */
		dsi_hw_video_hold(&dsi->hw, true);
		dpy_os_msleep(30);
		dsi->video_running = false;
	}
	dpy_timing_ctrl_disable(dsi->edev.tc);
	dpy_timing_ctrl_set_vblank_source(dsi->edev.tc, NULL);

	/* exit commands still go out in LP */
	dpy_encoder_dev_sink_unprepare(&dsi->edev);

	combo_dphy_dsi_disable(dsi->phy);
	combo_dphy_power_off(dsi->phy);
	dsi_hw_disable(&dsi->hw);
	dpy_timing_ctrl_unprepare(dsi->edev.tc);
	dsi_enc_power_off(dsi);
	dpy_pins_release(&dsi->pdata->pins);
}

static const struct dpy_encoder_funcs dsi_enc_funcs = {
	.atomic_check = dsi_enc_atomic_check,
	.atomic_enable = dsi_enc_atomic_enable,
	.atomic_disable = dsi_enc_atomic_disable,
};

/* ------------------------------------------------------------------ */
/* Component                                                           */
/* ------------------------------------------------------------------ */
static int dsi_enc_bind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct dsi_enc *dsi = dev->priv;
	int ret;

	dsi->phy = combo_dphy_get(dev->node);
	if (!dsi->phy) {
		dpy_err("%s: no combo phy\n", dpy_dev_name(dev));
		return -ENODEV;
	}
	if (!dsi->host.device) {
		dpy_err("%s: no DSI device attached\n", dpy_dev_name(dev));
		return -ENODEV;
	}
	ret = dpy_encoder_dev_bind(&dsi->edev, ddev, &dsi_enc_funcs,
			      DPY_ENCODER_DSI, DPY_CONNECTOR_DSI);
	if (ret)
		return ret;
	ret = dpy_os_request_irq(dsi->irq, dsi_enc_irq_handler,
				 dpy_dev_name(dev), dsi);
	if (ret)
		dpy_encoder_dev_unbind(&dsi->edev);
	return ret;
}

static void dsi_enc_unbind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct dsi_enc *dsi = dev->priv;

	(void)ddev;
	dpy_os_free_irq(dsi->irq, dsi);
	dpy_encoder_dev_unbind(&dsi->edev);
}

static const struct dpy_component_ops dsi_enc_component_ops = {
	.bind = dsi_enc_bind,
	.unbind = dsi_enc_unbind,
};

static int dsi_enc_probe(struct dpy_dev *dev)
{
	struct dsi_enc *dsi;
	int ret;

	dsi = dpy_os_zalloc(sizeof(*dsi));
	if (!dsi)
		return -ENOMEM;
	dsi->edev.dev = dev;
	dsi->pdata = dpy_dev_pdata(dev);
	if (!dsi->pdata)
		dsi->pdata = &dsi_enc_default_pdata;
	dsi->hw.base = dpy_dev_ioremap(dev, "reg", NULL);
	dsi->irq = dpy_dev_irq(dev, "irq");
	dsi->mod_clk = dpy_dev_clk_get(dev, "mod");
	dsi->bus_clk = dpy_dev_clk_get(dev, "bus");
	dsi->rst = dpy_dev_reset_get(dev, "bus");
	dsi->xfer_lock = dpy_os_mutex_create();
	dpy_os_spin_init(&dsi->link_lock);
	if (!dsi->hw.base || dsi->irq < 0 || !dsi->xfer_lock) {
		ret = -ENODEV;
		goto err;
	}

	dsi->host.node = dev->node;
	dsi->host.ops = &dsi_enc_host_ops;
	dsi->host.priv = dsi;
	ret = dpy_dsi_host_register(&dsi->host);
	if (ret)
		goto err;

	dev->priv = dsi;
	return dpy_component_add(dev);

err:
	dpy_os_mutex_destroy(dsi->xfer_lock);
	dpy_os_clk_put(dsi->mod_clk);
	dpy_os_clk_put(dsi->bus_clk);
	dpy_os_reset_put(dsi->rst);
	dpy_os_free(dsi);
	return ret;
}

static void dsi_enc_remove(struct dpy_dev *dev)
{
	struct dsi_enc *dsi = dev->priv;

	dpy_component_del(dev);
	dpy_dsi_host_unregister(&dsi->host);
	dpy_os_mutex_destroy(dsi->xfer_lock);
	dpy_os_clk_put(dsi->mod_clk);
	dpy_os_clk_put(dsi->bus_clk);
	dpy_os_reset_put(dsi->rst);
	dpy_os_free(dsi);
	dev->priv = NULL;
}

static const struct dpy_match dsi_enc_match[] = {
	{ "allwinner,sunxi-mipi-dsi", NULL },
	{ NULL },
};

const struct dpy_driver dpy_dsi_driver = {
	.name = "mipi-dsi",
	.match = dsi_enc_match,
	.klass = DPY_COMP_ENCODER,
	.probe = dsi_enc_probe,
	.remove = dsi_enc_remove,
	.ops = &dsi_enc_component_ops,
};
