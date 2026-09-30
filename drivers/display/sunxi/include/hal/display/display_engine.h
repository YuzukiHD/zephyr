/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Native API of the display-engine/tcon display stack.
 *
 * The API is built on the atomic pipeline model of the driver: a caller
 * describes the complete wanted state of a display (mode, active, planes)
 * and submits it at once; the driver validates it against the hardware
 * constraints and applies it on the next vertical blank.
 *
 * Plane ids enumerate the hardware layers of the display engine. Use
 * display_get_plane_caps() to discover which formats and features each
 * plane supports; on sun252iw2 they are:
 *   0..3   video channel layers (RGB + YUV, scaler)
 *   4..7   UI channel 0 layers (RGB, scaler)
 *   8..11  UI channel 1 layers (RGB, no scaler)
 * Layers of the same channel share the channel scaler and are stacked
 * together in z order.
 */
#ifndef __DISPLAY_ENGINE_H__
#define __DISPLAY_ENGINE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_MAX_PLANES	12
#define DISPLAY_MAX_FB_PLANES	3
#define DISPLAY_MAX_MODES	8
#define DISPLAY_BACKLIGHT_MAX	255

/*
 * Pixel formats. Packed RGB names give the component order from the most
 * to the least significant bit of a little-endian pixel word (DRM fourcc
 * convention): ARGB8888 is stored in memory as B, G, R, A.
 * Packed/semi-planar YUV names give the memory byte order (DRM convention):
 * NV12 is a Y plane followed by an interleaved U, V plane.
 */
enum display_format {
	DISPLAY_FORMAT_INVALID = 0,

	DISPLAY_FORMAT_ARGB8888,
	DISPLAY_FORMAT_ABGR8888,
	DISPLAY_FORMAT_RGBA8888,
	DISPLAY_FORMAT_BGRA8888,
	DISPLAY_FORMAT_XRGB8888,
	DISPLAY_FORMAT_XBGR8888,
	DISPLAY_FORMAT_RGBX8888,
	DISPLAY_FORMAT_BGRX8888,
	DISPLAY_FORMAT_RGB888,
	DISPLAY_FORMAT_BGR888,
	DISPLAY_FORMAT_RGB565,
	DISPLAY_FORMAT_BGR565,
	DISPLAY_FORMAT_ARGB4444,
	DISPLAY_FORMAT_ABGR4444,
	DISPLAY_FORMAT_RGBA4444,
	DISPLAY_FORMAT_BGRA4444,
	DISPLAY_FORMAT_ARGB1555,
	DISPLAY_FORMAT_ABGR1555,
	DISPLAY_FORMAT_RGBA5551,
	DISPLAY_FORMAT_BGRA5551,

	/* packed YUV 4:2:2 */
	DISPLAY_FORMAT_YUYV,
	DISPLAY_FORMAT_YVYU,
	DISPLAY_FORMAT_UYVY,
	DISPLAY_FORMAT_VYUY,
	/* semi-planar YUV */
	DISPLAY_FORMAT_NV12,		/* 4:2:0, U then V */
	DISPLAY_FORMAT_NV21,		/* 4:2:0, V then U */
	DISPLAY_FORMAT_NV16,		/* 4:2:2, U then V */
	DISPLAY_FORMAT_NV61,		/* 4:2:2, V then U */
	DISPLAY_FORMAT_NV411,		/* 4:1:1, U then V */
	DISPLAY_FORMAT_NV114,		/* 4:1:1, V then U */
	/* three plane YUV */
	DISPLAY_FORMAT_YUV420,		/* I420: Y, U, V planes */
	DISPLAY_FORMAT_YVU420,		/* YV12: Y, V, U planes */
	DISPLAY_FORMAT_YUV422,
	DISPLAY_FORMAT_YUV411,

	DISPLAY_FORMAT_COUNT
};

enum display_blend_mode {
	/* straight (non pre-multiplied) alpha; the default */
	DISPLAY_BLEND_COVERAGE = 0,
	/* colour components are pre-multiplied by alpha */
	DISPLAY_BLEND_PREMULTIPLIED,
	/* ignore the pixel alpha, only the plane alpha is used */
	DISPLAY_BLEND_NONE,
};

enum display_color_encoding {
	DISPLAY_COLOR_BT601 = 0,
	DISPLAY_COLOR_BT709,
};

enum display_color_range {
	DISPLAY_RANGE_LIMITED = 0,	/* 16..235 */
	DISPLAY_RANGE_FULL,		/* 0..255 */
};

#define DISPLAY_MODE_FLAG_NHSYNC	(1U << 0)	/* hsync active low */
#define DISPLAY_MODE_FLAG_NVSYNC	(1U << 1)	/* vsync active low */
#define DISPLAY_MODE_FLAG_INTERLACE	(1U << 2)
#define DISPLAY_MODE_FLAG_PREFERRED	(1U << 8)

struct display_rect {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
};

struct display_mode {
	uint32_t width;
	uint32_t height;
	uint32_t htotal;
	uint32_t vtotal;
	uint32_t hsync_start;
	uint32_t hsync_end;
	uint32_t vsync_start;
	uint32_t vsync_end;
	uint32_t clock_khz;
	uint32_t refresh_hz;
	uint32_t flags;			/* DISPLAY_MODE_FLAG_* */
};

