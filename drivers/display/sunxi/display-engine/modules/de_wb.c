// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Write-back: captures the blended output of a display into memory, with
 * optional crop, down scaling and RGB -> YUV conversion. Exposed as a
 * writeback connector; one capture job is executed per commit.
 *
 * The write-back registers are programmed directly (not through the RCQ)
 * right before the capture is started.
 */
#define DPY_LOG_TAG "de-wb"
#include "../de_priv.h"
#include "de_scaler.h"

#define WB_GCTRL			0x000
#define   WB_GCTRL_START		DPY_BIT(0)
#define   WB_GCTRL_IN_PORT		DPY_GENMASK(17, 16)
#define   WB_GCTRL_CLK_GATE		DPY_BIT(29)
#define WB_SIZE				0x004
#define WB_CROP_COORD			0x008
#define WB_CROP_SIZE			0x00c
#define WB_ADDR_A(p)			(0x010 + (p) * 4)
#define WB_ADDR_AH			0x01c
#define WB_PITCH(p)			(0x030 + (p) * 4)
#define WB_FORMAT			0x044
#define WB_STATUS			0x04c
#define   WB_STATUS_FINISH		DPY_BIT(4)
#define   WB_STATUS_OVERFLOW		DPY_BIT(5)
#define   WB_STATUS_TIMEOUT		DPY_BIT(6)
#define   WB_STATUS_ALL			(DPY_BIT(0) | DPY_BIT(4) | DPY_BIT(5) | DPY_BIT(6))
#define WB_BYPASS			0x054
#define   WB_BYPASS_CSC			DPY_BIT(0)
#define   WB_BYPASS_COARSE		DPY_BIT(1)
#define   WB_BYPASS_FINE		DPY_BIT(2)
#define WB_CS_HORZ			0x070
#define WB_CS_VERT			0x074
#define WB_FS_INSIZE			0x080
#define WB_FS_OUTSIZE			0x084
#define WB_FS_HSTEP			0x088
#define WB_FS_VSTEP			0x08c
#define WB_Y_COEF(n)			(0x200 + (n) * 4)
#define WB_C_COEF(n)			(0x280 + (n) * 4)

#define WB_FRAC				18
#define WB_SIZE_REG(w, h)		(((uint32_t)((h) - 1) << 16) | ((w) - 1))
#define WB_MIN_W			8
#define WB_MIN_H			4
#define WB_MAX_W			4096
#define WB_LINE_BUF			2048

struct de_wb {
	struct de_engine *de;
	uint32_t base;
	uint32_t captures;
	uint32_t errors;
};

static const struct {
	uint32_t format;
	uint8_t code;
	bool yuv;
} de_wb_formats[] = {
	{ DISPLAY_FORMAT_RGB888, 0x0, false },
	{ DISPLAY_FORMAT_BGR888, 0x1, false },
	{ DISPLAY_FORMAT_ARGB8888, 0x4, false },
	{ DISPLAY_FORMAT_ABGR8888, 0x5, false },
	{ DISPLAY_FORMAT_BGRA8888, 0x6, false },
	{ DISPLAY_FORMAT_RGBA8888, 0x7, false },
	{ DISPLAY_FORMAT_YUV420, 0x8, true },
	{ DISPLAY_FORMAT_NV12, 0xc, true },
	{ DISPLAY_FORMAT_NV21, 0xd, true },
};

static int de_wb_format(uint32_t format, uint8_t *code, bool *yuv)
{
	unsigned int i;

	for (i = 0; i < DPY_ARRAY_SIZE(de_wb_formats); i++) {
		if (de_wb_formats[i].format == format) {
			*code = de_wb_formats[i].code;
			*yuv = de_wb_formats[i].yuv;
			return 0;
		}
	}
	return -EINVAL;
}

static inline void wb_write(struct de_wb *wb, uint32_t reg, uint32_t val)
{
	de_write(wb->de, wb->base + reg, val);
}

static inline uint32_t wb_read(struct de_wb *wb, uint32_t reg)
{
	return de_read(wb->de, wb->base + reg);
}

static int de_wb_connector_check(struct dpy_connector *connector,
				 struct dpy_atomic_state *state)
{
	struct dpy_connector_state *st;
	struct dpy_writeback_job *job;
	struct dpy_crtc_state *cs;
	uint8_t code;
	bool yuv;

