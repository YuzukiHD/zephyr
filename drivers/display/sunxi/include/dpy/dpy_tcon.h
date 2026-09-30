/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display pipeline framework - timing controller contract.
 *
 * The display engine (CRTC) and the timing controller (TCON) are separate
 * drivers that never touch each other's registers. They cooperate only
 * through this interface:
 *
 *  - the TCON registers a struct dpy_timing_ctrl at probe time;
 *  - when the TCON component binds it follows its input port in the
 *    graph to the display engine output and attaches itself to the CRTC
 *    created for that output (crtc->tc);
 *  - encoders reach the same TCON through their input port and program
 *    the output timing/interface via prepare()/enable();
 *  - on every vertical blank the vblank source (the TCON itself, or an
 *    interface that owns the vblank interrupt such as DSI in video mode)
 *    calls dpy_timing_ctrl_vblank(), which forwards to the CRTC.
 */
#ifndef __DPY_TCON_H__
#define __DPY_TCON_H__

#include <dpy/dpy_kms.h>

enum dpy_tcon_if {
	DPY_TCON_IF_HV = 0,	/* parallel/serial RGB, CCIR656 */
	DPY_TCON_IF_LVDS,
	DPY_TCON_IF_DSI,
	DPY_TCON_IF_CPU,	/* i8080 */
};

enum dpy_hv_mode {
	DPY_HV_PARALLEL_RGB = 0x0,
	DPY_HV_SERIAL_RGB = 0x8,
	DPY_HV_SERIAL_RGB_DUMMY = 0xa,
	DPY_HV_SERIAL_YUV = 0xc,
	DPY_HV_CCIR656_2CYC = 0xe,
};

enum dpy_tcon_frm {
	DPY_TCON_FRM_OFF = 0,
	DPY_TCON_FRM_RGB666,
	DPY_TCON_FRM_RGB565,
};

enum dpy_dsi_mode {
	DPY_DSI_VIDEO_SYNC_PULSE = 0,
	DPY_DSI_VIDEO_BURST,
	DPY_DSI_COMMAND,
};

struct dpy_tcon_hv_cfg {
	uint8_t hv_mode;	/* enum dpy_hv_mode */
	uint8_t srgb_seq;	/* serial RGB component order */
	uint8_t syuv_seq;
	uint8_t syuv_fdly;
	uint8_t rgb_swap;	/* output component permutation */
	uint8_t rb_swap;
	uint8_t clk_phase;	/* 0..3: dclk delay/invert selection */
	uint8_t frm;		/* enum dpy_tcon_frm, TCON dithering */
	uint32_t io_adjust;
};

struct dpy_tcon_lvds_cfg {
	bool dual_link;
	bool jeida;		/* JEIDA bit mapping instead of VESA/NS */
	bool bpc6;		/* 6 bits per component (3 data pairs) */
	uint8_t frm;
};

struct dpy_tcon_dsi_cfg {
	uint8_t mode;		/* enum dpy_dsi_mode */
	uint8_t lanes;
	uint8_t bpp;		/* bits per pixel on the link */
	bool slave;
	uint8_t dsi_id;		/* DSI host instance driving this TCON */
	uint8_t phy_id;		/* combo phy instance clocking it */
};

struct dpy_tcon_output {
	enum dpy_tcon_if iface;
	struct dpy_display_mode mode;
	uint32_t bus_flags;
	union {
		struct dpy_tcon_hv_cfg hv;
		struct dpy_tcon_lvds_cfg lvds;
		struct dpy_tcon_dsi_cfg dsi;
	};
};

enum dpy_tcon_pattern {
	DPY_PATTERN_NONE = 0,
	DPY_PATTERN_COLORBAR,
	DPY_PATTERN_GRAYSCALE,
	DPY_PATTERN_BLACK_WHITE,
	DPY_PATTERN_BLACK,
	DPY_PATTERN_WHITE,
	DPY_PATTERN_GRID,
};

struct dpy_timing_ctrl;

