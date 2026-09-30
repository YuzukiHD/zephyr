/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display pipeline description generated from the devicetree (dt_graph.c).
 */
#ifndef __DPY_DT_GRAPH_H__
#define __DPY_DT_GRAPH_H__

#include <dpy/dpy_graph.h>
#include <dpy/dpy_pdata.h>

/* decode the devicetree command sequences; call before display_probe() */
int dpy_dt_prepare(void);

/* SoC data of the combo D-PHY nodes (soc/soc_sun252iw2.c) */
extern const struct dpy_combo_dphy_soc_pdata dpy_sun252iw2_dphy_pdata;

/* SoC description: one-time setup hook only, the graph comes from the DT */
extern const struct dpy_soc_desc dpy_soc;
/* board description generated from the devicetree */
extern const struct dpy_board_desc dpy_board;

#endif /* __DPY_DT_GRAPH_H__ */
