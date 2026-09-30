// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * display-engine test suite, written against the public API only
 * (hal/display/display_engine.h). The shell front-end is OS specific and
 * lives in osal/.
 *
 *   display_test info                     capabilities, modes, planes
 *   display_test pattern <0..6>           TCON test pattern (0: off)
 *   display_test fill <plane> <fmt> [w h] colour bars on one plane, 1s
 *   display_test hold <plane> <fmt> [w h] colour bars, left up for 'off'
 *   display_test overlay [fmt]            YUV colour bars on the video channel
 *                                         (default NV12) under a translucent
 *                                         ARGB layer, left up for 'off'
 *   display_test video [fmt]              a YUV window (video channel, z 1) and
 *                                         a translucent ARGB layer (UI1, z 2)
 *                                         over whatever plane 4 shows (e.g. an
 *                                         LVGL screen at z 0); only these two
 *                                         planes are touched, 'off' removes them
 *   display_test formats <plane>          cycle through the plane formats
 *   display_test scale <plane>            one buffer, several sizes
 *   display_test alpha                    global and per pixel alpha
 *   display_test zorder                   three channels, rotating z
 *   display_test capture                  write-back of the screen
 *   display_test bl <0..255>              backlight
 *   display_test bcsh <b> <c> <s> <h>     picture adjustment, 0..100
 *   display_test gamma <x100>             gamma curve, 100 = linear
 *   display_test bg <rrggbb>              background colour
 *   display_test off                      disable all planes
 *   display_test blank <0|1>
 *   display_test stats
 */
#include <stdio.h>
#include <stdlib.h>

#include <dpy/dpy_debug.h>
#include <dpy/dpy_os.h>
#include <hal/display/display_engine.h>

#define T_ERR(...) dpy_os_printf("display_test: " __VA_ARGS__)

struct dt_buf {
	void *cpu;
	size_t size;
	struct display_framebuffer fb;
};

static const uint32_t dt_bars[8] = {
	0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
	0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000,
};

static int dt_mode(struct display_mode *m)
{
	int ret = display_get_mode(m);

	if (ret)
		T_ERR("no active display (%d)\n", ret);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Buffers                                                             */
/* ------------------------------------------------------------------ */
static int dt_alloc(struct dt_buf *b, enum display_format fmt, uint32_t w,
		    uint32_t h)
{
	uint32_t cpp0 = display_format_cpp(fmt, 0);
	uint32_t n, p, pitch[3] = { 0 }, ph[3] = { 0 };
	size_t size = 0;
	uintptr_t base;

	memset(b, 0, sizeof(*b));
	if (!cpp0)
		return -EINVAL;
	n = display_format_cpp(fmt, 2) ? 3 : display_format_cpp(fmt, 1) ? 2 : 1;
	for (p = 0; p < n; p++) {
		uint32_t hs = 1, vs = 1;

		if (p) {
			switch (fmt) {
			case DISPLAY_FORMAT_NV12: case DISPLAY_FORMAT_NV21:
			case DISPLAY_FORMAT_YUV420: case DISPLAY_FORMAT_YVU420:
				hs = 2; vs = 2; break;
			case DISPLAY_FORMAT_NV411: case DISPLAY_FORMAT_NV114:
			case DISPLAY_FORMAT_YUV411:
				hs = 4; break;
			default:
				hs = 2; break;
			}
		}
		pitch[p] = ((w + hs - 1) / hs * display_format_cpp(fmt, p) + 3) & ~3U;
		ph[p] = (h + vs - 1) / vs;
		size += (size_t)pitch[p] * ph[p];
	}
	b->cpu = dpy_os_dma_alloc(size, 4096, NULL);
	if (!b->cpu) {
		T_ERR("out of memory (%u bytes)\n", (unsigned int)size);
		return -ENOMEM;
	}
	b->size = size;
	base = (uintptr_t)b->cpu;
	b->fb.format = fmt;
	b->fb.width = w;
	b->fb.height = h;
	b->fb.plane_count = n;
	for (p = 0; p < n; p++) {
		b->fb.plane_address[p] = base;
		b->fb.plane_stride[p] = pitch[p];
		b->fb.plane_size[p] = pitch[p] * ph[p];
		base += (uintptr_t)pitch[p] * ph[p];
	}
	b->fb.address = b->fb.plane_address[0];
	b->fb.stride = pitch[0];
	return 0;
}

static void dt_free(struct dt_buf *b)
{
	dpy_os_dma_free(b->cpu);
	b->cpu = NULL;
}

static void dt_rgb2yuv(uint32_t argb, uint8_t *y, uint8_t *u, uint8_t *v)
{
	int r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;

	/* BT.601 limited range */
	*y = (uint8_t)(16 + ((66 * r + 129 * g + 25 * b + 128) >> 8));
	*u = (uint8_t)(128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8));
	*v = (uint8_t)(128 + ((112 * r - 94 * g - 18 * b + 128) >> 8));
}

