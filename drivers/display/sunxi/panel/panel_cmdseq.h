/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Panel command sequence interpreter (power, reset, init commands).
 */
#ifndef __PANEL_CMDSEQ_H__
#define __PANEL_CMDSEQ_H__

#include <dpy/dpy_dsi.h>
#include <dpy/dpy_pdata.h>

struct dpy_cmd_ctx {
	struct dpy_dsi_device *dsi;		/* for DCS/generic commands */
	const struct dpy_spi_gpio *spi;		/* for SPI commands */
};

int dpy_cmdseq_run(const struct dpy_cmd_seq *seq, const struct dpy_cmd_ctx *ctx);
void dpy_spi_gpio_setup(const struct dpy_spi_gpio *spi);

#endif /* __PANEL_CMDSEQ_H__ */
