/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Platform glue of CherryUSB for the Allwinner sunxi OTG controller: clocks,
 * reset, PHY, interrupt connection and data cache maintenance.
 */

#define DT_DRV_COMPAT allwinner_sunxi_musb

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/usb/usb_phy_sunxi.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

#include "usb_config.h"

LOG_MODULE_REGISTER(usb_glue_sunxi, CONFIG_USB_PHY_SUNXI_LOG_LEVEL);

#define SUNXI_VEND0		0x43
/* VEND0 bit 0: the CPU moves the FIFO data (no DMA) */
#define SUNXI_VEND0_PIO		BIT(0)

#if DT_HAS_COMPAT_STATUS_OKAY(allwinner_sunxi_musb)

#define USB_NODE DT_INST(0, allwinner_sunxi_musb)

extern void USBD_IRQHandler(uint8_t busid);

static void sunxi_usbd_isr(const void *arg)
{
	ARG_UNUSED(arg);
	USBD_IRQHandler(0);
}

void usb_dc_low_level_init(uint8_t busid)
{
	const struct device *clk = DEVICE_DT_GET(DT_CLOCKS_CTLR(USB_NODE));
	const struct device *phy = DEVICE_DT_GET(DT_PHANDLE(USB_NODE, phys));
	const struct reset_dt_spec rst = RESET_DT_SPEC_GET(USB_NODE);
	static bool connected;

	ARG_UNUSED(busid);

	if (!device_is_ready(clk) || !device_is_ready(rst.dev) || !device_is_ready(phy)) {
		LOG_ERR("clock, reset or phy not ready");
		return;
	}

	clock_control_on(clk, (clock_control_subsys_t)(uintptr_t)DT_CLOCKS_CELL(USB_NODE, clkid));
	reset_line_deassert_dt(&rst);

	if (sunxi_usb_phy_acquire(phy, SUNXI_USB_PHY_DEVICE)) {
		LOG_ERR("cannot take the phy");
		return;
	}

	sys_write8(SUNXI_VEND0_PIO, DT_REG_ADDR(USB_NODE) + SUNXI_VEND0);

	if (!connected) {
		IRQ_CONNECT(DT_IRQN(USB_NODE), DT_IRQ(USB_NODE, priority), sunxi_usbd_isr, NULL, 0);
		connected = true;
	}
	irq_enable(DT_IRQN(USB_NODE));
}

void usb_dc_low_level_deinit(uint8_t busid)
{
	const struct device *phy = DEVICE_DT_GET(DT_PHANDLE(USB_NODE, phys));

	ARG_UNUSED(busid);

	irq_disable(DT_IRQN(USB_NODE));
	sunxi_usb_phy_release(phy, SUNXI_USB_PHY_DEVICE);
}

/* Base address for usbd_initialize() */
uintptr_t usb_sunxi_otg_base(void)
{
	return DT_REG_ADDR(USB_NODE);
}

#endif /* DT_HAS_COMPAT_STATUS_OKAY(allwinner_sunxi_musb) */
