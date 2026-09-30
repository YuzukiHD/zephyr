// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * TCON LCD timing controller driver.
 *
 * Generates the output timing for parallel RGB, LVDS and MIPI DSI
 * interfaces, owns the TCON interrupt (vertical blank) and exposes all of
 * it through struct dpy_timing_ctrl. It only accesses TCON LCD registers
 * and, through tcon_top, the display interface top registers.
 */
#define DPY_LOG_TAG "tcon-lcd"
#include <dpy/dpy_log.h>

#include "tcon_priv.h"
#include "tcon_regs.h"

/*
 * Values that differ between TCON LCD blocks of different SoCs, selected
 * by the matched compatible.
 */
struct tcon_lcd_variant {
	uint32_t mod_rate_max;		/* highest module clock, Hz */
	uint8_t hv_div_min;		/* pixel clock divider range, HV */
	uint8_t hv_div_max;
	uint8_t lvds_div;		/* fixed divider: 7 bits per LVDS pair */
	uint8_t start_delay_min;	/* start delay: vblank lines - margin */
	uint8_t start_delay_margin;
	uint8_t flip_line_margin;	/* lines after the start delay line */
	uint8_t safe_period_mode;
	uint8_t safe_fifo_per_mhz;	/* FIFO entries held back per MHz */
	uint8_t lvds_ana_c;		/* second link pad driver trim */
	uint8_t lvds_ana_r;
};

/*
 * The display engine latches its registers when the TCON reaches the line
 * given by the start delay (measured: the completion flag rises at line 31
 * with a start delay of 31, and follows it when it is changed), so the line
 * interrupt for early flip completion sits flip_line_margin lines after it.
 */
static const struct tcon_lcd_variant tcon_lcd_variant_default = {
	.mod_rate_max = 600000000U,
	.hv_div_min = 6,
	.hv_div_max = 127,
	.lvds_div = 7,
	.start_delay_min = 10,
	.start_delay_margin = 8,
	.flip_line_margin = 12,
	.safe_period_mode = 3,
	.safe_fifo_per_mhz = 15,
	.lvds_ana_c = 4,
	.lvds_ana_r = 3,
};

struct tcon_lcd {
	struct dpy_dev *dev;
	const struct tcon_lcd_variant *var;
	struct dpy_timing_ctrl tc;
	struct tcon_top *top;
	uintptr_t base;
	int irq;
	uint32_t id;
	uint32_t de_port;
	struct dpy_clk *mod_clk;
	struct dpy_clk *bus_clk;
	struct dpy_reset *rst;
	bool powered;
	bool mod_on;			/* mod_clk enabled by us */
	uint32_t vblank_irq;		/* TCON_IRQ_VBLK or TCON_IRQ_CNTR */
	struct tcon_clk_plan clk;
	dpy_spinlock_t lock;		/* GINT0 read-modify-write */
	uint32_t start_delay;		/* TCON0_CTL start delay, in lines */
	uint32_t vblank_irqs;		/* statistics */
	uint32_t line_irqs;
};

#define to_tcon_lcd(t)	dpy_container_of(t, struct tcon_lcd, tc)

static inline uint32_t tcon_read(struct tcon_lcd *t, uint32_t reg)
{
	return dpy_readl(t->base + reg);
}

static inline void tcon_write(struct tcon_lcd *t, uint32_t reg, uint32_t val)
{
	dpy_writel(val, t->base + reg);
}

static inline void tcon_update(struct tcon_lcd *t, uint32_t reg, uint32_t mask,
			       uint32_t val)
{
	dpy_updatel(t->base + reg, mask, val);
}

/*
 * GINT0 interrupt flags are cleared by writing 0. Every write rewrites them
 * as 1 (no effect) except the ones in @clear, so updating the enable bits
 * cannot drop a flag raised between the read and the write. The FSYNC bit
 * is not a flag and keeps its value. Called with t->lock held.
 */
#define TCON_GINT0_W0C_FLAGS	(TCON_GINT0_FLAGS & \
				 ~TCON_GINT0_FLAG(TCON_IRQ_FSYNC))

static void tcon_gint0_update(struct tcon_lcd *t, uint32_t en_mask,
			      uint32_t en, uint32_t clear)
{
	uint32_t val = tcon_read(t, TCON_GINT0);

	val = (val & ~en_mask) | (en & en_mask);
	tcon_write(t, TCON_GINT0, (val | TCON_GINT0_W0C_FLAGS) & ~clear);
}

/* ------------------------------------------------------------------ */
/* Power                                                               */
/* ------------------------------------------------------------------ */
static int tcon_power_on(struct tcon_lcd *t)
{
	int ret;

	if (t->powered)
		return 0;
	ret = tcon_top_get(t->top);
	if (ret)
		return ret;
	ret = dpy_os_reset_deassert(t->rst);
	if (!ret)
		ret = dpy_os_clk_enable(t->bus_clk);
	if (ret) {
		tcon_top_put(t->top);
		return ret;
	}
	t->powered = true;
	return 0;
}

