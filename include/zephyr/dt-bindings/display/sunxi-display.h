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
/* wait @ms milliseconds */
#define SUNXI_CMD_DELAY(ms)		1 0 ms
/* drive a GPIO to the physical @level (0/1) */
#define SUNXI_CMD_GPIO(pin, level)	2 level pin
/*
 * Writes: @n bytes (the command byte and its parameters, separated by spaces, no commas),
 * then @ms delay
 */
#define SUNXI_CMD_DCS(ms, n, bytes)	4 n ms bytes
/* generic MIPI DSI write */
#define SUNXI_CMD_GENERIC(ms, n, bytes)	5 n ms bytes
/* bit-banged SPI command / data bytes (panel spi init) */
#define SUNXI_CMD_SPI_CMD(ms, n, bytes)	6 n ms bytes
#define SUNXI_CMD_SPI_DATA(ms, n, bytes)	7 n ms bytes

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_DISPLAY_SUNXI_DISPLAY_H_ */
