// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Shared encoder helpers: graph wiring of encoder,
 * connector, timing controller and sink, pin control and GPIOs.
 */
#define DPY_LOG_TAG "encoder"
#include <dpy/dpy_log.h>

#include "encoder/encoder_common.h"

static int dpy_encoder_dev_get_modes(struct dpy_connector *connector)
{
	struct dpy_encoder_dev *edev = connector->priv;

	if (edev->sink.bridge && edev->sink.bridge->funcs->get_modes)
		return edev->sink.bridge->funcs->get_modes(edev->sink.bridge,
							   connector->modes,
							   DPY_MAX_MODES);
	return dpy_panel_get_modes(edev->sink.panel, connector->modes,
				   DPY_MAX_MODES);
}

static const struct dpy_connector_funcs dpy_encoder_dev_connector_funcs = {
	.get_modes = dpy_encoder_dev_get_modes,
};

int dpy_encoder_dev_bind(struct dpy_encoder_dev *edev, struct dpy_device *ddev,
		    const struct dpy_encoder_funcs *funcs,
		    enum dpy_encoder_type etype,
		    enum dpy_connector_type ctype)
{
	struct dpy_gnode *node = edev->dev->node;
	struct dpy_gnode *tcon;
	int ret;

	tcon = dpy_node_remote(node, 0, DPY_EP_ANY, NULL, NULL);
	edev->tc = tcon ? dpy_timing_ctrl_find(tcon) : NULL;
	if (!edev->tc) {
		dpy_err("%s: input is not linked to a timing controller\n",
			dpy_dev_name(edev->dev));
		return -ENODEV;
	}

	ret = dpy_find_sink(node, 1, &edev->sink);
	if (ret) {
		dpy_err("%s: no panel or bridge behind the encoder\n",
			dpy_dev_name(edev->dev));
		return ret;
	}

	edev->encoder.node = node;
	edev->encoder.priv = edev;
	ret = dpy_encoder_init(ddev, &edev->encoder, funcs, etype,
			       dpy_dev_name(edev->dev));
	if (ret)
		return ret;
	edev->encoder.possible_crtcs =
		dpy_encoder_graph_possible_crtcs(ddev, node);
	if (!edev->encoder.possible_crtcs) {
		dpy_err("%s: not reachable from any crtc\n",
			dpy_dev_name(edev->dev));
		ret = -ENODEV;
		goto err_encoder;
	}

	if (edev->sink.bridge) {
		ret = dpy_bridge_attach(&edev->encoder, edev->sink.bridge, NULL);
		if (ret)
			goto err_encoder;
		edev->connector.info = edev->sink.bridge->info;
	} else {
		edev->connector.panel = edev->sink.panel;
		edev->connector.info = edev->sink.panel->info;
	}

	edev->connector.priv = edev;
	ret = dpy_connector_init(ddev, &edev->connector,
				 &dpy_encoder_dev_connector_funcs, ctype,
				 edev->sink.panel ? edev->sink.panel->name
						 : dpy_dev_name(edev->dev));
	if (ret)
		goto err_encoder;
	edev->connector.encoder = &edev->encoder;
	dpy_connector_update_modes(&edev->connector);
	return 0;

err_encoder:
	dpy_encoder_cleanup(&edev->encoder);
	return ret;
}

void dpy_encoder_dev_unbind(struct dpy_encoder_dev *edev)
{
	dpy_connector_cleanup(&edev->connector);
	dpy_encoder_cleanup(&edev->encoder);
	if (edev->sink.bridge)
		edev->sink.bridge->encoder = NULL;
}

const struct dpy_crtc_state *dpy_encoder_dev_crtc_state(struct dpy_encoder_dev *edev)
{
	return edev->encoder.crtc ? edev->encoder.crtc->state : NULL;
}

int dpy_encoder_dev_sink_prepare(struct dpy_encoder_dev *edev)
{
	if (edev->sink.bridge) {
		dpy_bridge_chain_pre_enable(edev->sink.bridge);
		return 0;
	}
	return dpy_panel_prepare(edev->sink.panel);
}

int dpy_encoder_dev_sink_enable(struct dpy_encoder_dev *edev)
{
	if (edev->sink.bridge) {
		dpy_bridge_chain_enable(edev->sink.bridge);
		return 0;
	}
	return dpy_panel_enable(edev->sink.panel);
}

void dpy_encoder_dev_sink_disable(struct dpy_encoder_dev *edev)
{
	if (edev->sink.bridge)
		dpy_bridge_chain_disable(edev->sink.bridge);
	else
		dpy_panel_disable(edev->sink.panel);
}

void dpy_encoder_dev_sink_unprepare(struct dpy_encoder_dev *edev)
{
	if (edev->sink.bridge)
		dpy_bridge_chain_post_disable(edev->sink.bridge);
	else
		dpy_panel_unprepare(edev->sink.panel);
}