static void tcon_power_off(struct tcon_lcd *t)
{
	if (!t->powered)
		return;
	if (t->mod_on)
		dpy_os_clk_disable(t->mod_clk);
	t->mod_on = false;
	dpy_os_clk_disable(t->bus_clk);
	dpy_os_reset_assert(t->rst);
	tcon_top_put(t->top);
	t->powered = false;
}

static int tcon_setup_clock(struct tcon_lcd *t, uint32_t pixclk,
			    uint32_t min_div, uint32_t max_div)
{
	int ret;

	/* parent from the SoC description, rate from the plan */
	dpy_dev_clk_setup(t->dev, "mod", t->mod_clk);
	ret = tcon_clk_plan(t->mod_clk, pixclk, min_div, max_div,
			    t->var->mod_rate_max, &t->clk);
	if (ret)
		return ret;
	ret = dpy_os_clk_set_rate(t->mod_clk, t->clk.mod_rate);
	if (ret)
		dpy_warn("cannot set module clock to %u Hz\n", t->clk.mod_rate);
	t->clk.mod_rate = dpy_os_clk_get_rate(t->mod_clk);
	if (t->clk.mod_rate)
		t->clk.pixclk = DPY_DIV_ROUND_CLOSEST(t->clk.mod_rate,
						      t->clk.div);
	ret = dpy_os_clk_enable(t->mod_clk);
	if (ret)
		return ret;
	t->mod_on = true;
	tcon_update(t, TCON0_DCLK, TCON0_DCLK_DIV,
		    DPY_FIELD_PREP(TCON0_DCLK_DIV, t->clk.div));
	dpy_info("pixel clock %u Hz (module %u Hz / %u)\n", t->clk.pixclk,
		 t->clk.mod_rate, t->clk.div);
	return 0;
}

/*
 * DSI: the dot clock is generated by the combo phy PLL (routed by
 * tcon_top), the module clock only has to run for the register interface.
 */
static int tcon_setup_phy_clock(struct tcon_lcd *t, uint32_t pixclk)
{
	int ret;

	dpy_dev_clk_setup(t->dev, "mod", t->mod_clk);
	ret = dpy_os_clk_enable(t->mod_clk);
	if (ret)
		return ret;
	t->mod_on = true;
	t->clk.mod_rate = dpy_os_clk_get_rate(t->mod_clk);
	t->clk.div = 1;
	t->clk.pixclk = pixclk;
	tcon_update(t, TCON0_DCLK, TCON0_DCLK_DIV,
		    DPY_FIELD_PREP(TCON0_DCLK_DIV, 1));
	return 0;
}

/* ------------------------------------------------------------------ */
/* Register programming                                                */
/* ------------------------------------------------------------------ */
static void tcon_init_regs(struct tcon_lcd *t)
{
	tcon_update(t, TCON_GCTL, TCON_GCTL_IO_MAP_SEL | TCON_GCTL_PAD_SEL,
		    TCON_GCTL_PAD_SEL);
	tcon_update(t, TCON0_CTL, TCON0_CTL_EN, 0);
	tcon_update(t, TCON_GCTL, TCON_GCTL_EN, 0);
	tcon_write(t, TCON_GINT0, 0);
	tcon_update(t, TCON_GCTL, TCON_GCTL_EN, TCON_GCTL_EN);
}

static void tcon_set_timing(struct tcon_lcd *t, const struct dpy_display_mode *m)
{
	uint32_t hbp = m->htotal - m->hsync_start;	/* back porch + sync */
	uint32_t hspw = m->hsync_end - m->hsync_start;
	uint32_t vbp = m->vtotal - m->vsync_start;
	uint32_t vspw = m->vsync_end - m->vsync_start;

	tcon_write(t, TCON0_BASIC0,
		   DPY_FIELD_PREP(TCON0_BASIC0_X, m->hdisplay - 1) |
		   DPY_FIELD_PREP(TCON0_BASIC0_Y, m->vdisplay - 1));
	tcon_write(t, TCON0_BASIC1,
		   DPY_FIELD_PREP(TCON0_BASIC1_HT, m->htotal - 1) |
		   DPY_FIELD_PREP(TCON0_BASIC1_HBP, hbp ? hbp - 1 : 0));
	/* the vertical total is programmed in half lines */
	tcon_write(t, TCON0_BASIC2,
		   DPY_FIELD_PREP(TCON0_BASIC2_VT, m->vtotal * 2) |
		   DPY_FIELD_PREP(TCON0_BASIC2_VBP, vbp ? vbp - 1 : 0));
	tcon_write(t, TCON0_BASIC3,
		   DPY_FIELD_PREP(TCON0_BASIC3_HSPW, hspw ? hspw - 1 : 0) |
		   DPY_FIELD_PREP(TCON0_BASIC3_VSPW, vspw ? vspw - 1 : 0));
}

