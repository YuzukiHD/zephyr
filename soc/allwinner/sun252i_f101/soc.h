/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_ALLWINNER_SUN252I_F101_SOC_H_
#define ZEPHYR_SOC_ALLWINNER_SUN252I_F101_SOC_H_

#define SUN252I_F101_CCU_BASE			0x02001000

/* UART bus clock/reset register (UART_BGR_REG) */
#define SUN252I_F101_CCU_UART_BGR_REG		0x090C
#define SUN252I_F101_CCU_UART_BGR_GATE(n)	BIT(n)
#define SUN252I_F101_CCU_UART_BGR_RST(n)		BIT(16 + (n))

/* T-Head C9xx M-mode CSRs */
#define SUN252I_F101_CSR_MXSTATUS		0x7C0
#define SUN252I_F101_CSR_MHCR			0x7C1
#define SUN252I_F101_CSR_MCOR			0x7C2
#define SUN252I_F101_CSR_MHINT			0x7C5

/* MXSTATUS bits */
#define SUN252I_F101_MXSTATUS_THEADISAEE		BIT(22)
#define SUN252I_F101_MXSTATUS_MM			BIT(15)

/* Enable XuanTie ISA extensions and hardware handling of misaligned access */
#define SUN252I_F101_MXSTATUS_INIT		(SUN252I_F101_MXSTATUS_THEADISAEE | \
						 SUN252I_F101_MXSTATUS_MM)

/*
 * MCOR bits: invalidate I/D cache, BHT and branch target buffer.
 * Values from the F101 SPL (platform/cpu/xuantie/c9xx_cache.c).
 */
#define SUN252I_F101_MCOR_INIT			0x70013

/*
 * MHCR bits: IE/DE (enable I/D cache), WA, WB, RS, BPE, BTB, WBR, L0BTB.
 * MHINT bits: DPLD, AMR, IPLD, LPE (cache prefetch).
 */
#define SUN252I_F101_MHCR_INIT			0x11FF
#define SUN252I_F101_MHINT_INIT			0x16E30C

#endif /* ZEPHYR_SOC_ALLWINNER_SUN252I_F101_SOC_H_ */