static void dt_put_rgb(uint8_t *p, enum display_format f, uint32_t c)
{
	uint32_t a = c >> 24, r = (c >> 16) & 0xff, g = (c >> 8) & 0xff,
		 b = c & 0xff;
	uint32_t w = 0;
	uint16_t s = 0;

	switch (f) {
	case DISPLAY_FORMAT_ARGB8888: case DISPLAY_FORMAT_XRGB8888:
		w = c; break;
	case DISPLAY_FORMAT_ABGR8888: case DISPLAY_FORMAT_XBGR8888:
		w = (a << 24) | (b << 16) | (g << 8) | r; break;
	case DISPLAY_FORMAT_RGBA8888: case DISPLAY_FORMAT_RGBX8888:
		w = (r << 24) | (g << 16) | (b << 8) | a; break;
	case DISPLAY_FORMAT_BGRA8888: case DISPLAY_FORMAT_BGRX8888:
		w = (b << 24) | (g << 16) | (r << 8) | a; break;
	case DISPLAY_FORMAT_RGB888:
		p[0] = (uint8_t)b; p[1] = (uint8_t)g; p[2] = (uint8_t)r; return;
	case DISPLAY_FORMAT_BGR888:
		p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b; return;
	case DISPLAY_FORMAT_RGB565:
		s = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); break;
	case DISPLAY_FORMAT_BGR565:
		s = (uint16_t)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3)); break;
	case DISPLAY_FORMAT_ARGB4444:
		s = (uint16_t)(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)); break;
	case DISPLAY_FORMAT_ABGR4444:
		s = (uint16_t)(((a >> 4) << 12) | ((b >> 4) << 8) | ((g >> 4) << 4) | (r >> 4)); break;
	case DISPLAY_FORMAT_RGBA4444:
		s = (uint16_t)(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4)); break;
	case DISPLAY_FORMAT_BGRA4444:
		s = (uint16_t)(((b >> 4) << 12) | ((g >> 4) << 8) | ((r >> 4) << 4) | (a >> 4)); break;
	case DISPLAY_FORMAT_ARGB1555:
		s = (uint16_t)(((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)); break;
	case DISPLAY_FORMAT_ABGR1555:
		s = (uint16_t)(((a >> 7) << 15) | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3)); break;
	case DISPLAY_FORMAT_RGBA5551:
		s = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7)); break;
	case DISPLAY_FORMAT_BGRA5551:
		s = (uint16_t)(((b >> 3) << 11) | ((g >> 3) << 6) | ((r >> 3) << 1) | (a >> 7)); break;
	default:
		return;
	}
	if (display_format_cpp(f, 0) == 4)
		memcpy(p, &w, 4);
	else
		memcpy(p, &s, 2);
}

/* colour bars with an alpha ramp from left (transparent) to right */
static uint32_t dt_pixel(const struct dt_buf *b, uint32_t x, uint32_t y,
			 bool ramp)
{
	uint32_t c = dt_bars[x * 8 / b->fb.width];

	(void)y;
	if (ramp)
		c = (c & 0xffffff) | ((x * 255 / (b->fb.width - 1)) << 24);
	return c;
}

