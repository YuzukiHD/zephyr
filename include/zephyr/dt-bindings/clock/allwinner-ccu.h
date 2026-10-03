/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_CLOCK_ALLWINNER_CCU_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_CLOCK_ALLWINNER_CCU_H_

/*
 * Allwinner sunxi CCU family clock/reset id encoding.
 *
 * A clock or reset id carries everything the generic sunxi drivers need
 * to poke a gate or reset line in any of the CCU blocks:
 *
 *   id = (base_index << 20) | (reg_offset << 8) | bit
 *
 *   - base_index selects one of the bases listed in the 'ccu-bases'
 *     property of the CCU node (main CCU, R_CCU, RTC_CCU, ...)
 *   - reg_offset is the register offset within that CCU (low 12 bits of
 *     the register address, so up to 4 KiB of registers per block)
 *   - bit is the gate / reset bit position within the register
 */

#define ALLWINNER_CCU_MAIN		0
#define ALLWINNER_CCU_R		1
#define ALLWINNER_CCU_RTC		2

#define ALLWINNER_CCU_ID(base, reg, bit)	(((base) << 20) | ((reg) << 8) | (bit))

#define ALLWINNER_CCU_ID_BASE(id)		((id) >> 20)
#define ALLWINNER_CCU_ID_REG(id)		(((id) >> 8) & 0xfff)
#define ALLWINNER_CCU_ID_BIT(id)		((id) & 0xff)

/* Crystal oscillator and 32k clock rates shared by the sunxi family */
#define ALLWINNER_CCU_HOSC_RATE		24000000
#define ALLWINNER_CCU_CLK32K_RATE	32768

/*
 * Standard sunxi APB_UART clock register layout.
 *
 * The APB_UART clock feeds the UART bus clocks. Its mux selects between
 * HOSC, CLK32K and two PLL_PERI outputs; the M and N factors divide the
 * parent clock.
 */
#define ALLWINNER_CCU_APB_UART_MUX_MASK		(0x7u << 24)
#define ALLWINNER_CCU_APB_UART_FACTOR_N_MASK	(0x3u << 8)
#define ALLWINNER_CCU_APB_UART_FACTOR_M_MASK	(0x1fu << 0)

#define ALLWINNER_CCU_APB_UART_MUX_HOSC		0
#define ALLWINNER_CCU_APB_UART_MUX_CLK32K	1
#define ALLWINNER_CCU_APB_UART_MUX_PLL480M	2
#define ALLWINNER_CCU_APB_UART_MUX_PLL1X	3

/*
 * Standard sunxi PLL_PERI control register layout.
 */
#define ALLWINNER_CCU_PLL_PERI_N_MASK		(0xffu << 8)
#define ALLWINNER_CCU_PLL_PERI_P0_MASK		(0x7u << 16)
#define ALLWINNER_CCU_PLL_PERI_INPUT_DIV2	(1u << 1)
#define ALLWINNER_CCU_PLL_PERI_DIV_MASK		(0x7u << 2)

/*
 * A clock id whose bit field is ALLWINNER_CCU_BIT_RATE is not a gate: it
 * names a rate-only clock (the CPU clock) in the register of the id. Such a
 * clock is always on, clock_control_on()/off() do nothing for it.
 */
#define ALLWINNER_CCU_BIT_RATE			0xff

/*
 * Standard sunxi PLL_CPU: control register and spread spectrum register in the
 * main CCU. The output is HOSC * N / P / M0 / M1; the N factor is latched by
 * the update bit, with the spread spectrum mode on while it changes.
 */
#define ALLWINNER_CCU_PLL_CPU_REG		0x0000
#define ALLWINNER_CCU_PLL_CPU_SSC_REG		0x0200
/* highest rate clock_control_set_rate() accepts: tested, the PLL itself allows more */
#define ALLWINNER_CCU_PLL_CPU_MAX_RATE		1008000000
#define ALLWINNER_CCU_PLL_CPU_N_MIN		12
#define ALLWINNER_CCU_PLL_CPU_N_MAX		125

#define ALLWINNER_CCU_PLL_CPU_M1_MASK		(0xfu << 0)
#define ALLWINNER_CCU_PLL_CPU_N_MASK		(0xffu << 8)
#define ALLWINNER_CCU_PLL_CPU_P_MASK		(0x7u << 16)
#define ALLWINNER_CCU_PLL_CPU_M0_MASK		(0x3u << 20)
#define ALLWINNER_CCU_PLL_CPU_UPDATE		(1u << 26)
#define ALLWINNER_CCU_PLL_CPU_SSC_MODE		(1u << 31)

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_CLOCK_ALLWINNER_CCU_H_ */
