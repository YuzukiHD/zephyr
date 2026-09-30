/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * sunxi MIPI DSI host controller, register level programming.
 */
#ifndef __DSI_HOST_H__
#define __DSI_HOST_H__

#include <dpy/dpy_dsi.h>
#include <dpy/dpy_tcon.h>

struct dsi_hw {
	uintptr_t base;
};

/* interrupt numbers for dsi_hw_irq_enable() / dsi_hw_irq_ack() */
#define DSI_HW_IRQ_VIDEO_VBLK	2

struct dsi_hw_cfg {
	struct dpy_display_mode mode;
	uint32_t lanes;
	uint32_t format;	/* enum dpy_dsi_format */
	uint32_t bpp;
	uint32_t dsi_mode;	/* enum dpy_dsi_mode */
	uint8_t channel;
	bool slave;
	uint32_t mod_clk_hz;	/* rate of the DSI module clock */
};

/* lane sequences run by the instruction engine */
enum dsi_seq {
	DSI_SEQ_LP11 = 0,	/* all lanes stop */
	DSI_SEQ_HS_CLOCK,	/* clock lane to HS */
	DSI_SEQ_HS_VIDEO,	/* clock + data lanes to HS, stream */
	DSI_SEQ_HS_DATA,	/* data lanes to HS, stream */
	DSI_SEQ_LP_TX,		/* escape mode LP transmit */
	DSI_SEQ_LP_RX,		/* LP transmit + bus turnaround */
};

/* -EINVAL when the horizontal blanking cannot hold the sync packets */
int dsi_hw_check_mode(const struct dsi_hw_cfg *cfg);
void dsi_hw_config(struct dsi_hw *hw, const struct dsi_hw_cfg *cfg);
void dsi_hw_disable(struct dsi_hw *hw);
void dsi_hw_run(struct dsi_hw *hw, enum dsi_seq seq);
int dsi_hw_wait_idle(struct dsi_hw *hw, uint32_t timeout_us);
/* pause/resume the video stream so LP commands can be interleaved */
void dsi_hw_video_hold(struct dsi_hw *hw, bool hold);
int dsi_hw_transfer(struct dsi_hw *hw, const struct dpy_dsi_msg *msg);

void dsi_hw_irq_enable(struct dsi_hw *hw, uint32_t irq, bool on);
/* returns and acknowledges the pending enabled interrupts (bit = irq) */
uint32_t dsi_hw_irq_ack(struct dsi_hw *hw);
uint32_t dsi_hw_get_line(struct dsi_hw *hw);
void dsi_hw_dump(struct dsi_hw *hw, void (*print)(const char *fmt, ...));

#endif /* __DSI_HOST_H__ */