/* lines between the start of the vertical blank and the data request */
static void tcon_set_start_delay(struct tcon_lcd *t,
				 const struct dpy_display_mode *m)
{
	int32_t delay = (int32_t)m->vtotal - (int32_t)m->vdisplay -
			t->var->start_delay_margin;

	delay = DPY_CLAMP(delay, (int32_t)t->var->start_delay_min,
			  (int32_t)DPY_FIELD_GET(TCON0_CTL_START_DELAY,
						 TCON0_CTL_START_DELAY));
	t->start_delay = delay;
	tcon_update(t, TCON0_CTL, TCON0_CTL_START_DELAY,
		    DPY_FIELD_PREP(TCON0_CTL_START_DELAY, delay));
}

/*
 * Frame rate modulation: spread the dropped low bits over consecutive
 * frames for 6 bit (or 5/6/5 bit) panels.
 */
static void tcon_set_frm(struct tcon_lcd *t, uint8_t frm)
{
	static const uint32_t seeds[6] = { 1, 3, 5, 7, 11, 13 };
	static const uint32_t patterns[4] = {
		0x01010000, 0x15151111, 0x57575555, 0x7f7f7777,
	};
	unsigned int i;

	if (frm == DPY_TCON_FRM_OFF) {
		tcon_write(t, TCON0_FRM_CTL, 0);
		return;
	}
	for (i = 0; i < 6; i++)
		tcon_write(t, TCON0_FRM_SEED(i), seeds[i]);
	for (i = 0; i < 4; i++)
		tcon_write(t, TCON0_FRM_TBL(i), patterns[i]);
	tcon_write(t, TCON0_FRM_CTL, TCON0_FRM_EN |
		   (frm == DPY_TCON_FRM_RGB565 ?
		    TCON0_FRM_MODE_R | TCON0_FRM_MODE_B : 0));
}

static void tcon_set_io(struct tcon_lcd *t, const struct dpy_tcon_output *out,
			uint8_t clk_phase, uint8_t rgb_swap, uint8_t rb_swap,
			uint32_t io_adjust)
{
	static const struct {
		bool inv;
		uint8_t sel;
	} phase[4] = { { false, 0 }, { false, 2 }, { true, 0 }, { true, 2 } };
	const struct dpy_display_mode *m = &out->mode;
	uint32_t pol = 0;

	if (!(m->flags & DISPLAY_MODE_FLAG_NHSYNC))
		pol |= TCON0_IO_HSYNC_POSITIVE;
	if (!(m->flags & DISPLAY_MODE_FLAG_NVSYNC))
		pol |= TCON0_IO_VSYNC_POSITIVE;
	if (out->bus_flags & DPY_BUS_FLAG_DE_LOW)
		pol |= TCON0_IO_DE_INV;
	if (out->bus_flags & DPY_BUS_FLAG_DATA_INVERT)
		pol |= TCON0_IO_DATA_INV;
	if (!clk_phase && (out->bus_flags & DPY_BUS_FLAG_PIXDATA_NEGEDGE))
		clk_phase = 2;
	clk_phase &= 3;
	if (phase[clk_phase].inv)
		pol |= TCON0_IO_CLK_INV;
	pol |= DPY_FIELD_PREP(TCON0_IO_DCLK_SEL, phase[clk_phase].sel);
	tcon_write(t, TCON0_IO_POL, pol);

	/* every output driven, no tri-state */
	tcon_write(t, TCON0_IO_TRI, 0);
	tcon_write(t, TCON_IO_ADJ, io_adjust);

	tcon_update(t, TCON0_CTL, TCON0_CTL_RB_SWAP | TCON0_CTL_RGB_SWAP,
		    (rb_swap ? TCON0_CTL_RB_SWAP : 0) |
		    DPY_FIELD_PREP(TCON0_CTL_RGB_SWAP, rgb_swap));

	/* DE data requests are held back for this many FIFO entries */
	tcon_write(t, TCON_SAFE_PERIOD,
		   DPY_FIELD_PREP(TCON_SAFE_PERIOD_MODE,
				  t->var->safe_period_mode) |
		   DPY_FIELD_PREP(TCON_SAFE_PERIOD_FIFO_NUM,
				  DPY_MIN(m->clock / 1000 *
					  t->var->safe_fifo_per_mhz,
					  DPY_FIELD_GET(TCON_SAFE_PERIOD_FIFO_NUM,
							TCON_SAFE_PERIOD_FIFO_NUM))));
	tcon_write(t, TCON_FSYNC_GEN_CTRL, 0);
}