static void dt_fill(struct dt_buf *b, bool ramp)
{
	const struct display_framebuffer *fb = &b->fb;
	enum display_format f = fb->format;
	uint32_t cpp = display_format_cpp(f, 0);
	uint8_t *p0 = (uint8_t *)fb->plane_address[0];
	uint32_t x, y;

	if (f < DISPLAY_FORMAT_YUYV) {
		for (y = 0; y < fb->height; y++)
			for (x = 0; x < fb->width; x++)
				dt_put_rgb(p0 + y * fb->plane_stride[0] + x * cpp,
					   f, dt_pixel(b, x, y, ramp));
		goto done;
	}

	for (y = 0; y < fb->height; y++) {
		for (x = 0; x < fb->width; x++) {
			uint8_t Y, U, V, *c;
			uint32_t hs, vs;

			dt_rgb2yuv(dt_pixel(b, x, y, false), &Y, &U, &V);
			switch (f) {
			case DISPLAY_FORMAT_YUYV: case DISPLAY_FORMAT_YVYU:
			case DISPLAY_FORMAT_UYVY: case DISPLAY_FORMAT_VYUY: {
				uint8_t *q = p0 + y * fb->plane_stride[0] + (x & ~1U) * 2;
				bool odd = x & 1;

				if (f == DISPLAY_FORMAT_YUYV) {
					q[odd ? 2 : 0] = Y; q[1] = U; q[3] = V;
				} else if (f == DISPLAY_FORMAT_YVYU) {
					q[odd ? 2 : 0] = Y; q[1] = V; q[3] = U;
				} else if (f == DISPLAY_FORMAT_UYVY) {
					q[odd ? 3 : 1] = Y; q[0] = U; q[2] = V;
				} else {
					q[odd ? 3 : 1] = Y; q[0] = V; q[2] = U;
				}
				continue;
			}
			default:
				break;
			}
			p0[y * fb->plane_stride[0] + x] = Y;
			hs = f == DISPLAY_FORMAT_NV411 || f == DISPLAY_FORMAT_NV114 ||
			     f == DISPLAY_FORMAT_YUV411 ? 4 : 2;
			vs = f == DISPLAY_FORMAT_NV12 || f == DISPLAY_FORMAT_NV21 ||
			     f == DISPLAY_FORMAT_YUV420 || f == DISPLAY_FORMAT_YVU420 ? 2 : 1;
			if ((x % hs) || (y % vs))
				continue;
			if (fb->plane_count == 2) {
				bool vu = f == DISPLAY_FORMAT_NV21 ||
					  f == DISPLAY_FORMAT_NV61 ||
					  f == DISPLAY_FORMAT_NV114;

				c = (uint8_t *)fb->plane_address[1] +
				    (y / vs) * fb->plane_stride[1] + (x / hs) * 2;
				c[0] = vu ? V : U;
				c[1] = vu ? U : V;
			} else {
				bool vu = f == DISPLAY_FORMAT_YVU420;

				((uint8_t *)fb->plane_address[1])[(y / vs) * fb->plane_stride[1] + x / hs] = vu ? V : U;
				((uint8_t *)fb->plane_address[2])[(y / vs) * fb->plane_stride[2] + x / hs] = vu ? U : V;
			}
		}
	}
done:
	dpy_os_dcache_clean(b->cpu, b->size);
}

static void dt_solid(struct dt_buf *b, uint32_t argb)
{
	uint32_t x, y, cpp = display_format_cpp(b->fb.format, 0);

	for (y = 0; y < b->fb.height; y++)
		for (x = 0; x < b->fb.width; x++)
			dt_put_rgb((uint8_t *)b->fb.plane_address[0] +
				   y * b->fb.plane_stride[0] + x * cpp,
				   b->fb.format, argb);
	dpy_os_dcache_clean(b->cpu, b->size);
}

/* ------------------------------------------------------------------ */
/* Submit helpers                                                      */
/* ------------------------------------------------------------------ */
static void dt_plane(struct display_pipeline_state *s, uint32_t id,
		     const struct dt_buf *b, int32_t x, int32_t y, uint32_t w,
		     uint32_t h, uint8_t z, uint8_t alpha)
{
	struct display_plane_state *p = &s->planes[s->plane_count++];

	memset(p, 0, sizeof(*p));
	p->enable = true;
	p->plane_id = id;
	p->framebuffer = b->fb;
	p->destination.x = x;
	p->destination.y = y;
	p->destination.width = w;
	p->destination.height = h;
	p->zpos = z;
	p->alpha = alpha;
}

static int dt_submit(struct display_pipeline_state *s, const char *what)
{
	int ret = display_submit(s);

	if (ret)
		T_ERR("%s: submit failed: %d\n", what, ret);
	return ret;
}

static void dt_blank_planes(void)
{
	struct display_pipeline_state s;

	display_pipeline_state_init(&s);
	s.plane_count = 0;
	display_submit(&s);
}

/*
 * hold/unhold: like fill, but the plane is left on screen instead of being
 * blanked after one second, so a separate `display_test capture` can read
 * it back while it is still showing. Diagnostic only, one buffer at a time.
 */
static struct dt_buf dt_hold_buf;
static bool dt_held;

