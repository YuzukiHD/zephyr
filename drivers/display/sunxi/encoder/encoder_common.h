/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Shared encoder helpers.
 */
#ifndef __ENCODER_COMMON_H__
#define __ENCODER_COMMON_H__

#include <dpy/dpy_device.h>
#include <dpy/dpy_panel.h>
#include <dpy/dpy_pdata.h>
#include <dpy/dpy_tcon.h>

/*
 * An encoder component: input port 0 is linked to a timing controller,
 * output port 1 to a panel or bridge.
 */
struct dpy_encoder_dev {
	struct dpy_dev *dev;
	struct dpy_encoder encoder;
	struct dpy_connector connector;
	struct dpy_timing_ctrl *tc;
	struct dpy_sink sink;
};

int dpy_encoder_dev_bind(struct dpy_encoder_dev *edev, struct dpy_device *ddev,
		    const struct dpy_encoder_funcs *funcs,
		    enum dpy_encoder_type etype,
		    enum dpy_connector_type ctype);
void dpy_encoder_dev_unbind(struct dpy_encoder_dev *edev);

/* current (committed) crtc state of the crtc driving this encoder */
const struct dpy_crtc_state *dpy_encoder_dev_crtc_state(struct dpy_encoder_dev *edev);

int dpy_encoder_dev_sink_prepare(struct dpy_encoder_dev *edev);
int dpy_encoder_dev_sink_enable(struct dpy_encoder_dev *edev);
void dpy_encoder_dev_sink_disable(struct dpy_encoder_dev *edev);
void dpy_encoder_dev_sink_unprepare(struct dpy_encoder_dev *edev);

#endif /* __ENCODER_COMMON_H__ */
