/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

/*
 * Zephyr display driver API on top of the Allwinner display-engine stack.
 *
 * One UI plane of the display engine scans out a framebuffer in SRAM/PSRAM;
 * display_write() copies pixels into it and cleans the data cache.
 */

#define DT_DRV_COMPAT allwinner_sunxi_display

#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/display_sunxi.h>
#include <zephyr/logging/log.h>

#include <hal/display/display_engine.h>

#include "dt_graph.h"

LOG_MODULE_REGISTER(display_sunxi, CONFIG_DISPLAY_LOG_LEVEL);

/* UI channel 0, layer 0 (see the plane table in display_engine.h) */
#define SUNXI_FB_PLANE		4
/* the framebuffer is sized from the devicetree node; it must match the panel */
#define SUNXI_MAX_WIDTH		DT_INST_PROP(0, width)
#define SUNXI_MAX_HEIGHT	DT_INST_PROP(0, height)

#if defined(CONFIG_DISPLAY_SUNXI_ARGB8888)
#define SUNXI_PIXEL_FORMAT	PIXEL_FORMAT_ARGB_8888
#define SUNXI_DISPLAY_FORMAT	DISPLAY_FORMAT_ARGB8888
#define SUNXI_BPP		4
#else
#define SUNXI_PIXEL_FORMAT	PIXEL_FORMAT_RGB_565
#define SUNXI_DISPLAY_FORMAT	DISPLAY_FORMAT_RGB565
#define SUNXI_BPP		2
#endif

static uint8_t sunxi_fb[SUNXI_MAX_WIDTH * SUNXI_MAX_HEIGHT * SUNXI_BPP]
	__aligned(64);

struct sunxi_display_data {
	uint16_t width;
	uint16_t height;
	uint32_t stride;
	uint8_t brightness;
	bool blanked;
	/* plane that scans out YCbCr pictures, -1 when there is none */
	int video_plane;
};

static int sunxi_display_write(const struct device *dev, const uint16_t x,
			       const uint16_t y,
			       const struct display_buffer_descriptor *desc,
			       const void *buf)
{
	struct sunxi_display_data *data = dev->data;
	const uint8_t *src = buf;
	uint8_t *dst;
	size_t row_bytes = (size_t)desc->width * SUNXI_BPP;
	size_t src_pitch = (size_t)desc->pitch * SUNXI_BPP;
	uint16_t row;

	if (desc->width > desc->pitch || x + desc->width > data->width ||
	    y + desc->height > data->height) {
		return -EINVAL;
	}

	for (row = 0; row < desc->height; row++) {
		dst = sunxi_fb + (size_t)(y + row) * data->stride +
		      (size_t)x * SUNXI_BPP;
		memcpy(dst, src + row * src_pitch, row_bytes);
		sys_cache_data_flush_range(dst, row_bytes);
	}

	return 0;
}

static int sunxi_display_blanking(const struct device *dev, bool on)
{
	struct sunxi_display_data *data = dev->data;
	int ret = display_blank(on);

	if (ret == 0) {
		data->blanked = on;
	}
	return ret;
}

static int sunxi_display_blanking_on(const struct device *dev)
{
	return sunxi_display_blanking(dev, true);
}

static int sunxi_display_blanking_off(const struct device *dev)
{
	return sunxi_display_blanking(dev, false);
}

static int sunxi_display_set_brightness(const struct device *dev,
					const uint8_t brightness)
{
	struct sunxi_display_data *data = dev->data;
	int ret = display_set_backlight(brightness);

	if (ret == 0) {
		data->brightness = brightness;
	}
	return ret;
}