static int dt_hold(uint32_t plane, enum display_format fmt, uint32_t w,
		   uint32_t h)
{
	struct display_pipeline_state s;
	struct display_mode m;
	int ret;

	if (dt_held) {
		T_ERR("already holding a plane, run 'off' first\n");
		return -EBUSY;
	}
	if (dt_mode(&m))
		return -ENODEV;
	ret = dt_alloc(&dt_hold_buf, fmt, w ? w : m.width, h ? h : m.height);
	if (ret)
		return ret;
	dt_fill(&dt_hold_buf, false);
	display_pipeline_state_init(&s);
	s.plane_count = 0;
	dt_plane(&s, plane, &dt_hold_buf, 0, 0, m.width, m.height, 0, 0xff);
	ret = dt_submit(&s, display_format_name(fmt));
	if (ret) {
		dt_free(&dt_hold_buf);
		return ret;
	}
	dt_held = true;
	dpy_os_printf("holding plane %u, %s %ux%u; 'display_test off' to clear\n",
		      plane, display_format_name(fmt), dt_hold_buf.fb.width,
		      dt_hold_buf.fb.height);
	return 0;
}

static struct dt_buf dt_hold_buf2;
static bool dt_held2;
static bool dt_video_active;

/* switch off planes 0 and 8 and leave the others (an LVGL plane) alone */
static void dt_video_off(void)
{
	struct display_pipeline_state s;

	display_pipeline_state_init(&s);
	s.plane_count = 2;
	s.planes[0].plane_id = 0;
	s.planes[0].enable = false;
	s.planes[1].plane_id = 8;
	s.planes[1].enable = false;
	display_submit_ex(&s, DISPLAY_SUBMIT_PARTIAL);
}

static void dt_unhold(void)
{
	if (dt_video_active) {
		dt_video_off();
		dt_video_active = false;
	} else {
		dt_blank_planes();
	}
	if (dt_held) {
		dt_free(&dt_hold_buf);
		dt_held = false;
	}
	if (dt_held2) {
		dt_free(&dt_hold_buf2);
		dt_held2 = false;
	}
}

static void dt_rect(struct dt_buf *b, uint32_t x0, uint32_t y0, uint32_t x1,
		    uint32_t y1, uint32_t argb)
{
	uint32_t x, y, cpp = display_format_cpp(b->fb.format, 0);

	for (y = y0; y < y1 && y < b->fb.height; y++)
		for (x = x0; x < x1 && x < b->fb.width; x++)
			dt_put_rgb((uint8_t *)b->fb.plane_address[0] +
				   y * b->fb.plane_stride[0] + x * cpp,
				   b->fb.format, argb);
}

/*
 * A YUV picture on the video channel (plane 0) with an ARGB layer on the
 * first UI channel (plane 4) on top: per pixel alpha over the video. The
 * layer has a transparent background, an alpha ramp strip, a translucent
 * panel with a white border and three translucent colour squares.
 */
static int dt_overlay(enum display_format vfmt)
{
	struct display_pipeline_state s;
	struct display_mode m;
	uint32_t w, h, x, i, bw, sq;
	struct dt_buf *v = &dt_hold_buf, *o = &dt_hold_buf2;
	static const uint32_t squares[3] = { 0xa0ff0000, 0xa000ff00, 0xa00000ff };
	int ret;

	if (dt_held || dt_held2) {
		T_ERR("already holding a plane, run 'off' first\n");
		return -EBUSY;
	}
	if (dt_mode(&m))
		return -ENODEV;
	w = m.width;
	h = m.height;
	ret = dt_alloc(v, vfmt, w, h);
	if (ret)
		return ret;
	ret = dt_alloc(o, DISPLAY_FORMAT_ARGB8888, w, h);
	if (ret) {
		dt_free(v);
		return ret;
	}

	dt_fill(v, false);

	dt_solid(o, 0x00000000);
	for (x = 0; x < w; x++)
		dt_rect(o, x, 0, x + 1, h / 8,
			((x * 255 / (w - 1)) << 24) | 0x00ffffff);
	dt_rect(o, w / 8, h / 4, w * 7 / 8, h * 3 / 4, 0x99000000);
	bw = 4;
	dt_rect(o, w / 8, h / 4, w * 7 / 8, h / 4 + bw, 0xffffffff);
	dt_rect(o, w / 8, h * 3 / 4 - bw, w * 7 / 8, h * 3 / 4, 0xffffffff);
	dt_rect(o, w / 8, h / 4, w / 8 + bw, h * 3 / 4, 0xffffffff);
	dt_rect(o, w * 7 / 8 - bw, h / 4, w * 7 / 8, h * 3 / 4, 0xffffffff);
	sq = h / 6;
	for (i = 0; i < 3; i++)
		dt_rect(o, w / 8 + sq / 2 + i * (sq + sq / 2),
			h / 2 - sq / 2,
			w / 8 + sq / 2 + i * (sq + sq / 2) + sq,
			h / 2 + sq / 2, squares[i]);
	dpy_os_dcache_clean(o->cpu, o->size);

	display_pipeline_state_init(&s);
	s.plane_count = 0;
	dt_plane(&s, 0, v, 0, 0, w, h, 0, 0xff);
	dt_plane(&s, 4, o, 0, 0, w, h, 1, 0xff);
	ret = dt_submit(&s, "overlay");
	if (ret) {
		dt_free(o);
		dt_free(v);
		return ret;
	}
	dt_held = true;
	dt_held2 = true;
	dpy_os_printf("overlay: plane 0 %s %ux%u under plane 4 ARGB8888; "
		      "'display_test off' to clear\n",
		      display_format_name(vfmt), w, h);
	return 0;
}