static void tcon_config_hv(struct tcon_lcd *t, const struct dpy_tcon_output *out)
{
	const struct dpy_tcon_hv_cfg *hv = &out->hv;

	tcon_update(t, TCON0_CTL, TCON0_CTL_IF,
		    DPY_FIELD_PREP(TCON0_CTL_IF, TCON0_IF_HV));
	tcon_write(t, TCON0_HV_CTL,
		   DPY_FIELD_PREP(TCON0_HV_MODE, hv->hv_mode) |
		   DPY_FIELD_PREP(TCON0_HV_SRGB_SEQ, hv->srgb_seq) |
		   DPY_FIELD_PREP(TCON0_HV_SYUV_SEQ, hv->syuv_seq) |
		   DPY_FIELD_PREP(TCON0_HV_SYUV_FDLY, hv->syuv_fdly));
	tcon_set_timing(t, &out->mode);
	tcon_set_start_delay(t, &out->mode);
	tcon_set_frm(t, hv->frm);
	tcon_set_io(t, out, hv->clk_phase, hv->rgb_swap, hv->rb_swap,
		    hv->io_adjust);
	t->vblank_irq = TCON_IRQ_VBLK;
}

static void tcon_config_lvds(struct tcon_lcd *t,
			     const struct dpy_tcon_output *out)
{
	const struct dpy_tcon_lvds_cfg *lvds = &out->lvds;
	uint32_t ctl = TCON0_LVDS_CLK_SEL;

	tcon_update(t, TCON0_CTL, TCON0_CTL_IF,
		    DPY_FIELD_PREP(TCON0_CTL_IF, TCON0_IF_HV));
	tcon_write(t, TCON0_HV_CTL, 0);
	if (lvds->dual_link)
		ctl |= TCON0_LVDS_LINK;
	if (lvds->jeida)
		ctl |= TCON0_LVDS_MODE;
	if (lvds->bpc6)
		ctl |= TCON0_LVDS_BITWIDTH;
	tcon_write(t, TCON0_LVDS_CTL, ctl);	/* enabled in tcon_enable */
	tcon_set_timing(t, &out->mode);
	tcon_set_start_delay(t, &out->mode);
	tcon_set_frm(t, lvds->frm);
	tcon_set_io(t, out, 0, 0, 0, 0);
	t->vblank_irq = TCON_IRQ_VBLK;
}

/*
 * DSI: the TCON runs in trigger ("CPU") mode and streams lines to the DSI
 * host. In video mode the host generates the frame timing and owns the
 * vertical blank interrupt; in command mode the TCON counter paces frames.
 */
static void tcon_config_dsi(struct tcon_lcd *t, const struct dpy_tcon_output *out)
{
	const struct dpy_display_mode *m = &out->mode;
	const struct dpy_tcon_dsi_cfg *dsi = &out->dsi;
	uint32_t vbp = m->vtotal - m->vsync_start;
	uint32_t vspw = m->vsync_end - m->vsync_start;
	uint32_t delay_lines, start_delay;
	bool cmd = dsi->mode == DPY_DSI_COMMAND;

	tcon_set_timing(t, m);
	tcon_update(t, TCON0_CTL, TCON0_CTL_START_DELAY,
		    DPY_FIELD_PREP(TCON0_CTL_START_DELAY, 0xf));
	tcon_update(t, TCON_SYNC_CTL, TCON_SYNC_DSI_NUM, 0);

	tcon_update(t, TCON0_CTL, TCON0_CTL_IF,
		    DPY_FIELD_PREP(TCON0_CTL_IF, (cmd || dsi->slave) ?
				   TCON0_IF_HV : TCON0_IF_CPU));

	if (!dsi->slave) {
		tcon_write(t, TCON0_CPU_CTL,
			   DPY_FIELD_PREP(TCON0_CPU_MODE, TCON0_CPU_MODE_DSI) |
			   TCON0_CPU_DA | TCON0_CPU_FLUSH |
			   TCON0_CPU_TRI_FIFO_EN | TCON0_CPU_TRI_EN);
		tcon_update(t, TCON_ECC_FIFO, TCON_ECC_FIFO_SETTING,
			    DPY_FIELD_PREP(TCON_ECC_FIFO_SETTING, 1U << 3));

		tcon_write(t, TCON0_CPU_TRI1,
			   DPY_FIELD_PREP(TCON0_TRI1_BLOCK_NUM,
					  m->vdisplay - 1));
		delay_lines = (vbp > vspw + 3) ? 3 : vbp;
		if (vbp <= vspw + 3)
			dpy_warn("vertical back porch too short for DSI\n");
		start_delay = delay_lines * m->htotal / 8 - 1;

		if (cmd) {
			tcon_write(t, TCON0_CPU_TRI0,
				   DPY_FIELD_PREP(TCON0_TRI0_BLOCK_SIZE,
						  m->hdisplay - 1) |
				   DPY_FIELD_PREP(TCON0_TRI0_BLOCK_SPACE, 200 - 1));
			tcon_write(t, TCON0_CPU_TRI2,
				   DPY_FIELD_PREP(TCON0_TRI2_START_DELAY, 4 - 1) |
				   DPY_FIELD_PREP(TCON0_TRI2_TRANS_START_SET,
						  10 - 1));
		} else {
			tcon_write(t, TCON0_CPU_TRI0,
				   DPY_FIELD_PREP(TCON0_TRI0_BLOCK_SIZE,
						  m->hdisplay - 1) |
				   DPY_FIELD_PREP(TCON0_TRI0_BLOCK_SPACE,
						  (m->htotal - m->hdisplay) / 2));
			tcon_write(t, TCON0_CPU_TRI2,
				   DPY_FIELD_PREP(TCON0_TRI2_START_DELAY,
						  start_delay) |
				   DPY_FIELD_PREP(TCON0_TRI2_TRANS_START_SET, 10));
		}

		if (cmd) {
			/* no TE: pace frames with the internal counter */
			uint32_t cntr = (uint32_t)m->htotal * m->vtotal / 4;
			uint32_t mdiv;

			for (mdiv = 1; mdiv < 256; mdiv++)
				if (cntr / mdiv < 65535)
					break;
			tcon_write(t, TCON0_CPU_TRI3,
				   DPY_FIELD_PREP(TCON0_TRI3_COUNTER_M, mdiv - 1) |
				   DPY_FIELD_PREP(TCON0_TRI3_COUNTER_N,
						  cntr / mdiv - 1) |
				   DPY_FIELD_PREP(TCON0_TRI3_INT_MODE, 1));
		}
	}

	tcon_set_frm(t, DPY_TCON_FRM_OFF);
	tcon_set_io(t, out, 0, 0, 0, 0);

	/* the pixel clock comes from the combo phy PLL */
	tcon_top_lcd_clk_from_phy(t->top, t->id, out->dsi.phy_id, true);
	tcon_top_dsi_route(t->top, out->dsi.dsi_id, t->id, true);

	t->vblank_irq = cmd ? TCON_IRQ_CNTR : TCON_IRQ_VBLK;
}