static void sunxi_display_get_capabilities(const struct device *dev,
					   struct display_capabilities *caps)
{
	const struct sunxi_display_data *data = dev->data;

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = data->width;
	caps->y_resolution = data->height;
	caps->supported_pixel_formats = SUNXI_PIXEL_FORMAT;
	caps->current_pixel_format = SUNXI_PIXEL_FORMAT;
	caps->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static int sunxi_display_show_framebuffer(struct sunxi_display_data *data)
{
	struct display_pipeline_state state;
	struct display_plane_state *p;

	display_pipeline_state_init(&state);
	state.plane_count = 0;
	p = &state.planes[state.plane_count++];
	memset(p, 0, sizeof(*p));
	p->enable = true;
	p->plane_id = SUNXI_FB_PLANE;
	p->alpha = 0xff;
	p->blend_mode = DISPLAY_BLEND_NONE;
	p->framebuffer.address = (uintptr_t)sunxi_fb;
	p->framebuffer.plane_address[0] = (uintptr_t)sunxi_fb;
	p->framebuffer.plane_stride[0] = data->stride;
	p->framebuffer.plane_count = 1;
	p->framebuffer.format = SUNXI_DISPLAY_FORMAT;
	p->framebuffer.width = data->width;
	p->framebuffer.height = data->height;
	p->framebuffer.stride = data->stride;
	p->destination.width = data->width;
	p->destination.height = data->height;

	return display_submit(&state);
}

/* The first plane that takes YCbCr and scales; it is not the frame buffer plane */
static int sunxi_display_find_video_plane(void)
{
	struct display_caps caps;
	struct display_plane_caps pc;

	if (display_get_caps(&caps) != 0) {
		return -1;
	}
	for (uint32_t id = 0; id < caps.plane_count; id++) {
		if (id == SUNXI_FB_PLANE || display_get_plane_caps(id, &pc) != 0) {
			continue;
		}
		if ((pc.flags & DISPLAY_PLANE_CAP_YUV) && (pc.flags & DISPLAY_PLANE_CAP_SCALE)) {
			return id;
		}
	}

	return -1;
}

static void sunxi_display_fb_plane(struct sunxi_display_data *data, struct display_plane_state *p,
				   bool enable)
{
	memset(p, 0, sizeof(*p));
	p->enable = enable;
	p->plane_id = SUNXI_FB_PLANE;
	p->alpha = 0xff;
	p->blend_mode = DISPLAY_BLEND_NONE;
	p->framebuffer.address = (uintptr_t)sunxi_fb;
	p->framebuffer.plane_address[0] = (uintptr_t)sunxi_fb;
	p->framebuffer.plane_stride[0] = data->stride;
	p->framebuffer.plane_count = 1;
	p->framebuffer.format = SUNXI_DISPLAY_FORMAT;
	p->framebuffer.width = data->width;
	p->framebuffer.height = data->height;
	p->framebuffer.stride = data->stride;
	p->destination.width = data->width;
	p->destination.height = data->height;
}

int display_sunxi_show_yuv(const struct device *dev, const struct display_sunxi_yuv *img)
{
	struct sunxi_display_data *data = dev->data;
	struct display_pipeline_state state;
	struct display_plane_state *v, *fb;
	uint32_t dw, dh;

	if (data->video_plane < 0) {
		return -ENOTSUP;
	}

	/* largest size that fits the screen and keeps the shape of the picture */
	if ((uint64_t)data->width * img->height <= (uint64_t)data->height * img->width) {
		dw = data->width;
		dh = (uint64_t)img->height * data->width / img->width;
	} else {
		dh = data->height;
		dw = (uint64_t)img->width * data->height / img->height;
	}
	dw &= ~1U;
	dh &= ~1U;

	display_pipeline_state_init(&state);
	state.plane_count = 2;
	fb = &state.planes[0];
	sunxi_display_fb_plane(data, fb, false);
	v = &state.planes[1];
	memset(v, 0, sizeof(*v));
	v->enable = true;
	v->plane_id = data->video_plane;
	v->alpha = 0xff;
	v->blend_mode = DISPLAY_BLEND_NONE;
	v->color_encoding = img->bt709 ? DISPLAY_COLOR_BT709 : DISPLAY_COLOR_BT601;
	v->color_range = img->full_range ? DISPLAY_RANGE_FULL : DISPLAY_RANGE_LIMITED;
	v->framebuffer.address = (uintptr_t)img->y;
	v->framebuffer.plane_address[0] = (uintptr_t)img->y;
	v->framebuffer.plane_address[1] = (uintptr_t)img->uv;
	v->framebuffer.plane_stride[0] = img->stride_y;
	v->framebuffer.plane_stride[1] = img->stride_uv;
	v->framebuffer.plane_count = 2;
	v->framebuffer.format = img->nv21 ? DISPLAY_FORMAT_NV21 : DISPLAY_FORMAT_NV12;
	v->framebuffer.width = img->width;
	v->framebuffer.height = img->height;
	v->framebuffer.stride = img->stride_y;
	v->source.width = img->width;
	v->source.height = img->height;
	v->destination.x = (data->width - dw) / 2;
	v->destination.y = (data->height - dh) / 2;
	v->destination.width = dw;
	v->destination.height = dh;

	return display_submit_ex(&state, DISPLAY_SUBMIT_PARTIAL);
}

int display_sunxi_hide_yuv(const struct device *dev)
{
	struct sunxi_display_data *data = dev->data;
	struct display_pipeline_state state;

	if (data->video_plane < 0) {
		return -ENOTSUP;
	}
	display_pipeline_state_init(&state);
	state.plane_count = 2;
	sunxi_display_fb_plane(data, &state.planes[0], true);
	memset(&state.planes[1], 0, sizeof(state.planes[1]));
	state.planes[1].plane_id = data->video_plane;
	state.planes[1].enable = false;

	return display_submit_ex(&state, DISPLAY_SUBMIT_PARTIAL);
}

static int sunxi_display_init(const struct device *dev)
{
	struct sunxi_display_data *data = dev->data;
	struct display_mode mode;
	int ret;

	ret = dpy_dt_prepare();
	if (ret) {
		LOG_ERR("bad command sequence in the devicetree: %d", ret);
		return ret;
	}
	ret = display_probe();
	if (ret) {
		LOG_ERR("display_probe failed: %d", ret);
		return ret;
	}
	ret = display_wait_ready(CONFIG_DISPLAY_SUNXI_READY_TIMEOUT_MS);
	if (ret) {
		LOG_ERR("display not ready: %d", ret);
		return ret;
	}
	ret = display_get_mode(&mode);
	if (ret) {
		LOG_ERR("no display mode: %d", ret);
		return ret;
	}
	if (mode.width > SUNXI_MAX_WIDTH || mode.height > SUNXI_MAX_HEIGHT) {
		LOG_ERR("mode %ux%u exceeds the framebuffer %ux%u", mode.width,
			mode.height, SUNXI_MAX_WIDTH, SUNXI_MAX_HEIGHT);
		return -ENOMEM;
	}

	data->width = mode.width;
	data->height = mode.height;
	data->stride = mode.width * SUNXI_BPP;
	memset(sunxi_fb, 0, sizeof(sunxi_fb));
	sys_cache_data_flush_range(sunxi_fb, sizeof(sunxi_fb));

	ret = sunxi_display_show_framebuffer(data);
	if (ret) {
		LOG_ERR("cannot show the framebuffer: %d", ret);
		return ret;
	}

	data->video_plane = sunxi_display_find_video_plane();

	LOG_INF("%ux%u @ %u Hz, fb %p, video plane %d", mode.width, mode.height,
		mode.refresh_hz, (void *)sunxi_fb, data->video_plane);
	return 0;
}

static DEVICE_API(display, sunxi_display_api) = {
	.blanking_on = sunxi_display_blanking_on,
	.blanking_off = sunxi_display_blanking_off,
	.write = sunxi_display_write,
	.set_brightness = sunxi_display_set_brightness,
	.get_capabilities = sunxi_display_get_capabilities,
};

static struct sunxi_display_data sunxi_display_data0;

DEVICE_DT_INST_DEFINE(0, sunxi_display_init, NULL, &sunxi_display_data0, NULL,
		      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY,
		      &sunxi_display_api);
