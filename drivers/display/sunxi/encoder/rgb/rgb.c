// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Parallel RGB (HV) encoder.
 *
 * The TCON drives the pads directly; this encoder owns the pin
 * configuration, decides who dithers for panels with less than 8 bits per
 * component and sequences TCON and panel.
 */
#define DPY_LOG_TAG "rgb"
#include <dpy/dpy_log.h>

#include "encoder/encoder_common.h"

struct rgb_enc {
	struct dpy_encoder_dev edev;
	const struct dpy_rgb_pdata *pdata;
};

static const struct dpy_rgb_pdata rgb_enc_default_pdata = {
	.hv_mode = DPY_HV_PARALLEL_RGB,
};

#define to_rgb_enc(e) dpy_container_of(e, struct rgb_enc, edev.encoder)

static int rgb_enc_atomic_check(struct dpy_encoder *encoder,
				struct dpy_crtc_state *cs,
				struct dpy_connector_state *conn_state)
{
	struct rgb_enc *rgb = to_rgb_enc(encoder);
	const struct dpy_display_info *info = &rgb->edev.connector.info;

	(void)conn_state;
	cs->bus_format = info->bus_format ? info->bus_format
					  : DPY_BUS_FMT_RGB888_1X24;
	cs->bus_flags = info->bus_flags;
	/* the TCON FRM reduces the depth: let the DE send full 8 bit data */
	if (rgb->pdata->use_tcon_frm)
		cs->bus_format = DPY_BUS_FMT_RGB888_1X24;
	return 0;
}

static void rgb_enc_atomic_enable(struct dpy_encoder *encoder,
				  struct dpy_atomic_state *state)
{
	struct rgb_enc *rgb = to_rgb_enc(encoder);
	const struct dpy_rgb_pdata *pd = rgb->pdata;
	const struct dpy_display_info *info = &rgb->edev.connector.info;
	const struct dpy_crtc_state *cs = dpy_encoder_dev_crtc_state(&rgb->edev);
	struct dpy_tcon_output cfg;
	uint32_t bpc;

	(void)state;
	if (!cs)
		return;

	memset(&cfg, 0, sizeof(cfg));
	cfg.iface = DPY_TCON_IF_HV;
	cfg.mode = cs->mode;
	cfg.bus_flags = info->bus_flags;
	cfg.hv.hv_mode = pd->hv_mode;
	cfg.hv.srgb_seq = pd->srgb_seq;
	cfg.hv.syuv_seq = pd->syuv_seq;
	cfg.hv.syuv_fdly = pd->syuv_fdly;
	cfg.hv.rgb_swap = pd->rgb_swap;
	cfg.hv.rb_swap = pd->rb_swap;
	cfg.hv.clk_phase = pd->clk_phase;
	cfg.hv.io_adjust = pd->io_adjust;
	bpc = dpy_bus_format_bpc(info->bus_format);
	if (pd->use_tcon_frm && bpc < 8)
		cfg.hv.frm = bpc == 5 ? DPY_TCON_FRM_RGB565 : DPY_TCON_FRM_RGB666;

	dpy_pins_apply(&pd->pins);
	if (dpy_timing_ctrl_prepare(rgb->edev.tc, &cfg))
		return;
	dpy_encoder_dev_sink_prepare(&rgb->edev);
	dpy_timing_ctrl_enable(rgb->edev.tc);
	dpy_encoder_dev_sink_enable(&rgb->edev);
}

static void rgb_enc_atomic_disable(struct dpy_encoder *encoder,
				   struct dpy_atomic_state *state)
{
	struct rgb_enc *rgb = to_rgb_enc(encoder);

	(void)state;
	dpy_encoder_dev_sink_disable(&rgb->edev);
	dpy_timing_ctrl_disable(rgb->edev.tc);
	dpy_encoder_dev_sink_unprepare(&rgb->edev);
	dpy_timing_ctrl_unprepare(rgb->edev.tc);
	dpy_pins_release(&rgb->pdata->pins);
}

static const struct dpy_encoder_funcs rgb_enc_funcs = {
	.atomic_check = rgb_enc_atomic_check,
	.atomic_enable = rgb_enc_atomic_enable,
	.atomic_disable = rgb_enc_atomic_disable,
};

static int rgb_enc_bind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct rgb_enc *rgb = dev->priv;

	return dpy_encoder_dev_bind(&rgb->edev, ddev, &rgb_enc_funcs,
			       DPY_ENCODER_DPI, DPY_CONNECTOR_DPI);
}

static void rgb_enc_unbind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct rgb_enc *rgb = dev->priv;

	(void)ddev;
	dpy_encoder_dev_unbind(&rgb->edev);
}

static const struct dpy_component_ops rgb_enc_component_ops = {
	.bind = rgb_enc_bind,
	.unbind = rgb_enc_unbind,
};

static int rgb_enc_probe(struct dpy_dev *dev)
{
	struct rgb_enc *rgb;

	rgb = dpy_os_zalloc(sizeof(*rgb));
	if (!rgb)
		return -ENOMEM;
	rgb->edev.dev = dev;
	rgb->pdata = dpy_dev_pdata(dev);
	if (!rgb->pdata)
		rgb->pdata = &rgb_enc_default_pdata;
	dev->priv = rgb;
	return dpy_component_add(dev);
}

static void rgb_enc_remove(struct dpy_dev *dev)
{
	dpy_component_del(dev);
	dpy_os_free(dev->priv);
	dev->priv = NULL;
}

static const struct dpy_match rgb_enc_match[] = {
	{ "allwinner,sunxi-rgb", NULL },
	{ NULL },
};

const struct dpy_driver dpy_rgb_driver = {
	.name = "rgb",
	.match = rgb_enc_match,
	.klass = DPY_COMP_ENCODER,
	.probe = rgb_enc_probe,
	.remove = rgb_enc_remove,
	.ops = &rgb_enc_component_ops,
};