/* ------------------------------------------------------------------ */
/* Timing controller operations                                        */
/* ------------------------------------------------------------------ */
static int tcon_prepare(struct dpy_timing_ctrl *tc,
			const struct dpy_tcon_output *out)
{
	struct tcon_lcd *t = to_tcon_lcd(tc);
	uint32_t pixclk = out->mode.clock * 1000;
	int ret;

	ret = tcon_power_on(t);
	if (ret)
		return ret;

	tcon_init_regs(t);

	switch (out->iface) {
	case DPY_TCON_IF_HV:
		ret = tcon_setup_clock(t, pixclk, t->var->hv_div_min,
				       t->var->hv_div_max);
		if (!ret)
			tcon_config_hv(t, out);
		break;
	case DPY_TCON_IF_LVDS:
		/* seven bits per pixel clock on each LVDS pair */
		ret = tcon_setup_clock(t, pixclk, t->var->lvds_div,
				       t->var->lvds_div);
		if (!ret)
			tcon_config_lvds(t, out);
		break;
	case DPY_TCON_IF_DSI:
		ret = tcon_setup_phy_clock(t, pixclk);
		if (!ret)
			tcon_config_dsi(t, out);
		break;
	default:
		ret = -ENOTSUP;
		break;
	}
	if (ret) {
		tcon_power_off(t);
		return ret;
	}

	/* take the pixels from the display engine output this TCON is linked to */
	tcon_update(t, TCON0_CTL, TCON0_CTL_SRC_SEL,
		    DPY_FIELD_PREP(TCON0_CTL_SRC_SEL, TCON0_SRC_DE));
	tcon_top_route_de(t->top, t->de_port, t->id);
#ifdef CONFIG_DISPLAY_BOOT_COLORBAR
	tcon_update(t, TCON0_CTL, TCON0_CTL_SRC_SEL,
		    DPY_FIELD_PREP(TCON0_CTL_SRC_SEL, TCON0_SRC_COLORBAR));
#endif
	return 0;
}

static int tcon_enable(struct dpy_timing_ctrl *tc)
{
	struct tcon_lcd *t = to_tcon_lcd(tc);
	unsigned long flags;
	int i;

	tcon_top_lcd_to_pads(t->top, t->id);

	/* drop stale interrupt flags */
	flags = dpy_os_spin_lock_irqsave(&t->lock);
	tcon_gint0_update(t, 0, 0, TCON_GINT0_W0C_FLAGS);
	dpy_os_spin_unlock_irqrestore(&t->lock, flags);

	tcon_update(t, TCON0_DCLK, TCON0_DCLK_EN, TCON0_DCLK_EN);
	tcon_update(t, TCON0_CTL, TCON0_CTL_EN, TCON0_CTL_EN);

	if (tc->out.iface == DPY_TCON_IF_LVDS) {
		tcon_update(t, TCON0_LVDS_CTL, TCON0_LVDS_EN, TCON0_LVDS_EN);
		if (tc->out.lvds.dual_link) {
			/* dual link drives the second link from the TCON pads */
			for (i = 0; i < 2; i++)
				tcon_write(t, TCON0_LVDS_ANA(i),
					   DPY_FIELD_PREP(TCON0_LVDS_ANA_C,
							  t->var->lvds_ana_c) |
					   DPY_FIELD_PREP(TCON0_LVDS_ANA_R,
							  t->var->lvds_ana_r));
			dpy_os_udelay(5);
			for (i = 0; i < 2; i++)
				tcon_update(t, TCON0_LVDS_ANA(i),
					    TCON0_LVDS_ANA_EN_24M |
					    TCON0_LVDS_ANA_EN_LVDS |
					    TCON0_LVDS_ANA_EN_MB,
					    TCON0_LVDS_ANA_EN_24M |
					    TCON0_LVDS_ANA_EN_LVDS |
					    TCON0_LVDS_ANA_EN_MB);
			dpy_os_udelay(5);
			for (i = 0; i < 2; i++)
				tcon_update(t, TCON0_LVDS_ANA(i),
					    TCON0_LVDS_ANA_EN_DRVC |
					    TCON0_LVDS_ANA_EN_DRVD,
					    TCON0_LVDS_ANA_EN_DRVC |
					    DPY_FIELD_PREP(TCON0_LVDS_ANA_EN_DRVD,
							   tc->out.lvds.bpc6 ? 0x7 : 0xf));
		}
	}
	return 0;
}

