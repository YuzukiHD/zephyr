// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * sun252iw2 (F101) display subsystem: the code that cannot be described in
 * the devicetree. The hardware graph itself (register windows, clocks,
 * resets, IRQs, links) lives in the devicetree, see dt_graph.c.
 */
#include <dpy/dpy_graph.h>
#include <dpy/dpy_os.h>
#include <dpy/dpy_pdata.h>

#include "../dt_graph.h"

#define SYS_CFG_BASE		0x03000000
#define SID_BASE		0x03006000

/*
 * The DSI/LVDS pad termination is trimmed per chip: a 4 bit code is fused
 * in SID word 0x18 bits [7:4] and has to be copied, together with an
 * unlock key, into the SYS_CFG resistor control register.
 */
static void sun252iw2_dphy_calibrate(void)
{
	uintptr_t sid = dpy_os_ioremap(SID_BASE, 0x100);
	uintptr_t syscfg = dpy_os_ioremap(SYS_CFG_BASE, 0x200);
	uint32_t code;

	code = dpy_readl(sid + 0x18);
	if (!code)
		return;
	code = (code >> 4) & 0xf;
	dpy_writel(code | (code << 8) | (0x1937U << 16), syscfg + 0x164);
}

const struct dpy_combo_dphy_soc_pdata dpy_sun252iw2_dphy_pdata = {
	.calibrate = sun252iw2_dphy_calibrate,
};

/*
 * The MBUS priority of the display engine is set by the mbus driver
 * (drivers/misc/mbus_sunxi) from the devicetree.
 *
 * SYS_CFG word 0x04 is programmed with 0x0b000000 before
 * the display engine is used (it most likely routes an SRAM area to the
 * display engine). Its meaning is not documented; the value is kept as is.
 */
static int sun252iw2_display_init(void)
{
	uintptr_t syscfg = dpy_os_ioremap(SYS_CFG_BASE, 0x200);

	dpy_writel(0x0b000000, syscfg + 0x04);
	return 0;
}

const struct dpy_soc_desc dpy_soc = {
	.name = "sun252iw2",
	.init = sun252iw2_display_init,
};
