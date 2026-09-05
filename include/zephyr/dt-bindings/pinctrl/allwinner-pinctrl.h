/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_ALLWINNER_PINCTRL_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_ALLWINNER_PINCTRL_H_

/* Pin number space: each PIO bank has 32 pins, banks PA..PF map to 0..5 */
#define ALLWINNER_PIN(port, num) (((port) * 32) + (num))

/*
 * Pin mux configuration value.
 *
 * The muxsel field selects the pin function (0 = input, 1 = output,
 * 2..15 = peripheral functions, see the SoC manual for the mapping).
 */
#define ALLWINNER_PINMUX(port, num, muxsel) \
	((ALLWINNER_PIN(port, num) << 4) | (muxsel))

#define ALLWINNER_PINMUX_PIN(pinmux)	((pinmux) >> 4)
#define ALLWINNER_PINMUX_MUXSEL(pinmux)	((pinmux) & 0x0f)

/* PIO banks */
#define ALLWINNER_PIO_PA	0
#define ALLWINNER_PIO_PB	1
#define ALLWINNER_PIO_PC	2
#define ALLWINNER_PIO_PD	3
#define ALLWINNER_PIO_PE	4
#define ALLWINNER_PIO_PF	5

/*
 * PIO hardware layout types, selected by the 'allwinner,pio-hw-type'
 * property of the pinctrl/gpio nodes. See
 * include/zephyr/drivers/pinctrl/pinctrl_sunxi.h for the register layout
 * description of each type.
 */
#define ALLWINNER_PIO_HW_TYPE0	0

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_ALLWINNER_PINCTRL_H_ */