static void tcon_disable(struct dpy_timing_ctrl *tc)
{
	struct tcon_lcd *t = to_tcon_lcd(tc);
	unsigned long flags;
	int i;

	flags = dpy_os_spin_lock_irqsave(&t->lock);
	tcon_gint0_update(t, TCON_GINT0_EN(TCON_IRQ_VBLK) |
			  TCON_GINT0_EN(TCON_IRQ_LINE) |
			  TCON_GINT0_EN(TCON_IRQ_CNTR) |
			  TCON_GINT0_EN(TCON_IRQ_TRIF), 0, 0);
	dpy_os_spin_unlock_irqrestore(&t->lock, flags);

	if (tc->out.iface == DPY_TCON_IF_LVDS) {
		for (i = 0; i < 2; i++)
			tcon_update(t, TCON0_LVDS_ANA(i),
				    TCON0_LVDS_ANA_EN_DRVC | TCON0_LVDS_ANA_EN_DRVD, 0);
		dpy_os_udelay(5);
		for (i = 0; i < 2; i++)
			tcon_update(t, TCON0_LVDS_ANA(i),
				    TCON0_LVDS_ANA_EN_MB | TCON0_LVDS_ANA_SRC_SEL, 0);
		tcon_update(t, TCON0_LVDS_CTL, TCON0_LVDS_EN, 0);
	}

	tcon_update(t, TCON0_CTL, TCON0_CTL_EN, 0);
	tcon_update(t, TCON0_DCLK, TCON0_DCLK_EN, 0);
	/* let the panel see a clean end of frame */
	dpy_os_msleep(20);
}

static void tcon_unprepare(struct dpy_timing_ctrl *tc)
{
	struct tcon_lcd *t = to_tcon_lcd(tc);

	/*
	 * The DSI routing stays up until here: panel exit commands are sent
	 * between disable and unprepare and need the DSI clocks.
	 */
	if (tc->out.iface == DPY_TCON_IF_DSI) {
		tcon_top_dsi_route(t->top, tc->out.dsi.dsi_id, t->id, false);
		tcon_top_lcd_clk_from_phy(t->top, t->id, tc->out.dsi.phy_id,
					  false);
	}
	tcon_update(t, TCON_GCTL, TCON_GCTL_EN, 0);
	tcon_update(t, TCON0_DCLK, TCON0_DCLK_EN, 0);
	tcon_update(t, TCON_GCTL, TCON_GCTL_PAD_SEL | TCON_GCTL_IO_MAP_SEL,
		    TCON_GCTL_IO_MAP_SEL);
	tcon_power_off(t);
}

/*
 * First line where the registers latched for the next frame are visible:
 * the display engine is started at the line given by the start delay.
 */
static uint32_t tcon_flip_line(const struct tcon_lcd *t,
			       const struct dpy_display_mode *m)
{
	uint32_t vtotal = m->vtotal;
	uint32_t line = t->start_delay + t->var->flip_line_margin;

	return line < vtotal ? line : vtotal - 1;
}

static int tcon_set_vblank(struct dpy_timing_ctrl *tc, bool on)
{
	struct tcon_lcd *t = to_tcon_lcd(tc);
	unsigned long flags;

	if (!t->powered)
		return on ? -EINVAL : 0;
	flags = dpy_os_spin_lock_irqsave(&t->lock);
	tcon_gint0_update(t, TCON_GINT0_EN(t->vblank_irq),
			  on ? TCON_GINT0_EN(t->vblank_irq) : 0, 0);
	/*
	 * The display engine latches its registers just before the active
	 * area, long before the vblank interrupt at its end. A line interrupt
	 * right after the latch lets a flip complete about a frame earlier;
	 * the vblank interrupt stays as the fallback.
	 */
	if (t->vblank_irq == TCON_IRQ_VBLK) {
		tcon_update(t, TCON_GINT1, TCON_GINT1_LINE_NUM,
			    DPY_FIELD_PREP(TCON_GINT1_LINE_NUM,
					   tcon_flip_line(t, &tc->out.mode)));
		tcon_gint0_update(t, TCON_GINT0_EN(TCON_IRQ_LINE),
				  on ? TCON_GINT0_EN(TCON_IRQ_LINE) : 0, 0);
	}
	dpy_os_spin_unlock_irqrestore(&t->lock, flags);
	return 0;
}

