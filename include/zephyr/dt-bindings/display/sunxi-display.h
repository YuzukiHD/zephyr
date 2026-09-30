/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_DISPLAY_SUNXI_DISPLAY_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_DISPLAY_SUNXI_DISPLAY_H_

#include <zephyr/dt-bindings/pinctrl/allwinner-pinctrl.h>

/* Global sunxi pin number, e.g. SUNXI_PIN(ALLWINNER_PIO_PD, 22) */
#define SUNXI_PIN(bank, n)		((bank) * 32 + (n))

/* rgb encoder: hv-mode */
#define SUNXI_HV_PARALLEL_RGB		0x0
#define SUNXI_HV_SERIAL_RGB		0x8
#define SUNXI_HV_SERIAL_RGB_DUMMY	0xa
#define SUNXI_HV_SERIAL_YUV		0xc
#define SUNXI_HV_CCIR656_2CYC		0xe

/* panel-dsi: mode-flags */
#define SUNXI_DSI_MODE_VIDEO		(1 << 0)
#define SUNXI_DSI_MODE_VIDEO_BURST	(1 << 1)
#define SUNXI_DSI_MODE_VIDEO_SYNC_PULSE	(1 << 2)
#define SUNXI_DSI_MODE_LPM		(1 << 3)
#define SUNXI_DSI_CLOCK_NON_CONTINUOUS	(1 << 4)
#define SUNXI_DSI_MODE_NO_EOT_PACKET	(1 << 5)

/*
 * Command sequences (power-on/off, init, exit) are cell arrays, each
 * command being "type len arg data...". Use the macros below to build them.
 */
#define SUNXI_ARG_COUNT(...) \
	SUNXI_ARG_COUNT_(__VA_ARGS__, \
		64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, \
		48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, \
		32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, \
		16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define SUNXI_ARG_COUNT_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, \
			 _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, \
			 _28, _29, _30, _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, \
			 _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, _51, _52, _53, \
			 _54, _55, _56, _57, _58, _59, _60, _61, _62, _63, _64, N, ...) N

/* wait @ms milliseconds */
#define SUNXI_CMD_DELAY(ms)		1 0 ms
/* drive a GPIO to the physical @level (0/1) */
#define SUNXI_CMD_GPIO(pin, level)	2 level pin
/* MIPI DCS write: command byte and parameters, then @ms delay */
#define SUNXI_CMD_DCS(ms, ...)		4 SUNXI_ARG_COUNT(__VA_ARGS__) ms __VA_ARGS__
/* generic MIPI DSI write */
#define SUNXI_CMD_GENERIC(ms, ...)	5 SUNXI_ARG_COUNT(__VA_ARGS__) ms __VA_ARGS__
/* bit-banged SPI command / data bytes (panel spi init) */
#define SUNXI_CMD_SPI_CMD(ms, ...)	6 SUNXI_ARG_COUNT(__VA_ARGS__) ms __VA_ARGS__
#define SUNXI_CMD_SPI_DATA(ms, ...)	7 SUNXI_ARG_COUNT(__VA_ARGS__) ms __VA_ARGS__

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_DISPLAY_SUNXI_DISPLAY_H_ */
