// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * sun252iw2 display engine (DE 2.1 class): one display, three channels.
 *
 * Register map (offsets from the engine base 0x05000000):
 *   channel slots at 0x100000 + slot * 0x20000
 *     slot 0: video channel   CSC 0x100, input CSC 0x200, overlay 0x1000,
 *                             video scaler 0x4000
 *     slot 3: UI channel 0    overlay 0x1000, graphic scaler 0x4000
 *     slot 4: UI channel 1    overlay 0x1000 (no scaler)
 *   display 0 at 0x1c0000:    display CSC 0x100, blender 0x1000,
 *                             dither 0x8000, gamma 0x9000
 *   write-back at 0x010000
 */
#include "../de_priv.h"
#include "../de_soc.h"

#define DE_CHN(slot)		(0x100000 + (slot) * 0x20000)
#define DE_DISP0		0x1c0000

static const uint32_t de_rgb_formats[] = {
	DISPLAY_FORMAT_ARGB8888, DISPLAY_FORMAT_ABGR8888,
	DISPLAY_FORMAT_RGBA8888, DISPLAY_FORMAT_BGRA8888,
	DISPLAY_FORMAT_XRGB8888, DISPLAY_FORMAT_XBGR8888,
	DISPLAY_FORMAT_RGBX8888, DISPLAY_FORMAT_BGRX8888,
	DISPLAY_FORMAT_RGB888, DISPLAY_FORMAT_BGR888,
	DISPLAY_FORMAT_RGB565, DISPLAY_FORMAT_BGR565,
	DISPLAY_FORMAT_ARGB4444, DISPLAY_FORMAT_ABGR4444,
	DISPLAY_FORMAT_RGBA4444, DISPLAY_FORMAT_BGRA4444,
	DISPLAY_FORMAT_ARGB1555, DISPLAY_FORMAT_ABGR1555,
	DISPLAY_FORMAT_RGBA5551, DISPLAY_FORMAT_BGRA5551,
};

static const uint32_t de_video_formats[] = {
	DISPLAY_FORMAT_ARGB8888, DISPLAY_FORMAT_ABGR8888,
	DISPLAY_FORMAT_RGBA8888, DISPLAY_FORMAT_BGRA8888,
	DISPLAY_FORMAT_XRGB8888, DISPLAY_FORMAT_XBGR8888,
	DISPLAY_FORMAT_RGBX8888, DISPLAY_FORMAT_BGRX8888,
	DISPLAY_FORMAT_RGB888, DISPLAY_FORMAT_BGR888,
	DISPLAY_FORMAT_RGB565, DISPLAY_FORMAT_BGR565,
	DISPLAY_FORMAT_ARGB4444, DISPLAY_FORMAT_ABGR4444,
	DISPLAY_FORMAT_RGBA4444, DISPLAY_FORMAT_BGRA4444,
	DISPLAY_FORMAT_ARGB1555, DISPLAY_FORMAT_ABGR1555,
	DISPLAY_FORMAT_RGBA5551, DISPLAY_FORMAT_BGRA5551,
	DISPLAY_FORMAT_YUYV, DISPLAY_FORMAT_YVYU,
	DISPLAY_FORMAT_UYVY, DISPLAY_FORMAT_VYUY,
	DISPLAY_FORMAT_NV12, DISPLAY_FORMAT_NV21,
	DISPLAY_FORMAT_NV16, DISPLAY_FORMAT_NV61,
	DISPLAY_FORMAT_NV411, DISPLAY_FORMAT_NV114,
	DISPLAY_FORMAT_YUV420, DISPLAY_FORMAT_YVU420,
	DISPLAY_FORMAT_YUV422, DISPLAY_FORMAT_YUV411,
};

static const struct de_stage_desc de_vi0_stages[] = {
	{ "vi0-ovl", DE_STAGE_OVL_VI, DE_CHN(0) + 0x1000, 0, 4 },
	{ "vi0-vsu", DE_STAGE_VSU, DE_CHN(0) + 0x4000, 0, 2048 },
	{ "vi0-csc", DE_STAGE_CCSC, DE_CHN(0) + 0x100, DE_CHN(0) + 0x200, 0 },
};

static const struct de_stage_desc de_ui0_stages[] = {
	{ "ui0-ovl", DE_STAGE_OVL_UI, DE_CHN(3) + 0x1000, 0, 4 },
	{ "ui0-gsu", DE_STAGE_GSU, DE_CHN(3) + 0x4000, 0, 2048 },
};

static const struct de_stage_desc de_ui1_stages[] = {
	{ "ui1-ovl", DE_STAGE_OVL_UI, DE_CHN(4) + 0x1000, 0, 4 },
};

static const struct de_stage_desc de_disp0_stages[] = {
	{ "disp0-bld", DE_STAGE_BLD, DE_DISP0 + 0x1000, 0, 3 },
	{ "disp0-csc", DE_STAGE_DCSC, DE_DISP0 + 0x100, 0, 0 },
	{ "disp0-gamma", DE_STAGE_GAMMA, DE_DISP0 + 0x9000, 0, 0 },
	{ "disp0-dither", DE_STAGE_DITHER, DE_DISP0 + 0x8000, 0, 0 },
};

static const struct de_pipe_desc de_sun252iw2_pipes[] = {
	{
		.name = "vi0",
		.type = DE_PIPE_FRONTEND,
		.id = 0,
		.disp = 0,
		.stages = de_vi0_stages,
		.nstages = DPY_ARRAY_SIZE(de_vi0_stages),
		.formats = de_video_formats,
		.nformats = DPY_ARRAY_SIZE(de_video_formats),
		.max_width = 4096,
		.max_height = 4096,
	},
	{
		.name = "ui0",
		.type = DE_PIPE_FRONTEND,
		.id = 1,
		.disp = 0,
		.stages = de_ui0_stages,
		.nstages = DPY_ARRAY_SIZE(de_ui0_stages),
		.formats = de_rgb_formats,
		.nformats = DPY_ARRAY_SIZE(de_rgb_formats),
		.max_width = 4096,
		.max_height = 4096,
	},
	{
		.name = "ui1",
		.type = DE_PIPE_FRONTEND,
		.id = 2,
		.disp = 0,
		.stages = de_ui1_stages,
		.nstages = DPY_ARRAY_SIZE(de_ui1_stages),
		.formats = de_rgb_formats,
		.nformats = DPY_ARRAY_SIZE(de_rgb_formats),
		.max_width = 4096,
		.max_height = 4096,
	},
	{
		.name = "disp0",
		.type = DE_PIPE_BACKEND,
		.id = 0,
		.disp = 0,
		.stages = de_disp0_stages,
		.nstages = DPY_ARRAY_SIZE(de_disp0_stages),
	},
};

const struct de_soc_desc de_sun252iw2_desc = {
	.name = "sun252iw2-de",
	.core_clk_hz = 300000000,
	.pipes = de_sun252iw2_pipes,
	.npipes = DPY_ARRAY_SIZE(de_sun252iw2_pipes),
	.wb_offset = 0x010000,
	.regs_arena = 16 * 1024,
};
