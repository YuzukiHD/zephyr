/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display engine driver private definitions.
 */
#ifndef __DE_PRIV_H__
#define __DE_PRIV_H__

#include <dpy/dpy_kms.h>
#include <dpy/dpy_log.h>
#include <dpy/dpy_pdata.h>
#include <dpy/dpy_tcon.h>

#include "de_hw.h"

#define DE_MAX_DISPS		1
#define DE_MAX_CHANNELS		4
#define DE_MAX_LAYERS		4
#define DE_MAX_PIPES		(DE_MAX_CHANNELS + DE_MAX_DISPS)
#define DE_PIPE_MAX_STAGES	6
#define DE_STAGE_MAX_BLKS	12
#define DE_MAX_REGBLKS		64

struct de_engine;
struct de_pipeline;
struct de_stage;
struct de_crtc_state;

/* ------------------------------------------------------------------ */
/* Shadow register blocks and the register command queue               */
/* ------------------------------------------------------------------ */
/*
 * Every stage keeps its registers in shadow blocks in DMA memory. A
 * commit marks the changed blocks in the RCQ header table and asks the
 * engine to fetch them at the next frame start; the whole configuration
 * therefore switches atomically on a frame boundary.
 */
struct de_rcq_hdr {
	uint32_t low_addr;	/* block address bits [31:0], 32 byte aligned */
	uint32_t dw0;		/* [23:0] length in bytes, [31:24] address bits [39:32] */
	uint32_t dirty;		/* bit 0: load this block */
	uint32_t reg_offset;	/* register offset from the engine base */
};

struct de_regblk {
	const char *name;
	uint32_t offset;	/* from the engine base */
	uint32_t size;		/* bytes */
	uint32_t *shadow;
	bool dirty;
	bool rcq;		/* updated through the RCQ (else direct) */
	uint8_t disp;		/* RCQ list the block belongs to */
	struct de_rcq_hdr *hdr;
};

struct de_regs {
	uint8_t *arena;
	uint32_t arena_size;
	uint32_t used;
	struct de_regblk blks[DE_MAX_REGBLKS];
	uint32_t nblks;
};

struct de_regblk *de_regblk_create(struct de_engine *de, const char *name,
				   uint32_t offset, uint32_t size, bool rcq,
				   uint8_t disp);

static inline void de_rb_write(struct de_regblk *b, uint32_t off, uint32_t val)
{
	uint32_t *p = &b->shadow[off / 4];

	if (*p != val) {
		*p = val;
		b->dirty = true;
	}
}

static inline uint32_t de_rb_read(const struct de_regblk *b, uint32_t off)
{
	return b->shadow[off / 4];
}

static inline void de_rb_update(struct de_regblk *b, uint32_t off,
				uint32_t mask, uint32_t val)
{
	de_rb_write(b, off, (de_rb_read(b, off) & ~mask) | (val & mask));
}

void de_regs_mark_all_dirty(struct de_engine *de, uint8_t disp);
/* write dirty (or all) shadow blocks of @disp straight to the registers */
void de_regs_flush_direct(struct de_engine *de, uint8_t disp, bool all);

struct de_rcq {
	struct de_rcq_hdr *hdrs;
	dpy_dma_addr_t hdrs_dma;
	struct de_regblk *blks[DE_MAX_REGBLKS];
	uint32_t nhdrs;
	bool pending;		/* waiting for the hardware to load */
	uint32_t wait_vblanks;
	uint32_t commits;
	uint32_t timeouts;
	uint32_t early_done;	/* flips completed by the frame start line IRQ */
};

int de_rcq_init(struct de_engine *de, uint8_t disp);
void de_rcq_exit(struct de_engine *de, uint8_t disp);
/* mark the dirty blocks in the headers; returns how many are dirty */
uint32_t de_rcq_prepare(struct de_engine *de, uint8_t disp);
/* wait for a safe point in the frame and start the update */
void de_rcq_abort(struct de_engine *de, uint8_t disp);
/* arms the crtc flip completion and requests the load */
void de_rcq_trigger(struct de_engine *de, uint8_t disp);
/* IRQ context: true when the triggered update has been loaded */
/*
 * @vblank: called from the vblank IRQ (counts towards the give-up limit),
 * otherwise from the early line IRQ (only looks at the completion flag).
 */
bool de_rcq_check_done(struct de_engine *de, uint8_t disp, bool vblank);

/* ------------------------------------------------------------------ */
/* Top (global) registers, accessed directly                            */
/* ------------------------------------------------------------------ */
int de_top_power_on(struct de_engine *de);
void de_top_power_off(struct de_engine *de);
void de_top_disp_enable(struct de_engine *de, uint8_t disp, uint32_t w,
			uint32_t h, bool on);
void de_top_rcq_setup(struct de_engine *de, uint8_t disp,
		      dpy_dma_addr_t hdrs, uint32_t bytes);
