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
	/** Samples cover 0..255 instead of 16..235 (JPEG) */
	bool full_range;
	/** BT.709 matrix instead of BT.601 */
	bool bt709;
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

/** @brief Stop showing the picture and bring the frame buffer plane back */
int display_sunxi_hide_yuv(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_DISPLAY_DISPLAY_SUNXI_H_ */