static uint32_t tcon_get_line(struct dpy_timing_ctrl *tc)
{
	struct tcon_lcd *t = to_tcon_lcd(tc);

	return DPY_FIELD_GET(TCON_DEBUG_TCON0_LINE, tcon_read(t, TCON_DEBUG));
}

static int tcon_set_pattern(struct dpy_timing_ctrl *tc, uint32_t pattern)
{
	static const uint8_t src[] = {
		[DPY_PATTERN_NONE] = TCON0_SRC_DE,
		[DPY_PATTERN_COLORBAR] = TCON0_SRC_COLORBAR,
		[DPY_PATTERN_GRAYSCALE] = TCON0_SRC_GRAYSCALE,
		[DPY_PATTERN_BLACK_WHITE] = TCON0_SRC_BLACK_WHITE,
		[DPY_PATTERN_BLACK] = TCON0_SRC_BLACK,
		[DPY_PATTERN_WHITE] = TCON0_SRC_WHITE,
		[DPY_PATTERN_GRID] = TCON0_SRC_GRID,
	};
	struct tcon_lcd *t = to_tcon_lcd(tc);

	if (pattern >= DPY_ARRAY_SIZE(src))
		return -EINVAL;
	if (!t->powered)
		return -EINVAL;
	tcon_update(t, TCON0_CTL, TCON0_CTL_SRC_SEL,
		    DPY_FIELD_PREP(TCON0_CTL_SRC_SEL, src[pattern]));
	return 0;
}

static bool tcon_check_underflow(struct dpy_timing_ctrl *tc)
{
	struct tcon_lcd *t = to_tcon_lcd(tc);

	if (!(tcon_read(t, TCON_DEBUG) & TCON_DEBUG_TCON0_UNDERFLOW))
		return false;
	tcon_update(t, TCON_DEBUG, TCON_DEBUG_TCON0_UNDERFLOW, 0);
	return true;
}

static uint32_t tcon_get_pixel_clock(struct dpy_timing_ctrl *tc)
{
	return to_tcon_lcd(tc)->clk.pixclk;
}

static void tcon_dump(struct dpy_timing_ctrl *tc,
		      void (*print)(const char *fmt, ...))
{
	static const uint16_t regs[] = {
		TCON_GCTL, TCON_GINT0, TCON0_FRM_CTL, TCON0_CTL, TCON0_DCLK,
		TCON0_BASIC0, TCON0_BASIC1, TCON0_BASIC2, TCON0_BASIC3,
		TCON0_HV_CTL, TCON0_CPU_CTL, TCON0_LVDS_CTL, TCON0_IO_POL,
		TCON0_IO_TRI, TCON_DEBUG, TCON0_CPU_TRI0, TCON0_CPU_TRI2,
		TCON_SAFE_PERIOD, TCON0_LVDS_ANA(0),
	};
	struct tcon_lcd *t = to_tcon_lcd(tc);
	unsigned int i;

	print("%s: %s, pixclk %u Hz (mod %u / %u), underflow %u, irqs vblank %u line %u\n",
	      tc->name, t->powered ? "on" : "off", t->clk.pixclk,
	      t->clk.mod_rate, t->clk.div, tc->underflow_count,
	      t->vblank_irqs, t->line_irqs);
	if (!t->powered)
		return;
	for (i = 0; i < DPY_ARRAY_SIZE(regs); i++)
		print("  +%03x: %08x\n", regs[i], tcon_read(t, regs[i]));
	tcon_top_dump(t->top, print);
}

static const struct dpy_timing_ctrl_ops tcon_lcd_ops = {
	.prepare = tcon_prepare,
	.enable = tcon_enable,
	.disable = tcon_disable,
	.unprepare = tcon_unprepare,
	.set_vblank = tcon_set_vblank,
	.get_line = tcon_get_line,
	.set_pattern = tcon_set_pattern,
	.check_underflow = tcon_check_underflow,
	.get_pixel_clock = tcon_get_pixel_clock,
	.dump = tcon_dump,
};

