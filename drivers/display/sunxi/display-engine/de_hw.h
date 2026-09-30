/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display engine hardware description (per SoC).
 *
 * A display engine is described as a set of pipelines built from stages:
 *
 *   frontend (one per channel):  overlay -> [scaler] -> [channel CSC] --+
 *                                                                        |
 *   backend (one per display):   blender <-- inputs from the frontends --+
 *                                -> display CSC -> gamma -> dither -> TCON
 *
 * Each stage is one hardware module at a register offset of the engine.
 * The per-SoC file lists the instances and their order; the driver
 * instantiates the stage implementations and chains them.
 */
#ifndef __DE_HW_H__
#define __DE_HW_H__

#include <dpy/dpy_types.h>

enum de_stage_kind {
	DE_STAGE_OVL_VI = 0,	/* video overlay: 4 layers, RGB + YUV */
	DE_STAGE_OVL_UI,	/* UI overlay: 4 layers, RGB */
	DE_STAGE_VSU,		/* video scaler, separate luma/chroma */
	DE_STAGE_GSU,		/* graphic (RGB) scaler */
	DE_STAGE_CCSC,		/* channel colour space converter */
	DE_STAGE_BLD,		/* blender */
	DE_STAGE_DCSC,		/* display colour space converter / BCSH */
	DE_STAGE_GAMMA,		/* gamma LUT, colour matrix, CTC */
	DE_STAGE_DITHER,	/* output dither */
	DE_STAGE_KIND_NR,
};

struct de_stage_desc {
	const char *name;
	uint8_t kind;		/* enum de_stage_kind */
	uint32_t offset;	/* register offset from the engine base */
	uint32_t offset2;	/* secondary window (CCSC: input CSC), 0 if none */
	uint32_t param;		/* OVL: layers, scaler: line buffer width */
};

enum de_pipe_type {
	DE_PIPE_FRONTEND = 0,
	DE_PIPE_BACKEND,
};

struct de_pipe_desc {
	const char *name;
	uint8_t type;		/* enum de_pipe_type */
	uint8_t id;		/* frontend: blender port, backend: display */
	uint8_t disp;		/* display the frontend belongs to */
	const struct de_stage_desc *stages;
	uint8_t nstages;
	/* frontends only */
	const uint32_t *formats;
	uint8_t nformats;
	uint32_t max_width;
	uint32_t max_height;
};

struct de_soc_desc {
	const char *name;
	uint32_t core_clk_hz;	/* for the fetch bandwidth estimate */
	const struct de_pipe_desc *pipes;
	uint8_t npipes;
	uint32_t wb_offset;	/* 0: no write-back */
	uint32_t regs_arena;	/* bytes of DMA memory for shadow registers */
};

#endif /* __DE_HW_H__ */