struct display_framebuffer {
	/* shortcut for plane_address[0] when plane_address[0] is 0 */
	uintptr_t address;
	uintptr_t plane_address[DISPLAY_MAX_FB_PLANES];
	/* bytes per line of each plane; 0 derives it from width/stride */
	uint32_t plane_stride[DISPLAY_MAX_FB_PLANES];
	uint32_t plane_size[DISPLAY_MAX_FB_PLANES];
	uint32_t plane_count;
	enum display_format format;
	uint32_t width;
	uint32_t height;
	/* bytes per line of plane 0 when plane_stride[0] is 0 */
	uint32_t stride;
};

struct display_plane_state {
	bool enable;
	uint32_t plane_id;
	/* plane opacity, 0 = transparent, 255 = opaque */
	uint8_t alpha;
	/* stacking order across all planes, higher is on top */
	uint8_t zpos;
	uint8_t blend_mode;		/* enum display_blend_mode */
	uint8_t color_encoding;		/* enum display_color_encoding, YUV only */
	uint8_t color_range;		/* enum display_color_range, YUV only */
	struct display_framebuffer framebuffer;
	/* part of the framebuffer to show, zero size means the whole buffer */
	struct display_rect source;
	/* position on screen, zero size means the whole screen */
	struct display_rect destination;
};

struct display_pipeline_state {
	/* zero width/height keeps the current mode */
	struct display_mode mode;
	bool active;
	/* colour shown where no plane covers the screen, 0xAARRGGBB */
	uint32_t background_argb;
	uint32_t plane_count;
	struct display_plane_state planes[DISPLAY_MAX_PLANES];
};

/* flags for display_submit_ex() */
#define DISPLAY_SUBMIT_TEST_ONLY	(1U << 0)	/* check, do not apply */
#define DISPLAY_SUBMIT_PARTIAL		(1U << 1)	/* only touch listed planes */
#define DISPLAY_SUBMIT_NONBLOCK		(1U << 2)	/* do not wait for vblank */

#define DISPLAY_PLANE_CAP_SCALE		(1U << 0)
#define DISPLAY_PLANE_CAP_YUV		(1U << 1)
#define DISPLAY_PLANE_CAP_ALPHA		(1U << 2)

struct display_plane_caps {
	uint32_t plane_id;
	uint32_t channel;		/* planes of a channel share a scaler */
	uint32_t layer;			/* index inside the channel */
	uint32_t flags;			/* DISPLAY_PLANE_CAP_* */
	uint32_t max_width;
	uint32_t max_height;
	uint32_t format_count;
	enum display_format formats[DISPLAY_FORMAT_COUNT];
};

struct display_caps {
	uint32_t plane_count;
	uint32_t mode_count;
	uint32_t width_mm;
	uint32_t height_mm;
	const char *interface;		/* "rgb", "lvds", "dsi" */
	const char *panel;
	bool writeback;
};

struct display_color_adjust {
	/* 0..100, 50 is neutral */
	uint8_t brightness;
	uint8_t contrast;
	uint8_t saturation;
	uint8_t hue;
};

struct display_capture_req {
	struct display_framebuffer framebuffer;	/* ARGB8888/RGB888/NV12/NV21/YUV420 */
	struct display_rect source;		/* screen area, zero size = all */
	struct display_rect destination;	/* area of the buffer, zero = all */
	uint32_t timeout_ms;
};

struct display_stats {
	uint64_t vblank_count;
	uint32_t commit_count;
	uint32_t commit_timeout;
	uint32_t fifo_underflow;
	uint32_t last_commit_us;
	uint32_t refresh_mhz;		/* measured refresh rate, milli-Hz */
};

/* bring the display stack up; called once at boot */
int display_probe(void);
int display_remove(void);
/* wait until the display is bound and the boot modeset is done */
int display_wait_ready(uint32_t timeout_ms);

int display_open(void);
int display_close(void);

int display_get_caps(struct display_caps *caps);
int display_get_plane_caps(uint32_t plane_id, struct display_plane_caps *caps);
int display_get_mode(struct display_mode *mode);
/* returns the number of modes written to @modes */
int display_get_modes(struct display_mode *modes, uint32_t max);

/* fill @state with the current state (all planes, current mode) */
int display_get_state(struct display_pipeline_state *state);
/* reset @state to "nothing shown" defaults (alpha 255, full rects) */
void display_pipeline_state_init(struct display_pipeline_state *state);

int display_submit(const struct display_pipeline_state *state);
int display_submit_ex(const struct display_pipeline_state *state,
		      uint32_t flags);

int display_wait_vsync(uint32_t timeout_ms);
int display_blank(bool blank);

int display_set_backlight(uint32_t level);
int display_get_backlight(uint32_t *level);
int display_set_color_adjust(const struct display_color_adjust *adj);
int display_get_color_adjust(struct display_color_adjust *adj);
/* @lut is 3 * @entries values (R then G then B), 16 bit each; NULL disables */
int display_set_gamma(const uint16_t *lut, uint32_t entries);
int display_set_background(uint32_t argb);
/* show a built in test pattern generated by the timing controller */
int display_set_test_pattern(uint32_t pattern);

int display_capture(const struct display_capture_req *req);

int display_get_stats(struct display_stats *stats);

/* bytes per pixel of plane @plane of @format, 0 for unknown formats */
uint32_t display_format_cpp(enum display_format format, uint32_t plane);
const char *display_format_name(enum display_format format);

#ifdef __cplusplus
}
#endif

#endif /* __DISPLAY_ENGINE_H__ */
