/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/clock/allwinner-ccu.h>
#include <zephyr/sys/util.h>
#include <soc.h>

/*
 * The ns16550 driver only toggles the UART reset line at init and relies
 * on the clock being up when the system starts, so the SoC owns the
 * console UART clock here instead of depending on the bootloader.
 *
 * The clock id is taken from the 'clocks' property of the console UART
 * (chosen/zephyr,console), so the console can be moved to another UART
 * without touching this file.
 */
#define SOC_CONSOLE_CLK_ID	DT_PHA_BY_IDX(DT_CHOSEN(zephyr_console), clocks, 0, clkid)

static void xuantie_cpu_init(void)
{
	uint32_t mxstatus;

	/*
	 * Clear the individual interrupt-enable bits so no interrupt can
	 * fire before the kernel is ready,.
	 */
	__asm__ volatile("csrw mie, x0" ::: "memory");
	__asm__ volatile("csrw mip, x0" ::: "memory");

	/*
	 * Global machine interrupts must stay off until the kernel is up. A
	 * loader that jumps here (xfel exec) can leave mstatus.MIE set; once
	 * the PLIC driver enables MEIE, a pending peripheral interrupt (e.g.
	 * the UART left over from the loader) would then be taken in the
	 * middle of the kernel initialization.
	 */
	__asm__ volatile("csrc mstatus, %0" :: "r"(1 << 3) : "memory");

	/*
	 * Enable XuanTie ISA extensions (needed for the th.dcache/th.icache
	 * CMO instructions) and hardware handling of misaligned accesses.
	 */
	__asm__ volatile("csrr %0, %1" : "=r"(mxstatus) : "i"(SUN252I_F101_CSR_MXSTATUS));
	mxstatus |= SUN252I_F101_MXSTATUS_INIT;
	__asm__ volatile("csrw %1, %0" :: "r"(mxstatus), "i"(SUN252I_F101_CSR_MXSTATUS) : "memory");
}

void soc_early_init_hook(void)
{
	uint32_t clk_id = SOC_CONSOLE_CLK_ID;
	volatile uint32_t *bgr = (volatile uint32_t *)(SUN252I_F101_CCU_BASE +
						       ALLWINNER_CCU_ID_REG(clk_id));

	xuantie_cpu_init();

	/* Enable the console UART bus clock and deassert its reset early,
	 * so the console is available before the CCU driver is initialized.
	 */
	*bgr |= BIT(ALLWINNER_CCU_ID_BIT(clk_id)) |
		BIT(16 + ALLWINNER_CCU_ID_BIT(clk_id));
}

/*
 * Called from z_prep_c() before BSS zeroing and data copy, so it also
 * covers the case where the bootloader disabled the caches before
 * handing over. Values follow the F101 SPL
 * (platform/cpu/xuantie/c9xx_cache.c).
 */
void arch_cache_init(void)
{
	/*
	 * Invalidate I/D cache, BHT and branch target buffer before
	 * enabling the caches.
	 */
	__asm__ volatile(
		"csrw 0x7C2, %0\n"		/* mcor */
		"fence\n"
		"fence.i\n"
		:
		: "r"(SUN252I_F101_MCOR_INIT)
		: "memory");

	/* Enable I/D cache, write-allocate/back, return stack, branch
	 * prediction, BTB, write burst and L0 BTB.
	 */
	__asm__ volatile(
		"csrw 0x7C1, %0\n"		/* mhcr */
		"fence\n"
		:
		: "r"(SUN252I_F101_MHCR_INIT)
		: "memory");

	/* Enable D-cache prefetch, AMR, I-cache prefetch and loop acceleration. */
	__asm__ volatile(
		"csrw 0x7C5, %0\n"		/* mhint */
		"fence\n"
		:
		: "r"(SUN252I_F101_MHINT_INIT)
		: "memory");
}