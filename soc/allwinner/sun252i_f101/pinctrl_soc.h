/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_ALLWINNER_SUN252I_F101_PINCTRL_SOC_H_
#define ZEPHYR_SOC_ALLWINNER_SUN252I_F101_PINCTRL_SOC_H_

#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/pinctrl/allwinner-pinctrl.h>
#include <zephyr/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pinctrl_soc_pin_t {
	uint32_t pinmux;
	uint8_t pull;
} pinctrl_soc_pin_t;

#define ALLWINNER_PIO_PULL_NONE		0
#define ALLWINNER_PIO_PULL_UP		1
#define ALLWINNER_PIO_PULL_DOWN		2

#define ALLWINNER_DT_PULL(node_id)						\
	(DT_PROP(node_id, bias_pull_up) ?					\
		ALLWINNER_PIO_PULL_UP :						\
	 (DT_PROP(node_id, bias_pull_down) ?					\
		ALLWINNER_PIO_PULL_DOWN : ALLWINNER_PIO_PULL_NONE))

#define Z_PINCTRL_STATE_PIN_INIT(node_id, prop, idx)				\
	{									\
		.pinmux = DT_PROP_BY_IDX(node_id, prop, idx),			\
		.pull = ALLWINNER_DT_PULL(node_id)				\
	},

#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop)				\
	{ DT_FOREACH_CHILD_VARGS(DT_PHANDLE(node_id, prop),			\
				 DT_FOREACH_PROP_ELEM, pinmux,			\
				 Z_PINCTRL_STATE_PIN_INIT) }

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SOC_ALLWINNER_SUN252I_F101_PINCTRL_SOC_H_ */