/*
 * Hooks of the interface driving the TCON output:
 *  - an alternative vblank source (e.g. DSI host in video mode owns the
 *    vertical blank interrupt and the line counter);
 *  - frame_start: called from the TCON interrupt before a new frame is
 *    triggered in trigger (command) mode; returns false to skip the
 *    frame (e.g. the link is busy with a command transfer).
 * Every hook is optional.
 */
struct dpy_vblank_source {
	int (*set_enable)(struct dpy_vblank_source *src, bool on);
	uint32_t (*get_line)(struct dpy_vblank_source *src);
	bool (*frame_start)(struct dpy_vblank_source *src);
	void *priv;
};

struct dpy_timing_ctrl_ops {
	/* clocks on, timing and interface programmed, output still stopped */
	int (*prepare)(struct dpy_timing_ctrl *tc,
		       const struct dpy_tcon_output *out);
	/* start pushing pixels to the interface */
	int (*enable)(struct dpy_timing_ctrl *tc);
	void (*disable)(struct dpy_timing_ctrl *tc);
	void (*unprepare)(struct dpy_timing_ctrl *tc);
	int (*set_vblank)(struct dpy_timing_ctrl *tc, bool on);
	uint32_t (*get_line)(struct dpy_timing_ctrl *tc);
	int (*set_pattern)(struct dpy_timing_ctrl *tc, uint32_t pattern);
	/* returns and clears the FIFO underflow indication */
	bool (*check_underflow)(struct dpy_timing_ctrl *tc);
	/* pixel clock the hardware actually produces, Hz */
	uint32_t (*get_pixel_clock)(struct dpy_timing_ctrl *tc);
	void (*dump)(struct dpy_timing_ctrl *tc,
		     void (*print)(const char *fmt, ...));
};

struct dpy_timing_ctrl {
	struct dpy_list head;
	const char *name;
	const struct dpy_gnode *node;
	const struct dpy_timing_ctrl_ops *ops;
	void *priv;

	struct dpy_crtc *crtc;			/* attached CRTC */
	struct dpy_vblank_source *vsrc;		/* NULL: the TCON itself */
	bool prepared;
	bool enabled;
	struct dpy_tcon_output out;		/* last prepared output */
	uint32_t underflow_count;
};

int dpy_timing_ctrl_register(struct dpy_timing_ctrl *tc);
void dpy_timing_ctrl_unregister(struct dpy_timing_ctrl *tc);
struct dpy_timing_ctrl *dpy_timing_ctrl_find(const struct dpy_gnode *node);

/* attach @tc to the CRTC driving its input port (called from TCON bind) */
int dpy_timing_ctrl_attach(struct dpy_timing_ctrl *tc,
			   struct dpy_device *ddev);
void dpy_timing_ctrl_detach(struct dpy_timing_ctrl *tc);

void dpy_timing_ctrl_set_vblank_source(struct dpy_timing_ctrl *tc,
				       struct dpy_vblank_source *src);
/* vblank IRQ entry point for whatever owns the vblank interrupt */
void dpy_timing_ctrl_vblank(struct dpy_timing_ctrl *tc);
/* early-in-the-frame line IRQ entry point, forwards to the CRTC */
void dpy_timing_ctrl_line_irq(struct dpy_timing_ctrl *tc);

/* wrappers used by the CRTC side */
int dpy_timing_ctrl_enable_vblank(struct dpy_timing_ctrl *tc, bool on);
uint32_t dpy_timing_ctrl_get_line(struct dpy_timing_ctrl *tc);
/*
 * True while the scan position is inside the active area far enough from
 * the vertical blank for a shadow register update to be safe.
 */
bool dpy_timing_ctrl_in_safe_window(struct dpy_timing_ctrl *tc);

/* encoder helpers */
int dpy_timing_ctrl_prepare(struct dpy_timing_ctrl *tc,
			    const struct dpy_tcon_output *out);
int dpy_timing_ctrl_enable(struct dpy_timing_ctrl *tc);
void dpy_timing_ctrl_disable(struct dpy_timing_ctrl *tc);
void dpy_timing_ctrl_unprepare(struct dpy_timing_ctrl *tc);

#endif /* __DPY_TCON_H__ */
