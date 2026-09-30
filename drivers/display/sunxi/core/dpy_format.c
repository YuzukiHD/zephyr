// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - pixel format and mode helpers.
 */
#include <dpy/dpy_kms.h>

#define RGB(_f, _cpp, _a) \
	[_f] = { .format = _f, .name = #_f + 15, .num_planes = 1, \
		 .cpp = { _cpp }, .hsub = 1, .vsub = 1, .has_alpha = _a }
#define PACKED_YUV(_f) \
	[_f] = { .format = _f, .name = #_f + 15, .num_planes = 1, \
		 .cpp = { 2 }, .hsub = 2, .vsub = 1, .is_yuv = true }
#define SP_YUV(_f, _h, _v) \
	[_f] = { .format = _f, .name = #_f + 15, .num_planes = 2, \
		 .cpp = { 1, 2 }, .hsub = _h, .vsub = _v, .is_yuv = true }
#define P_YUV(_f, _h, _v) \
	[_f] = { .format = _f, .name = #_f + 15, .num_planes = 3, \
		 .cpp = { 1, 1, 1 }, .hsub = _h, .vsub = _v, .is_yuv = true }

static const struct dpy_format_info dpy_formats[DISPLAY_FORMAT_COUNT] = {
	RGB(DISPLAY_FORMAT_ARGB8888, 4, true),
	RGB(DISPLAY_FORMAT_ABGR8888, 4, true),
	RGB(DISPLAY_FORMAT_RGBA8888, 4, true),
	RGB(DISPLAY_FORMAT_BGRA8888, 4, true),
	RGB(DISPLAY_FORMAT_XRGB8888, 4, false),
	RGB(DISPLAY_FORMAT_XBGR8888, 4, false),
	RGB(DISPLAY_FORMAT_RGBX8888, 4, false),
	RGB(DISPLAY_FORMAT_BGRX8888, 4, false),
	RGB(DISPLAY_FORMAT_RGB888, 3, false),
	RGB(DISPLAY_FORMAT_BGR888, 3, false),
	RGB(DISPLAY_FORMAT_RGB565, 2, false),
	RGB(DISPLAY_FORMAT_BGR565, 2, false),
	RGB(DISPLAY_FORMAT_ARGB4444, 2, true),
	RGB(DISPLAY_FORMAT_ABGR4444, 2, true),
	RGB(DISPLAY_FORMAT_RGBA4444, 2, true),
	RGB(DISPLAY_FORMAT_BGRA4444, 2, true),
	RGB(DISPLAY_FORMAT_ARGB1555, 2, true),
	RGB(DISPLAY_FORMAT_ABGR1555, 2, true),
	RGB(DISPLAY_FORMAT_RGBA5551, 2, true),
	RGB(DISPLAY_FORMAT_BGRA5551, 2, true),
	PACKED_YUV(DISPLAY_FORMAT_YUYV),
	PACKED_YUV(DISPLAY_FORMAT_YVYU),
	PACKED_YUV(DISPLAY_FORMAT_UYVY),
	PACKED_YUV(DISPLAY_FORMAT_VYUY),
	SP_YUV(DISPLAY_FORMAT_NV12, 2, 2),
	SP_YUV(DISPLAY_FORMAT_NV21, 2, 2),
	SP_YUV(DISPLAY_FORMAT_NV16, 2, 1),
	SP_YUV(DISPLAY_FORMAT_NV61, 2, 1),
	SP_YUV(DISPLAY_FORMAT_NV411, 4, 1),
	SP_YUV(DISPLAY_FORMAT_NV114, 4, 1),
	P_YUV(DISPLAY_FORMAT_YUV420, 2, 2),
	P_YUV(DISPLAY_FORMAT_YVU420, 2, 2),
	P_YUV(DISPLAY_FORMAT_YUV422, 2, 1),
	P_YUV(DISPLAY_FORMAT_YUV411, 4, 1),
};