void de_top_rcq_trigger(struct de_engine *de, uint8_t disp);
bool de_top_rcq_finished(struct de_engine *de, uint8_t disp);
void de_top_wb_enable(struct de_engine *de, bool on);
void de_top_dump(struct de_engine *de, void (*print)(const char *fmt, ...));

/* ------------------------------------------------------------------ */
/* Stages and pipelines                                                */
/* ------------------------------------------------------------------ */
struct de_stage_ops {
	/* create register blocks and initial (disabled) content */
	int (*init)(struct de_stage *st);
	/* validate the plan for this stage, may refine it */
	int (*check)(struct de_stage *st, struct de_crtc_state *s);
	/* translate the plan into shadow register values */
	void (*apply)(struct de_stage *st, const struct de_crtc_state *s);
	void (*dump)(struct de_stage *st, void (*print)(const char *fmt, ...));
};

struct de_stage {
	const struct de_stage_desc *desc;
	const struct de_stage_ops *ops;
	struct de_engine *de;
	struct de_pipeline *pipe;
	struct de_regblk *blk[DE_STAGE_MAX_BLKS];
	uint8_t nblks;
	void *priv;
};

struct de_pipeline {
	const struct de_pipe_desc *desc;
	struct de_engine *de;
	uint8_t index;		/* frontends: channel index */
	struct de_stage stages[DE_PIPE_MAX_STAGES];
	uint8_t nstages;
	/* frontends: capabilities derived from the stages */
	uint8_t nlayers;
	bool has_scaler;
	bool is_video;
	uint32_t line_buffer;
};

extern const struct de_stage_ops *const de_stage_ops_table[DE_STAGE_KIND_NR];

int de_pipeline_build(struct de_engine *de);
struct de_stage *de_pipeline_find_stage(struct de_pipeline *pipe,
					enum de_stage_kind kind);
int de_pipeline_check(struct de_pipeline *pipe, struct de_crtc_state *s);
void de_pipeline_apply(struct de_pipeline *pipe, const struct de_crtc_state *s);

/* ------------------------------------------------------------------ */
/* Plans: what the frontends and the backend have to do                 */
/* ------------------------------------------------------------------ */
struct de_scale {
	uint32_t hstep;		/* input / output, fixed point (scaler) */
	uint32_t vstep;
	uint32_t hphase;
	uint32_t vphase;
};

enum de_sub {
	DE_SUB_RGB = 0,
	DE_SUB_422,
	DE_SUB_420,
	DE_SUB_411,
};

struct de_layer_plan {
	bool enable;
	uint32_t format;	/* enum display_format */
	uint8_t hw_fmt;		/* overlay format code */
	bool ui_sel;		/* video overlay fed with RGB data */
	uint8_t alpha_mode;	/* 0: pixel, 1: global, 2: pixel * global */
	uint8_t alpha_ctl;	/* pre-multiplication handling */
	struct dpy_rect crop;	/* integer source window in the framebuffer */
	struct dpy_rect ovl;	/* position/size inside the overlay window */
	struct dpy_rect frame;	/* destination on screen */
	dpy_dma_addr_t addr[3];	/* start of the crop window per plane */
	uint32_t pitch[3];
	uint8_t nplanes;
	struct de_scale ys;	/* per layer scaling (luma / rgb) */
	struct de_scale cs;	/* per layer scaling (chroma) */
};

struct de_chn_plan {
	bool enable;
	uint8_t nlayers;
	struct de_layer_plan layer[DE_MAX_LAYERS];
	uint8_t alpha;		/* shared layer alpha of the channel */
	uint8_t sub;		/* enum de_sub of the video data */
	bool yuv;
	uint8_t color_encoding;
	uint8_t color_range;
	uint32_t zmin, zmax;	/* normalized z range of the layers */

	/* overlay output and blender input */
	uint32_t ovl_w, ovl_h;
	struct dpy_rect bld;
	bool premul_out;	/* channel output is pre-multiplied */

	/* scaler */
	bool scale;
	struct de_scale ys, cs;
	uint32_t in_w, in_h;	/* scaler luma input after coarse decimation */
	uint32_t in_cw, in_ch;
	/* coarse decimation (video overlay), m/n pairs, 0: none */
	uint32_t yhm, yhn, yvm, yvn;
	uint32_t chm, chn, cvm, cvn;

	uint8_t pipe;		/* blender pipe index (0 = bottom) */
};

struct de_pipe_plan {
	bool enable;
	uint8_t port;		/* blender input (frontend) feeding the pipe */
	struct dpy_rect rect;
	bool premul;
};

struct de_disp_plan {
	uint32_t w, h;
	uint32_t background;	/* 0xAARRGGBB */
	uint8_t npipes;
	struct de_pipe_plan pipe[DE_MAX_CHANNELS];
	struct dpy_color_adjust adjust;
	bool gamma_enable;
	uint32_t gamma_seq;
	bool dither;
	uint8_t dither_fmt;
	uint8_t dither_mode;
};