/* ------------------------------------------------------------------ */
/* Interrupt                                                           */
/* ------------------------------------------------------------------ */
static void tcon_irq_handler(void *data)
{
	struct tcon_lcd *t = data;
	uint32_t gint0, pending;
	unsigned long flags;

	flags = dpy_os_spin_lock_irqsave(&t->lock);
	gint0 = tcon_read(t, TCON_GINT0);
	pending = gint0 & (gint0 >> 16) &
		  (TCON_GINT0_FLAG(TCON_IRQ_VBLK) | TCON_GINT0_FLAG(TCON_IRQ_LINE) |
		   TCON_GINT0_FLAG(TCON_IRQ_CNTR) | TCON_GINT0_FLAG(TCON_IRQ_TRIF));
	if (pending)
		tcon_gint0_update(t, 0, 0, pending);
	dpy_os_spin_unlock_irqrestore(&t->lock, flags);

	/* trigger mode: kick the next frame (interface first, then TCON) */
	if (pending & TCON_GINT0_FLAG(TCON_IRQ_CNTR)) {
		const struct dpy_vblank_source *vsrc = t->tc.vsrc;

		if (!vsrc || !vsrc->frame_start ||
		    vsrc->frame_start(t->tc.vsrc))
			tcon_update(t, TCON0_CPU_CTL, TCON0_CPU_TRI_START,
				    TCON0_CPU_TRI_START);
	}

	if (pending & TCON_GINT0_FLAG(TCON_IRQ_LINE)) {
		t->line_irqs++;
		dpy_timing_ctrl_line_irq(&t->tc);
	}
	if (pending & TCON_GINT0_FLAG(t->vblank_irq)) {
		t->vblank_irqs++;
		dpy_timing_ctrl_vblank(&t->tc);
	}
}

/* ------------------------------------------------------------------ */
/* Component                                                           */
/* ------------------------------------------------------------------ */
static int tcon_lcd_bind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct tcon_lcd *t = dev->priv;
	struct dpy_gnode *top_node, *engine;
	uint8_t port = 0;
	int ret;

	top_node = dpy_node_get_ref(dev->node, "top");
	t->top = top_node ? tcon_top_from_node(top_node) : NULL;
	if (!t->top) {
		dpy_err("%s: no tcon top\n", dpy_dev_name(dev));
		return -ENODEV;
	}

	engine = dpy_node_remote(dev->node, 0, DPY_EP_ANY, &port, NULL);
	if (!engine)
		return -ENODEV;
	t->de_port = port;

	ret = dpy_timing_ctrl_attach(&t->tc, ddev);
	if (ret)
		return ret;

	ret = dpy_os_request_irq(t->irq, tcon_irq_handler, dpy_dev_name(dev), t);
	if (ret) {
		dpy_timing_ctrl_detach(&t->tc);
		return ret;
	}
	return 0;
}

static void tcon_lcd_unbind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct tcon_lcd *t = dev->priv;

	(void)ddev;
	dpy_os_free_irq(t->irq, t);
	dpy_timing_ctrl_detach(&t->tc);
}

static const struct dpy_component_ops tcon_lcd_component_ops = {
	.bind = tcon_lcd_bind,
	.unbind = tcon_lcd_unbind,
};

static int tcon_lcd_probe(struct dpy_dev *dev)
{
	struct tcon_lcd *t;
	int ret;

	t = dpy_os_zalloc(sizeof(*t));
	if (!t)
		return -ENOMEM;
	t->dev = dev;
	t->var = dpy_dev_match_data(dev);
	t->id = dev->node->desc->id;
	t->base = dpy_dev_ioremap(dev, "reg", NULL);
	t->irq = dpy_dev_irq(dev, "irq");
	t->mod_clk = dpy_dev_clk_get(dev, "mod");
	t->bus_clk = dpy_dev_clk_get(dev, "bus");
	t->rst = dpy_dev_reset_get(dev, "bus");
	t->vblank_irq = TCON_IRQ_VBLK;
	dpy_os_spin_init(&t->lock);
	if (!t->base || t->irq < 0 || !t->mod_clk || !t->var) {
		ret = -ENODEV;
		goto err;
	}

	t->tc.name = dpy_dev_name(dev);
	t->tc.node = dev->node;
	t->tc.ops = &tcon_lcd_ops;
	t->tc.priv = t;
	ret = dpy_timing_ctrl_register(&t->tc);
	if (ret)
		goto err;

	dev->priv = t;
	return dpy_component_add(dev);

err:
	dpy_os_clk_put(t->mod_clk);
	dpy_os_clk_put(t->bus_clk);
	dpy_os_reset_put(t->rst);
	dpy_os_free(t);
	return ret;
}

static void tcon_lcd_remove(struct dpy_dev *dev)
{
	struct tcon_lcd *t = dev->priv;

	dpy_component_del(dev);
	dpy_timing_ctrl_unregister(&t->tc);
	dpy_os_clk_put(t->mod_clk);
	dpy_os_clk_put(t->bus_clk);
	dpy_os_reset_put(t->rst);
	dpy_os_free(t);
	dev->priv = NULL;
}

static const struct dpy_match tcon_lcd_match[] = {
	{ "allwinner,sunxi-tcon-lcd", &tcon_lcd_variant_default },
	{ NULL },
};

const struct dpy_driver dpy_tcon_lcd_driver = {
	.name = "tcon-lcd",
	.match = tcon_lcd_match,
	.klass = DPY_COMP_TCON,
	.probe = tcon_lcd_probe,
	.remove = tcon_lcd_remove,
	.ops = &tcon_lcd_component_ops,
};