/*
 * A video window (plane 0, video channel) at the centre of the screen and a
 * translucent layer (plane 8, second UI channel) above it, over the plane 4
 * content. Only planes 0 and 8 are submitted (partial commit), so a plane 4
 * owner such as LVGL keeps running underneath. The z order is plane 4 (0, it
 * is up to its owner), video 1, layer 2.
 */
static int dt_video(enum display_format vfmt)
{
	struct display_pipeline_state s;
	struct display_mode m;
	uint32_t w, h, vx, vy, vw, vh, bw = 4, i, sq;
	struct dt_buf *v = &dt_hold_buf, *o = &dt_hold_buf2;
	static const uint32_t squares[3] = { 0xa0ff0000, 0xa000ff00, 0xa00000ff };
	int ret;

	if (dt_held || dt_held2) {
		T_ERR("already holding a plane, run 'off' first\n");
		return -EBUSY;
	}
	if (dt_mode(&m))
		return -ENODEV;
	w = m.width;
	h = m.height;
	vw = w / 2;
	vh = h / 2;
	vx = w / 4;
	vy = h / 4;
	ret = dt_alloc(v, vfmt, vw, vh);
	if (ret)
		return ret;
	ret = dt_alloc(o, DISPLAY_FORMAT_ARGB8888, w, h);
	if (ret) {
		dt_free(v);
		return ret;
	}

	dt_fill(v, false);

	dt_solid(o, 0x00000000);
	/* alpha ramp strip along the top over the background */
	for (i = 0; i < w; i++)
		dt_rect(o, i, 0, i + 1, h / 8,
			((i * 255 / (w - 1)) << 24) | 0x00ffffff);
	/* frame around the video window */
	dt_rect(o, vx - bw, vy - bw, vx + vw + bw, vy, 0xffffffff);
	dt_rect(o, vx - bw, vy + vh, vx + vw + bw, vy + vh + bw, 0xffffffff);
	dt_rect(o, vx - bw, vy, vx, vy + vh, 0xffffffff);
	dt_rect(o, vx + vw, vy, vx + vw + bw, vy + vh, 0xffffffff);
	/* translucent caption bar over the lower third of the video */
	dt_rect(o, vx, vy + vh * 2 / 3, vx + vw, vy + vh, 0xa0000000);
	sq = vh / 6;
	for (i = 0; i < 3; i++)
		dt_rect(o, vx + sq / 2 + i * (sq + sq / 2),
			vy + vh * 5 / 6 - sq / 2,
			vx + sq / 2 + i * (sq + sq / 2) + sq,
			vy + vh * 5 / 6 + sq / 2, squares[i]);
	dpy_os_dcache_clean(o->cpu, o->size);

	display_pipeline_state_init(&s);
	s.plane_count = 0;
	dt_plane(&s, 0, v, (int32_t)vx, (int32_t)vy, vw, vh, 1, 0xff);
	dt_plane(&s, 8, o, 0, 0, w, h, 2, 0xff);
	ret = display_submit_ex(&s, DISPLAY_SUBMIT_PARTIAL);
	if (ret) {
		T_ERR("video: submit failed: %d\n", ret);
		dt_free(o);
		dt_free(v);
		return ret;
	}
	dt_held = true;
	dt_held2 = true;
	dt_video_active = true;
	dpy_os_printf("video: plane 0 %s %ux%u at %u,%u (z1), plane 8 ARGB8888 "
		      "(z2); 'display_test off' to clear\n",
		      display_format_name(vfmt), vw, vh, vx, vy);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */
static int dt_info(void)
{
	struct display_mode modes[DISPLAY_MAX_MODES];
	struct display_plane_caps pc;
	struct display_caps caps;
	int n, i;
	uint32_t f;

	if (display_get_caps(&caps))
		return -ENODEV;
	dpy_os_printf("interface %s, panel %s, %ux%u mm, %u planes%s\n",
		      caps.interface ? caps.interface : "-",
		      caps.panel ? caps.panel : "-", caps.width_mm,
		      caps.height_mm, caps.plane_count,
		      caps.writeback ? ", writeback" : "");
	n = display_get_modes(modes, DISPLAY_MAX_MODES);
	for (i = 0; i < n; i++)
		dpy_os_printf("mode %d: %ux%u@%u clk %u kHz\n", i,
			      modes[i].width, modes[i].height,
			      modes[i].refresh_hz, modes[i].clock_khz);
	for (i = 0; i < (int)caps.plane_count; i++) {
		if (display_get_plane_caps(i, &pc))
			continue;
		dpy_os_printf("plane %u: channel %u layer %u%s%s, %u formats:",
			      pc.plane_id, pc.channel, pc.layer,
			      pc.flags & DISPLAY_PLANE_CAP_SCALE ? " scale" : "",
			      pc.flags & DISPLAY_PLANE_CAP_YUV ? " yuv" : "",
			      pc.format_count);
		for (f = 0; f < pc.format_count; f++)
			dpy_os_printf(" %s", display_format_name(pc.formats[f]));
		dpy_os_printf("\n");
	}
	return 0;
}

static enum display_format dt_format_by_name(const char *name)
{
	uint32_t f;

	for (f = 1; f < DISPLAY_FORMAT_COUNT; f++)
		if (!strcmp(display_format_name((enum display_format)f), name))
			return (enum display_format)f;
	return DISPLAY_FORMAT_INVALID;
}

static int dt_show(uint32_t plane, enum display_format fmt, uint32_t w,
		   uint32_t h, bool ramp)
{
	struct display_pipeline_state s;
	struct display_mode m;
	struct dt_buf b;
	int ret;

	if (dt_mode(&m))
		return -ENODEV;
	ret = dt_alloc(&b, fmt, w ? w : m.width, h ? h : m.height);
	if (ret)
		return ret;
	dt_fill(&b, ramp);
	display_pipeline_state_init(&s);
	s.plane_count = 0;
	dt_plane(&s, plane, &b, 0, 0, m.width, m.height, 0, 0xff);
	ret = dt_submit(&s, display_format_name(fmt));
	if (!ret)
		dpy_os_msleep(1000);
	dt_blank_planes();
	dt_free(&b);
	return ret;
}

static int dt_formats(uint32_t plane)
{
	struct display_plane_caps pc;
	uint32_t i;
	int fails = 0;

	if (display_get_plane_caps(plane, &pc))
		return -EINVAL;
	for (i = 0; i < pc.format_count; i++) {
		dpy_os_printf("plane %u: %s\n", plane,
			      display_format_name(pc.formats[i]));
		if (dt_show(plane, pc.formats[i], 0, 0, false))
			fails++;
	}
	dpy_os_printf("%u formats, %d failed\n", pc.format_count, fails);
	return fails ? -EIO : 0;
}

static int dt_scale(uint32_t plane)
{
	static const uint32_t pct[] = { 100, 75, 50, 25, 150 };
	struct display_pipeline_state s;
	struct display_mode m;
	struct dt_buf b;
	unsigned int i;
	int ret = 0;

	if (dt_mode(&m))
		return -ENODEV;
	if (dt_alloc(&b, DISPLAY_FORMAT_ARGB8888, 320, 240))
		return -ENOMEM;
	dt_fill(&b, false);
	for (i = 0; i < DPY_ARRAY_SIZE(pct); i++) {
		uint32_t w = m.width * pct[i] / 100, h = m.height * pct[i] / 100;

		display_pipeline_state_init(&s);
		s.plane_count = 0;
		/* larger than the screen: centred and clipped */
		dt_plane(&s, plane, &b, ((int32_t)m.width - (int32_t)w) / 2,
			 ((int32_t)m.height - (int32_t)h) / 2, w, h, 0, 0xff);
		dpy_os_printf("scale %u%%: 320x240 -> %ux%u\n", pct[i], w, h);
		ret |= dt_submit(&s, "scale");
		dpy_os_msleep(800);
	}
	dt_blank_planes();
	dt_free(&b);
	return ret;
}

static int dt_alpha(void)
{
	struct display_pipeline_state s;
	struct display_mode m;
	struct dt_buf bars, box;
	int ret;

	if (dt_mode(&m))
		return -ENODEV;
	if (dt_alloc(&bars, DISPLAY_FORMAT_XRGB8888, m.width, m.height))
		return -ENOMEM;
	if (dt_alloc(&box, DISPLAY_FORMAT_ARGB8888, m.width / 2, m.height / 2)) {
		dt_free(&bars);
		return -ENOMEM;
	}
	dt_fill(&bars, false);

	/* plane alpha: opaque red at 50 % */
	dt_solid(&box, 0xffff0000);
	display_pipeline_state_init(&s);
	s.plane_count = 0;
	dt_plane(&s, 0, &bars, 0, 0, m.width, m.height, 0, 0xff);
	dt_plane(&s, 4, &box, m.width / 4, m.height / 4, m.width / 2,
		 m.height / 2, 1, 0x80);
	ret = dt_submit(&s, "plane alpha");
	dpy_os_msleep(1500);

	/* pixel alpha: ramp from transparent to opaque */
	dt_fill(&box, true);
	s.planes[1].alpha = 0xff;
	ret |= dt_submit(&s, "pixel alpha");
	dpy_os_msleep(1500);

	dt_blank_planes();
	dt_free(&box);
	dt_free(&bars);
	return ret;
}

static int dt_zorder(void)
{
	static const uint32_t colors[3] = { 0xffff0000, 0xff00ff00, 0xff0000ff };
	static const uint32_t planes[3] = { 0, 4, 8 };	/* vi0, ui0, ui1 */
	struct display_pipeline_state s;
	struct display_mode m;
	struct dt_buf b[3];
	uint32_t w, h, i, r;
	int ret = 0;

	if (dt_mode(&m))
		return -ENODEV;
	w = m.width / 2;
	h = m.height / 2;
	for (i = 0; i < 3; i++) {
		if (dt_alloc(&b[i], DISPLAY_FORMAT_XRGB8888, w, h)) {
			while (i--)
				dt_free(&b[i]);
			return -ENOMEM;
		}
		dt_solid(&b[i], colors[i]);
	}
	for (r = 0; r < 3; r++) {
		display_pipeline_state_init(&s);
		s.plane_count = 0;
		for (i = 0; i < 3; i++)
			dt_plane(&s, planes[i], &b[i], (int32_t)(i * w / 2),
				 (int32_t)(i * h / 2), w, h,
				 (uint8_t)((i + r) % 3), 0xff);
		dpy_os_printf("z order rotation %u\n", r);
		ret |= dt_submit(&s, "zorder");
		dpy_os_msleep(1000);
	}
	dt_blank_planes();
	for (i = 0; i < 3; i++)
		dt_free(&b[i]);
	return ret;
}

static int dt_capture(void)
{
	struct display_capture_req req;
	struct display_mode m;
	struct dt_buf b;
	uint32_t sum = 0, i, *px;
	int ret;

	if (dt_mode(&m))
		return -ENODEV;
	if (dt_alloc(&b, DISPLAY_FORMAT_ARGB8888, m.width, m.height))
		return -ENOMEM;
	memset(b.cpu, 0x5a, b.size);
	dpy_os_dcache_clean(b.cpu, b.size);

	memset(&req, 0, sizeof(req));
	req.framebuffer = b.fb;
	req.timeout_ms = 500;
	ret = display_capture(&req);
	if (ret) {
		T_ERR("capture failed: %d\n", ret);
		goto out;
	}
	dpy_os_dcache_invalidate(b.cpu, b.size);
	px = b.cpu;
	for (i = 0; i < m.width * m.height; i++)
		sum = sum * 31 + px[i];
	dpy_os_printf("captured %ux%u, checksum %08x, first pixels %08x %08x %08x\n",
		      m.width, m.height, sum, px[0], px[m.width / 2],
		      px[m.width * m.height - 1]);
out:
	dt_free(&b);
	return ret;
}

static int dt_gamma(uint32_t g100)
{
	static uint16_t lut[3 * 256];
	uint32_t i;

	if (!g100 || g100 == 100)
		return display_set_gamma(NULL, 0);
	for (i = 0; i < 256; i++) {
		/* a quadratic blend between x and x^2 is enough for a test */
		uint32_t x = i * 257;
		uint32_t sq = (uint32_t)(((uint64_t)x * x) >> 16);
		uint32_t v;

		if (g100 > 100)	/* brighten midtones */
			v = x + (x - sq) * (g100 - 100) / 100;
		else		/* darken midtones */
			v = x - (x - sq) * (100 - g100) / 100;
		if (v > 65535)
			v = 65535;
		lut[i] = lut[256 + i] = lut[512 + i] = (uint16_t)v;
	}
	return display_set_gamma(lut, 256);
}

static int dt_stats(void)
{
	struct display_stats st;

	if (display_get_stats(&st))
		return -ENODEV;
	dpy_os_printf("vblank %u, %u.%03u Hz, commits %u, timeouts %u, underflow %u, last commit %u us\n",
		      (uint32_t)st.vblank_count, st.refresh_mhz / 1000,
		      st.refresh_mhz % 1000, st.commit_count, st.commit_timeout,
		      st.fifo_underflow, st.last_commit_us);
	return 0;
}

int display_test_main(int argc, char **argv)
{
	const char *cmd = argc > 0 ? argv[0] : "info";

	if (!strcmp(cmd, "info"))
		return dt_info();
	if (!strcmp(cmd, "pattern") && argc > 1)
		return display_set_test_pattern(strtoul(argv[1], NULL, 0));
	if (!strcmp(cmd, "fill") && argc > 2) {
		enum display_format f = dt_format_by_name(argv[2]);

		if (f == DISPLAY_FORMAT_INVALID) {
			T_ERR("unknown format %s\n", argv[2]);
			return -EINVAL;
		}
		return dt_show(strtoul(argv[1], NULL, 0), f,
			       argc > 4 ? strtoul(argv[3], NULL, 0) : 0,
			       argc > 4 ? strtoul(argv[4], NULL, 0) : 0, false);
	}
	if (!strcmp(cmd, "hold") && argc > 2) {
		enum display_format f = dt_format_by_name(argv[2]);

		if (f == DISPLAY_FORMAT_INVALID) {
			T_ERR("unknown format %s\n", argv[2]);
			return -EINVAL;
		}
		return dt_hold(strtoul(argv[1], NULL, 0), f,
			       argc > 4 ? strtoul(argv[3], NULL, 0) : 0,
			       argc > 4 ? strtoul(argv[4], NULL, 0) : 0);
	}
	if (!strcmp(cmd, "overlay")) {
		enum display_format f = argc > 1 ? dt_format_by_name(argv[1])
						 : DISPLAY_FORMAT_NV12;

		if (f == DISPLAY_FORMAT_INVALID) {
			T_ERR("unknown format %s\n", argv[1]);
			return -EINVAL;
		}
		return dt_overlay(f);
	}
	if (!strcmp(cmd, "video")) {
		enum display_format f = argc > 1 ? dt_format_by_name(argv[1])
						 : DISPLAY_FORMAT_NV12;

		if (f == DISPLAY_FORMAT_INVALID) {
			T_ERR("unknown format %s\n", argv[1]);
			return -EINVAL;
		}
		return dt_video(f);
	}
	if (!strcmp(cmd, "formats") && argc > 1)
		return dt_formats(strtoul(argv[1], NULL, 0));
	if (!strcmp(cmd, "scale") && argc > 1)
		return dt_scale(strtoul(argv[1], NULL, 0));
	if (!strcmp(cmd, "alpha"))
		return dt_alpha();
	if (!strcmp(cmd, "zorder"))
		return dt_zorder();
	if (!strcmp(cmd, "capture"))
		return dt_capture();
	if (!strcmp(cmd, "bl") && argc > 1)
		return display_set_backlight(strtoul(argv[1], NULL, 0));
	if (!strcmp(cmd, "bcsh") && argc > 4) {
		struct display_color_adjust a = {
			.brightness = (uint8_t)strtoul(argv[1], NULL, 0),
			.contrast = (uint8_t)strtoul(argv[2], NULL, 0),
			.saturation = (uint8_t)strtoul(argv[3], NULL, 0),
			.hue = (uint8_t)strtoul(argv[4], NULL, 0),
		};

		return display_set_color_adjust(&a);
	}
	if (!strcmp(cmd, "gamma") && argc > 1)
		return dt_gamma(strtoul(argv[1], NULL, 0));
	if (!strcmp(cmd, "bg") && argc > 1)
		return display_set_background(strtoul(argv[1], NULL, 16));
	if (!strcmp(cmd, "off")) {
		dt_unhold();
		return 0;
	}
	if (!strcmp(cmd, "blank") && argc > 1)
		return display_blank(strtoul(argv[1], NULL, 0) != 0);
	if (!strcmp(cmd, "stats"))
		return dt_stats();

	dpy_os_printf("usage: display_test info|pattern <n>|fill <plane> <fmt> [w h]|formats <plane>|\n"
		      "       hold <plane> <fmt> [w h]|overlay [fmt]|video [fmt]|scale <plane>|alpha|\n"
		      "       zorder|\n"
		      "       capture|bl <n>|\n"
		      "       bcsh <b> <c> <s> <h>|gamma <x100>|bg <rrggbb>|off|blank <0|1>|stats\n");
	return -EINVAL;
}