struct de_crtc_state {
	struct dpy_crtc_state base;
	struct de_chn_plan chn[DE_MAX_CHANNELS];
	struct de_disp_plan disp;
	struct dpy_writeback_job *wb_job;
};

#define to_de_crtc_state(s) dpy_container_of(s, struct de_crtc_state, base)

/* dither output formats and algorithms */
#define DE_DITHER_FMT_888	0
#define DE_DITHER_FMT_444	1
#define DE_DITHER_FMT_565	2
#define DE_DITHER_FMT_666	3
#define DE_DITHER_QUANTIZE	0
#define DE_DITHER_FLOYD		1
#define DE_DITHER_ORDERED	3
#define DE_DITHER_SIERRA_LITE	4
#define DE_DITHER_BURKES	5

/* overlay pixel format code of @format (de_ovl.c) */
int de_ovl_format(uint32_t format, bool video, uint8_t *code, bool *ui_sel,
		  uint8_t *sub);
const char *de_stage_kind_str(uint8_t kind);

/* frontend planning (de_channel.c) */
int de_channel_plan(struct de_pipeline *pipe, struct de_crtc_state *s,
		    struct dpy_plane_state *const *layers,
		    const struct dpy_display_mode *mode);
/* backend planning (de_disp.c) */
int de_disp_plan(struct de_engine *de, struct de_crtc_state *s);

/* ------------------------------------------------------------------ */
/* KMS objects                                                         */
/* ------------------------------------------------------------------ */
struct de_crtc {
	struct dpy_crtc base;
	struct de_engine *de;
	struct de_pipeline *backend;
	uint8_t disp;
	bool hw_enabled;
	struct de_rcq rcq;
};

#define to_de_crtc(c) dpy_container_of(c, struct de_crtc, base)

struct de_plane {
	struct dpy_plane base;
	struct de_engine *de;
	struct de_pipeline *chn;
	uint8_t layer;
	char name[12];
};

#define to_de_plane(p) dpy_container_of(p, struct de_plane, base)

struct de_wb;

struct de_engine {
	struct dpy_dev *dev;
	const struct de_soc_desc *soc;
	const struct dpy_engine_pdata *board;
	uintptr_t base;
	struct dpy_clk *mod_clk;
	struct dpy_clk *bus_clk;
	struct dpy_reset *rst;
	uint32_t power_users;

	struct de_regs regs;
	struct de_pipeline pipes[DE_MAX_PIPES];
	uint8_t npipes;
	struct de_pipeline *frontend[DE_MAX_CHANNELS];
	uint8_t nfrontends;
	struct de_pipeline *backend[DE_MAX_DISPS];
	uint8_t nbackends;

	struct de_crtc crtc[DE_MAX_DISPS];
	struct de_plane planes[DE_MAX_CHANNELS * DE_MAX_LAYERS];
	uint8_t nplanes;
	struct de_plane *plane_map[DE_MAX_CHANNELS][DE_MAX_LAYERS];

	struct de_wb *wb;
	struct dpy_connector wb_connector;
};

int de_crtc_create(struct de_engine *de, struct dpy_device *ddev, uint8_t disp);
void de_crtc_destroy(struct de_engine *de, uint8_t disp);
int de_planes_create(struct de_engine *de, struct dpy_device *ddev);
void de_planes_destroy(struct de_engine *de);

/* write-back (de_wb.c) */
#ifdef CONFIG_DISPLAY_DE_WRITEBACK
int de_wb_create(struct de_engine *de, struct dpy_device *ddev);
void de_wb_destroy(struct de_engine *de);
/* run a capture job synchronously (called from the commit) */
int de_wb_run(struct de_engine *de, uint8_t disp,
	      const struct dpy_display_mode *mode,
	      struct dpy_writeback_job *job, struct dpy_crtc *crtc);
#else
static inline int de_wb_create(struct de_engine *de, struct dpy_device *ddev)
{
	(void)de;
	(void)ddev;
	return 0;
}
static inline void de_wb_destroy(struct de_engine *de) { (void)de; }
static inline int de_wb_run(struct de_engine *de, uint8_t disp,
			    const struct dpy_display_mode *mode,
			    struct dpy_writeback_job *job, struct dpy_crtc *crtc)
{
	(void)de;
	(void)disp;
	(void)mode;
	(void)crtc;
	job->status = -ENOTSUP;
	return -ENOTSUP;
}
#endif

static inline uint32_t de_read(struct de_engine *de, uint32_t off)
{
	return dpy_readl(de->base + off);
}

static inline void de_write(struct de_engine *de, uint32_t off, uint32_t val)
{
	dpy_writel(val, de->base + off);
}

static inline void de_update(struct de_engine *de, uint32_t off, uint32_t mask,
			     uint32_t val)
{
	dpy_updatel(de->base + off, mask, val);
}

#endif /* __DE_PRIV_H__ */
