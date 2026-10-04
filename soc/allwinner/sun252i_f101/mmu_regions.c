/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/arch/riscv/mm.h>
#include <zephyr/sys/util.h>

/*
 * The register blocks of the peripherals, in whole 4 MiB pages, mapped 1:1.
 * The offsets of the devices in them are in the device tree.
 */
const struct riscv_mmu_region riscv_mmu_regions[] = {
	/* video engine */
	RISCV_MMU_REGION("ve", 0x01c00000, 0x00400000),
	/* GPIO, PWM, CCU, GPADC, audio codec, I2S, S/PDIF */
	RISCV_MMU_REGION("apb0", 0x02000000, 0x00400000),
	/* UART, TWI */
	RISCV_MMU_REGION("uart", 0x02400000, 0x00400000),
	/* system control, DMA, SID, MBUS */
	RISCV_MMU_REGION("sys", 0x03000000, 0x00400000),
	/* SD host, SPI, USB */
	RISCV_MMU_REGION("ahb", 0x04000000, 0x00400000),
	/* display engine */
	RISCV_MMU_REGION("de", 0x05000000, 0x00400000),
	/* G2D, MIPI DSI and D-PHY, TCON */
	RISCV_MMU_REGION("disp", 0x05400000, 0x00400000),
	/* watchdog */
	RISCV_MMU_REGION("wdt", 0x06000000, 0x00400000),
	/* PLIC */
	RISCV_MMU_REGION("plic", 0x10000000, 0x00400000),
	/* CLINT */
	RISCV_MMU_REGION("clint", 0x14000000, 0x00400000),
};

const unsigned int riscv_mmu_regions_count = ARRAY_SIZE(riscv_mmu_regions);
