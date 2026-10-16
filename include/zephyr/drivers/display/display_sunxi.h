/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Extensions of the sunxi display driver
 *
 * The display engine has video planes that scan out YCbCr pictures directly
 * and scale them in hardware. These calls use one of them to show a decoded
 * picture without converting it to RGB first. While a picture is shown the
 * frame buffer plane that display_write() draws on is hidden, except with
 * CONFIG_DISPLAY_SUNXI_ARGB8888: then it stays on top and is blended with the
 * picture by the alpha of its pixels, which makes it an overlay (a pixel with
 * alpha 0 shows the picture).
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_DISPLAY_DISPLAY_SUNXI_H_
#define ZEPHYR_INCLUDE_DRIVERS_DISPLAY_DISPLAY_SUNXI_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The frame buffer that the display engine scans out and display_write() draws on. A caller that
 * changes it directly (the 2D accelerator moving a part of the picture) has to keep the data cache
 * coherent: write back before the accelerator reads it, drop the lines of what it wrote.
 *
 * @param stride_bytes gets the bytes from one row to the next (may be NULL)
 */
void *display_sunxi_framebuffer(const struct device *dev, uint32_t *stride_bytes);

/**
 * Waits for the start of the next vertical blank. A change of the frame buffer that begins then and
 * goes through it from the top faster than the display reads it is never seen half done.
 *
 * @return 0, or -ETIMEDOUT / -ENODEV
 */
int display_sunxi_wait_vsync(const struct device *dev, uint32_t timeout_ms);

/** A YCbCr 4:2:0 picture with an interleaved chroma plane */
struct display_sunxi_yuv {
	/** Luma plane */
	const void *y;
	/** Chroma plane */
	const void *uv;
	/** The chroma plane holds Cr before Cb (NV21) instead of Cb before Cr (NV12) */
	bool nv21;
	/** Picture size in pixels */
	uint16_t width;
	uint16_t height;
	/** Bytes from one row to the next in the luma and chroma plane */
	uint16_t stride_y;
	uint16_t stride_uv;
	/**
	 * Part of the picture to show (zoom); all zero shows the whole picture. The window is
	 * scaled to the screen keeping its shape. For YCbCr the values are rounded to even numbers.
	 */
	uint16_t crop_x;
	uint16_t crop_y;
	uint16_t crop_width;
	uint16_t crop_height;
	/** Samples cover 0..255 instead of 16..235 (JPEG) */
	bool full_range;
	/** BT.709 matrix instead of BT.601 */
	bool bt709;
	/**
	 * Return at once instead of waiting for the picture to appear (the next
	 * vertical blank). The previous picture is then scanned out until that
	 * moment, so its memory must be kept for one more refresh.
	 */
	bool nonblock;
};

/**
 * @brief Show a picture, scaled to fill the screen as far as its aspect ratio allows
 *
 * The memory must stay valid, and unchanged, until the next call or until
 * display_sunxi_hide_yuv(). Its data cache lines must be clean.
 *
 * @retval 0 on success
 * @retval -ENOTSUP the display engine has no video plane
 */
int display_sunxi_show_yuv(const struct device *dev, const struct display_sunxi_yuv *img);

/** An RGB picture for the scaling video plane */
struct display_sunxi_rgb {
	const void *data;
	uint16_t width;
	uint16_t height;
	/** Bytes from one row to the next */
	uint16_t stride;
	/**
	 * Part of the picture to show (zoom); all zero shows the whole picture. The window is
	 * scaled to the screen keeping its shape. For RGB the window is in pixels.
	 */
	uint16_t crop_x;
	uint16_t crop_y;
	uint16_t crop_width;
	uint16_t crop_height;
	/** 32 bit XRGB8888 instead of RGB565 */
	bool xrgb8888;
	/** As in display_sunxi_yuv */
	bool nonblock;
	/**
	 * Sharp enlargement for pictures with hard edges (a UI, pixel art): the picture is made a
	 * whole multiple of its size when that fits the screen, and the pixels are repeated, not
	 * interpolated. Bigger pictures are fitted as usual.
	 */
	bool pixel_perfect;
};

/**
 * @brief Show an RGB picture on the video plane, scaled like display_sunxi_show_yuv()
 *
 * The memory rules are the same as for display_sunxi_show_yuv().
 */
int display_sunxi_show_rgb(const struct device *dev, const struct display_sunxi_rgb *img);

/** @brief Stop showing the picture and bring the frame buffer plane back */
int display_sunxi_hide_yuv(const struct device *dev);

/**
 * Switches the frame buffer plane on or off without touching its content. A plane that is off
 * is not read by the display engine, which saves memory bandwidth while only a video is shown.
 */
int display_sunxi_fb_enable(const struct device *dev, bool enable);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_DISPLAY_DISPLAY_SUNXI_H_ */