	st = state->connectors[connector->index].new_state;
	job = st ? st->wb_job : NULL;
	if (!job)
		return 0;
	cs = dpy_atomic_new_crtc_state(state, st->crtc);
	if (!cs || !cs->active)
		return -EINVAL;
	if (de_wb_format(job->fb.format, &code, &yuv))
		return -EINVAL;
	if (job->src.x < 0 || job->src.y < 0 ||
	    job->src.x + job->src.w > cs->mode.hdisplay ||
	    job->src.y + job->src.h > cs->mode.vdisplay ||
	    job->src.w < WB_MIN_W || job->src.h < WB_MIN_H)
		return -ERANGE;
	if (job->dst.x < 0 || job->dst.y < 0 ||
	    job->dst.x + job->dst.w > job->fb.width ||
	    job->dst.y + job->dst.h > job->fb.height ||
	    job->dst.w < WB_MIN_W || job->dst.h < WB_MIN_H ||
	    job->dst.w > job->src.w || job->dst.h > job->src.h)
		return -ERANGE;
	if (yuv && job->dst.w > WB_LINE_BUF)
		return -ERANGE;
	return 0;
}

static const struct dpy_connector_funcs de_wb_connector_funcs = {
	.atomic_check = de_wb_connector_check,
};

int de_wb_create(struct de_engine *de, struct dpy_device *ddev)
{
	struct de_wb *wb;
	int ret;

	if (!de->soc->wb_offset)
		return 0;
	wb = dpy_os_zalloc(sizeof(*wb));
	if (!wb)
		return -ENOMEM;
	wb->de = de;
	wb->base = de->soc->wb_offset;
	de->wb_connector.priv = wb;
	ret = dpy_connector_init(ddev, &de->wb_connector, &de_wb_connector_funcs,
				 DPY_CONNECTOR_WRITEBACK, "writeback");
	if (ret) {
		dpy_os_free(wb);
		return ret;
	}
	de->wb = wb;
	return 0;
}

void de_wb_destroy(struct de_engine *de)
{
	if (!de->wb)
		return;
	dpy_connector_cleanup(&de->wb_connector);
	dpy_os_free(de->wb);
	de->wb = NULL;
}

static void de_wb_setup_scaler(struct de_wb *wb, uint32_t cw, uint32_t ch,
			       uint32_t ow, uint32_t oh, bool yuv420)
{
	uint32_t bypass = wb_read(wb, WB_BYPASS) &
			  ~(WB_BYPASS_COARSE | WB_BYPASS_FINE);
	uint32_t cs_w = cw, cs_h = ch;
	uint32_t hstep, vstep;
	unsigned int bucket, i;

	/* coarse: integer decimation while more than 2x down */
	wb_write(wb, WB_CS_HORZ, 0);
	wb_write(wb, WB_CS_VERT, 0);
	if (cw > 2 * ow) {
		wb_write(wb, WB_CS_HORZ, cw | (ow << 17));
		cs_w = 2 * ow;
		bypass |= WB_BYPASS_COARSE;
	}
	if (ch > 2 * oh) {
		wb_write(wb, WB_CS_VERT, ch | (oh << 17));
		cs_h = 2 * oh;
		bypass |= WB_BYPASS_COARSE;
	}

	/* fine: polyphase scaler for the rest */
	if (cs_w != ow || cs_h != oh || yuv420) {
		hstep = (uint32_t)(((uint64_t)cs_w << WB_FRAC) / ow);
		vstep = (uint32_t)(((uint64_t)cs_h << WB_FRAC) / oh);
		wb_write(wb, WB_FS_HSTEP, ((hstep & ((1U << WB_FRAC) - 1)) << 2) |
			 ((hstep >> WB_FRAC) << 20));
		wb_write(wb, WB_FS_VSTEP, ((vstep & ((1U << WB_FRAC) - 1)) << 2) |
			 ((vstep >> WB_FRAC) << 20));
		bucket = de_scaler_bucket(DPY_MAX(hstep, vstep), WB_FRAC);
		for (i = 0; i < DE_GSU_PHASES; i++) {
			wb_write(wb, WB_Y_COEF(i),
				 de_gsu_coef[bucket * DE_GSU_PHASES + i]);
			wb_write(wb, WB_C_COEF(i),
				 de_gsu_coef[bucket * DE_GSU_PHASES + i]);
		}
		bypass |= WB_BYPASS_FINE;
	} else {
		wb_write(wb, WB_FS_HSTEP, 1U << 20);
		wb_write(wb, WB_FS_VSTEP, 1U << 20);
	}
	wb_write(wb, WB_FS_INSIZE, WB_SIZE_REG(cs_w, cs_h));
	wb_write(wb, WB_FS_OUTSIZE, WB_SIZE_REG(ow, oh));
	wb_write(wb, WB_BYPASS, bypass);
}

