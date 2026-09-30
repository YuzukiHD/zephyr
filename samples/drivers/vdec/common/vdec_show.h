/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Put a decoded frame on the screen: the 2D accelerator converts it to the
 * display format, scales it up by an integer factor to fit and centres it on a
 * black screen, then the picture is written to the display.
 */

#ifndef SAMPLES_DRIVERS_VDEC_COMMON_VDEC_SHOW_H_
#define SAMPLES_DRIVERS_VDEC_COMMON_VDEC_SHOW_H_

#include <errno.h>
#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/g2d.h>
#include <zephyr/drivers/vdec.h>
#include <zephyr/sys/printk.h>

static inline int vdec_show(const struct vdec_frame *frame, bool blend_over_black)
{
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	const struct device *g2d = DEVICE_DT_GET_ANY(allwinner_sunxi_g2d);
	struct display_capabilities caps;
	struct g2d_surface src = {0}, dst = {0};
	struct g2d_rect srect, drect, full;
	enum g2d_format dst_fmt;
	unsigned int bpp, scale;
	uint32_t flags = 0;
	void *screen;
	int ret;

	if (!device_is_ready(disp) || g2d == NULL || !device_is_ready(g2d)) {
		return -ENODEV;
	}
	display_get_capabilities(disp, &caps);
	if (caps.current_pixel_format == PIXEL_FORMAT_ARGB_8888) {
		dst_fmt = G2D_PIXFMT_ARGB8888;
		bpp = 4;
	} else {
		dst_fmt = G2D_PIXFMT_RGB565;
		bpp = 2;
	}

	scale = MAX(1, MIN(caps.x_resolution / frame->width, caps.y_resolution / frame->height));
	full = (struct g2d_rect){0, 0, caps.x_resolution, caps.y_resolution};
	srect = (struct g2d_rect){0, 0, frame->width, frame->height};
	drect = (struct g2d_rect){(caps.x_resolution - frame->width * scale) / 2,
				  (caps.y_resolution - frame->height * scale) / 2,
				  frame->width * scale, frame->height * scale};

	screen = aligned_alloc(64, ROUND_UP((size_t)caps.x_resolution * caps.y_resolution * bpp, 64));
	if (screen == NULL) {
		return -ENOMEM;
	}
	dst.format = dst_fmt;
	dst.width = caps.x_resolution;
	dst.height = caps.y_resolution;
	dst.plane[0] = screen;
	dst.pitch[0] = caps.x_resolution * bpp;

	if (frame->format == VDEC_FORMAT_RGBA8888) {
		/* R, G, B, A bytes in memory are the word 0xAABBGGRR */
		src.format = G2D_PIXFMT_ABGR8888;
		src.plane[0] = frame->plane[0];
		src.pitch[0] = frame->stride[0];
		src.width = frame->stride[0] / 4;
	} else {
		src.format = frame->format == VDEC_FORMAT_NV21 ? G2D_PIXFMT_NV21 : G2D_PIXFMT_NV12;
		src.plane[0] = frame->plane[0];
		src.plane[1] = frame->plane[1];
		src.pitch[0] = frame->stride[0];
		src.pitch[1] = frame->stride[1];
		src.width = frame->stride[0];
		flags |= G2D_FLAG_YUV_FULL_RANGE;
	}
	src.height = frame->height;

	ret = g2d_fill(g2d, &dst, &full, 0xff000000);
	if (ret == 0) {
		ret = g2d_blit(g2d, &src, &srect, &dst, &drect, G2D_ROTATE_0, flags);
	}
	if (ret != 0) {
		printk("2D accelerator failed: %d\n", ret);
		free(screen);
		return ret;
	}

	{
		struct display_buffer_descriptor desc = {
			.width = caps.x_resolution,
			.height = caps.y_resolution,
			.pitch = caps.x_resolution,
			.buf_size = caps.x_resolution * caps.y_resolution * bpp,
		};

		ret = display_write(disp, 0, 0, &desc, screen);
	}
	free(screen);
	display_blanking_off(disp);

	return ret;
}

#endif /* SAMPLES_DRIVERS_VDEC_COMMON_VDEC_SHOW_H_ */
