/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Glue between the prebuilt decoder archive and Zephyr.
 *
 * The archive calls a fixed set of C library, clock, reset, interrupt and
 * semaphore functions under the names it was built with. Each group lives in
 * its own glue_*.c file and is implemented on Zephyr primitives only; nothing
 * outside this directory deals with those names.
 */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_GLUE_GLUE_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_GLUE_GLUE_H_

#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#define VE_NODE DT_INST(0, allwinner_sunxi_ve)

#define VE_CCU_BASE	0x02001000U
#define VE_CLK_REG	(VE_CCU_BASE + DT_PROP(VE_NODE, clock_reg))

/* Main CCU controller number and the clock / reset ids the archive passes in */
#define GLUE_CCU_SYS		2
#define GLUE_CLK_CPU_PLL	1
#define GLUE_CLK_DDR_PLL	3
#define GLUE_CLK_PERI_2X	5
#define GLUE_CLK_PERI_1X	6
#define GLUE_CLK_PERI_480M	8
#define GLUE_CLK_AUDIO1_DIV2	13
#define GLUE_CLK_VE		27
#define GLUE_CLK_BUS_VE		28
#define GLUE_CLK_BUS_VE_M	40
#define GLUE_RST_BUS_VE		6

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_GLUE_GLUE_H_ */
