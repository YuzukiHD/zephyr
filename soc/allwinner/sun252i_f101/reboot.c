/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

/*
 * The only way to reset the SoC is the watchdog: arm it with the shortest
 * timeout in reset mode and wait for it to expire (about one second).  The
 * registers are the ones of drivers/watchdog/wdt_sunxi.c, used directly so
 * that a reboot works whether or not the watchdog device is enabled.
 */
#define WDT_BASE	0x06011000UL
#define WDT_IRQ_EN	0x00
#define WDT_STATUS	0x04
#define WDT_CTL		0x10
#define WDT_CFG		0x14
#define WDT_MODE	0x18
#define WDT_OUT_CFG	0x1c

#define WDT_KEY		0x16aa0000U
#define WDT_CFG_RESET	BIT(0)
#define WDT_MODE_ENABLE	BIT(0)
#define WDT_TIMEOUT_1S	(0x1U << 4)
#define WDT_CTL_KEY	(0x0a57U << 1)
#define WDT_CTL_RESTART	BIT(0)
#define WDT_OUT_RESET_PULSE	0x3fU

void sys_arch_reboot(int type)
{
	ARG_UNUSED(type);

	sys_write32(WDT_KEY, WDT_BASE + WDT_MODE);
	sys_write32(0U, WDT_BASE + WDT_IRQ_EN);
	sys_write32(1U, WDT_BASE + WDT_STATUS);
	sys_write32(WDT_OUT_RESET_PULSE, WDT_BASE + WDT_OUT_CFG);
	sys_write32(WDT_KEY | WDT_CFG_RESET, WDT_BASE + WDT_CFG);
	sys_write32(WDT_KEY | WDT_TIMEOUT_1S | WDT_MODE_ENABLE, WDT_BASE + WDT_MODE);
	sys_write32(WDT_CTL_KEY | WDT_CTL_RESTART, WDT_BASE + WDT_CTL);

	for (;;) {
	}
}