const struct dpy_format_info *dpy_format_info(uint32_t format)
{
	if (format == DISPLAY_FORMAT_INVALID || format >= DISPLAY_FORMAT_COUNT)
		return NULL;
	return dpy_formats[format].num_planes ? &dpy_formats[format] : NULL;
}

uint32_t display_format_cpp(enum display_format format, uint32_t plane)
{
	const struct dpy_format_info *info = dpy_format_info(format);

	if (!info || plane >= info->num_planes)
		return 0;
	return info->cpp[plane];
}

const char *display_format_name(enum display_format format)
{
	const struct dpy_format_info *info = dpy_format_info(format);

	return info ? info->name : "INVALID";
}

uint32_t dpy_bus_format_bpc(uint32_t bus_format)
{
	switch (bus_format) {
	case DPY_BUS_FMT_RGB666_1X18:
	case DPY_BUS_FMT_RGB666_1X7X3_SPWG:
		return 6;
	case DPY_BUS_FMT_RGB565_1X16:
		return 5;
	default:
		return 8;
	}
}

/* ------------------------------------------------------------------ */
/* Modes                                                               */
/* ------------------------------------------------------------------ */
uint32_t dpy_mode_vrefresh_mhz(const struct dpy_display_mode *m)
{
	uint64_t total = (uint64_t)m->htotal * m->vtotal;

	if (!total)
		return 0;
	return (uint32_t)(((uint64_t)m->clock * 1000000ULL + total / 2) / total);
}

bool dpy_mode_equal(const struct dpy_display_mode *a,
		    const struct dpy_display_mode *b)
{
	return a->clock == b->clock &&
	       a->hdisplay == b->hdisplay && a->hsync_start == b->hsync_start &&
	       a->hsync_end == b->hsync_end && a->htotal == b->htotal &&
	       a->vdisplay == b->vdisplay && a->vsync_start == b->vsync_start &&
	       a->vsync_end == b->vsync_end && a->vtotal == b->vtotal &&
	       (a->flags & ~DISPLAY_MODE_FLAG_PREFERRED) ==
	       (b->flags & ~DISPLAY_MODE_FLAG_PREFERRED);
}

bool dpy_mode_valid(const struct dpy_display_mode *m)
{
	return m->clock && m->hdisplay && m->vdisplay &&
	       m->hdisplay <= m->hsync_start &&
	       m->hsync_start <= m->hsync_end &&
	       m->hsync_end <= m->htotal &&
	       m->vdisplay <= m->vsync_start &&
	       m->vsync_start <= m->vsync_end &&
	       m->vsync_end <= m->vtotal;
}

void dpy_mode_to_api(const struct dpy_display_mode *m, struct display_mode *out)
{
	memset(out, 0, sizeof(*out));
	out->width = m->hdisplay;
	out->height = m->vdisplay;
	out->htotal = m->htotal;
	out->vtotal = m->vtotal;
	out->hsync_start = m->hsync_start;
	out->hsync_end = m->hsync_end;
	out->vsync_start = m->vsync_start;
	out->vsync_end = m->vsync_end;
	out->clock_khz = m->clock;
	out->refresh_hz = (dpy_mode_vrefresh_mhz(m) + 500) / 1000;
	out->flags = m->flags;
}

void dpy_mode_from_api(const struct display_mode *in, struct dpy_display_mode *m)
{
	memset(m, 0, sizeof(*m));
	m->hdisplay = (uint16_t)in->width;
	m->vdisplay = (uint16_t)in->height;
	m->htotal = (uint16_t)in->htotal;
	m->vtotal = (uint16_t)in->vtotal;
	m->hsync_start = (uint16_t)in->hsync_start;
	m->hsync_end = (uint16_t)in->hsync_end;
	m->vsync_start = (uint16_t)in->vsync_start;
	m->vsync_end = (uint16_t)in->vsync_end;
	m->clock = in->clock_khz;
	m->flags = in->flags;
}
