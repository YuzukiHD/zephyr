// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display engine top: module clocks and resets inside the engine, the
 * per-display global control (enable, size) and the RCQ control. These
 * registers are written directly, never through the RCQ.
 */
#define DPY_LOG_TAG "de-top"
#include "de_priv.h"

/* engine internal resets and gates */
#define DE_TOP_RESET			0x010
#define DE_TOP_AHB_GATE			0x014
#define DE_TOP_MBUS_GATE		0x018
#define DE_TOP_CLK_GATE			0x01c
#define   DE_TOP_CORE(disp)		DPY_BIT((disp) * 4)
#define   DE_TOP_WB			DPY_BIT(16)
#define DE_TOP_RTWB_CTL			0x020
#define   DE_TOP_RTWB_START		DPY_BIT(0)
#define   DE_TOP_RTWB_SELF_TIMING	DPY_BIT(4)

/* global control of display (mixer) @disp */
#define DE_GLB(disp)			(0x100 + (disp) * 0x40)
#define DE_GLB_CTL(d)			(DE_GLB(d) + 0x00)
#define   DE_GLB_CTL_RT_EN		DPY_BIT(0)
#define   DE_GLB_CTL_FINISH_IRQ_EN	DPY_BIT(4)
#define DE_GLB_STATUS(d)		(DE_GLB(d) + 0x04)
#define   DE_GLB_STATUS_BUSY		DPY_BIT(4)
#define DE_GLB_SIZE(d)			(DE_GLB(d) + 0x08)
#define   DE_GLB_SIZE_H			DPY_GENMASK(28, 16)
#define   DE_GLB_SIZE_W			DPY_GENMASK(12, 0)
#define DE_RCQ_CTL(d)			(DE_GLB(d) + 0x10)
#define   DE_RCQ_CTL_UPDATE		DPY_BIT(0)
#define   DE_RCQ_CTL_FINISH_IRQ_EN	DPY_BIT(16)
#define DE_RCQ_STATUS(d)		(DE_GLB(d) + 0x14)
#define   DE_RCQ_STATUS_FINISH		DPY_BIT(0)
#define DE_RCQ_HDR_LOW(d)		(DE_GLB(d) + 0x18)
#define DE_RCQ_HDR_HIGH(d)		(DE_GLB(d) + 0x1c)
#define DE_RCQ_HDR_LEN(d)		(DE_GLB(d) + 0x20)

int de_top_power_on(struct de_engine *de)
{
	int ret;

	if (de->power_users++)
		return 0;

	ret = dpy_os_reset_deassert(de->rst);
	if (!ret)
		ret = dpy_os_clk_enable(de->bus_clk);
	if (ret)
		goto err;
	dpy_dev_clk_setup(de->dev, "mod", de->mod_clk);
	ret = dpy_os_clk_enable(de->mod_clk);
	if (ret) {
		dpy_os_clk_disable(de->bus_clk);
		goto err;
	}
	return 0;

err:
	dpy_os_reset_assert(de->rst);
	de->power_users--;
	return ret;
}

void de_top_power_off(struct de_engine *de)
{
	if (!de->power_users || --de->power_users)
		return;
	dpy_os_clk_disable(de->mod_clk);
	dpy_os_clk_disable(de->bus_clk);
	dpy_os_reset_assert(de->rst);
}

void de_top_disp_enable(struct de_engine *de, uint8_t disp, uint32_t w,
			uint32_t h, bool on)
{
	uint32_t core = DE_TOP_CORE(disp);

	if (on) {
		/* release the mixer core, then open its bus and clock gates */
		de_update(de, DE_TOP_RESET, core, core);
		de_update(de, DE_TOP_AHB_GATE, core, core);
		de_update(de, DE_TOP_MBUS_GATE, core, core);
		de_update(de, DE_TOP_CLK_GATE, core, core);
		de_write(de, DE_GLB_SIZE(disp),
			 DPY_FIELD_PREP(DE_GLB_SIZE_W, w - 1) |
			 DPY_FIELD_PREP(DE_GLB_SIZE_H, h - 1));
		de_write(de, DE_GLB_CTL(disp), DE_GLB_CTL_RT_EN);
	} else {
		de_write(de, DE_GLB_CTL(disp), 0);
		de_update(de, DE_TOP_CLK_GATE, core, 0);
		de_update(de, DE_TOP_MBUS_GATE, core, 0);
		de_update(de, DE_TOP_AHB_GATE, core, 0);
		de_update(de, DE_TOP_RESET, core, 0);
	}
}

void de_top_rcq_setup(struct de_engine *de, uint8_t disp,
		      dpy_dma_addr_t hdrs, uint32_t bytes)
{
	de_write(de, DE_RCQ_HDR_LOW(disp), (uint32_t)hdrs);
	de_write(de, DE_RCQ_HDR_HIGH(disp),
		 (uint32_t)(((uint64_t)hdrs >> 32) & 0xff));
	de_write(de, DE_RCQ_HDR_LEN(disp), bytes);
}


void de_top_rcq_trigger(struct de_engine *de, uint8_t disp)
{
	/* clear the previous completion (write one), then request a load */
	de_write(de, DE_RCQ_STATUS(disp), DE_RCQ_STATUS_FINISH);
	de_update(de, DE_RCQ_CTL(disp), DE_RCQ_CTL_UPDATE, DE_RCQ_CTL_UPDATE);
}

bool de_top_rcq_finished(struct de_engine *de, uint8_t disp)
{
	if (!(de_read(de, DE_RCQ_STATUS(disp)) & DE_RCQ_STATUS_FINISH))
		return false;
	de_write(de, DE_RCQ_STATUS(disp), DE_RCQ_STATUS_FINISH);
	return true;
}

/*
 * The write-back block only has a reset and a bus gate (it runs on the
 * mixer clock). Captures follow the TCON timing of the display they tap,
 * the self timed RTWB mode is left off.
 */
void de_top_wb_enable(struct de_engine *de, bool on)
{
	de_update(de, DE_TOP_RESET, DE_TOP_WB, on ? DE_TOP_WB : 0);
	de_update(de, DE_TOP_AHB_GATE, DE_TOP_WB, on ? DE_TOP_WB : 0);
	if (on)
		de_write(de, DE_TOP_RTWB_CTL, 0);
}

void de_top_dump(struct de_engine *de, void (*print)(const char *fmt, ...))
{
	if (!de->power_users) {
		print("de: powered off\n");
		return;
	}
	print("de top: reset %08x ahb %08x mbus %08x clk %08x rtwb %08x\n",
	      de_read(de, DE_TOP_RESET), de_read(de, DE_TOP_AHB_GATE),
	      de_read(de, DE_TOP_MBUS_GATE), de_read(de, DE_TOP_CLK_GATE),
	      de_read(de, DE_TOP_RTWB_CTL));
	print("de glb0: ctl %08x status %08x size %08x rcq ctl %08x status %08x hdr %08x len %u\n",
	      de_read(de, DE_GLB_CTL(0)), de_read(de, DE_GLB_STATUS(0)),
	      de_read(de, DE_GLB_SIZE(0)), de_read(de, DE_RCQ_CTL(0)),
	      de_read(de, DE_RCQ_STATUS(0)), de_read(de, DE_RCQ_HDR_LOW(0)),
	      de_read(de, DE_RCQ_HDR_LEN(0)));
}