int de_wb_run(struct de_engine *de, uint8_t disp,
	      const struct dpy_display_mode *mode,
	      struct dpy_writeback_job *job, struct dpy_crtc *crtc)
{
	struct de_wb *wb = de->wb;
	const struct dpy_format_info *fi = job->fb.info;
	uint32_t status = 0, p, t, limit_ms;
	uint8_t code;
	bool yuv;
	int ret;

	if (!wb || de_wb_format(job->fb.format, &code, &yuv)) {
		ret = -EINVAL;
		goto done;
	}

	de_top_wb_enable(de, true);
	wb_write(wb, WB_STATUS, WB_STATUS_ALL);
	wb_write(wb, WB_GCTRL, WB_GCTRL_CLK_GATE |
		 DPY_FIELD_PREP(WB_GCTRL_IN_PORT, disp));
	wb_write(wb, WB_SIZE, WB_SIZE_REG(mode->hdisplay, mode->vdisplay));
	wb_write(wb, WB_CROP_COORD, (uint32_t)job->src.x |
		 ((uint32_t)job->src.y << 16));
	wb_write(wb, WB_CROP_SIZE, WB_SIZE_REG(job->src.w, job->src.h));
	wb_write(wb, WB_FORMAT, code);

	for (p = 0; p < 3; p++) {
		uint64_t a = 0;

		if (p < fi->num_planes) {
			uint32_t hs = p ? fi->hsub : 1, vs = p ? fi->vsub : 1;

			a = job->fb.addr[p] +
			    (uint64_t)(job->dst.y / vs) * job->fb.pitch[p] +
			    (uint64_t)(job->dst.x / hs) * fi->cpp[p];
		}
		wb_write(wb, WB_ADDR_A(p), (uint32_t)a);
	}
	wb_write(wb, WB_ADDR_AH, 0);
	wb_write(wb, WB_PITCH(0), job->fb.pitch[0]);
	wb_write(wb, WB_PITCH(1), fi->num_planes > 1 ? job->fb.pitch[1] : 0);

	/* YUV output uses the built-in BT.601 conversion */
	wb_write(wb, WB_BYPASS, yuv ? WB_BYPASS_CSC : 0);
	de_wb_setup_scaler(wb, job->src.w, job->src.h, job->dst.w, job->dst.h,
			   yuv && fi->vsub > 1);

	/* capture the next frame of the running output */
	wb_write(wb, WB_GCTRL, wb_read(wb, WB_GCTRL) | WB_GCTRL_START);

	/* the capture starts on the next frame: allow three frames plus margin */
	limit_ms = 3 * 1000 / DPY_MAX(dpy_mode_vrefresh_mhz(mode) / 1000, 1U) + 20;
	for (t = 0; t < limit_ms; t++) {
		status = wb_read(wb, WB_STATUS);
		if (status & (WB_STATUS_FINISH | WB_STATUS_OVERFLOW |
			      WB_STATUS_TIMEOUT))
			break;
		dpy_os_msleep(1);
	}
	wb_write(wb, WB_STATUS, WB_STATUS_ALL);
	wb_write(wb, WB_GCTRL, WB_GCTRL_CLK_GATE);
	de_top_wb_enable(de, false);

	if (status & WB_STATUS_FINISH)
		ret = 0;
	else if (status & WB_STATUS_OVERFLOW)
		ret = -EOVERFLOW;
	else
		ret = -ETIMEDOUT;
	if (ret) {
		wb->errors++;
		dpy_warn("%s: capture failed: %d (status %08x)\n", crtc->name,
			 ret, status);
	}
	wb->captures++;

done:
	job->status = ret;
	if (job->done)
		dpy_os_sem_post(job->done);
	return ret;
}
