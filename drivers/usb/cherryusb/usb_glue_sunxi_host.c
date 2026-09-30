/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Platform glue of CherryUSB for the Allwinner sunxi EHCI/OHCI host
 * controllers: bus gates, resets, PHY and VBUS, interrupt connection.
 */

#define DT_DRV_COMPAT allwinner_sunxi_ehci

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/usb/usb_phy_sunxi.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include "usbh_core.h"
#include "usb_hc_ehci.h"
#include "usb_hc_ohci.h"

LOG_MODULE_REGISTER(usb_glue_sunxi_host, CONFIG_USB_PHY_SUNXI_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(allwinner_sunxi_ehci)

#define HCI_NODE DT_INST(0, allwinner_sunxi_ehci)

extern void USBH_IRQHandler(uint8_t busid);
extern void OHCI_IRQHandler(uint8_t busid);

static void sunxi_ehci_isr(const void *arg)
{
	ARG_UNUSED(arg);
	USBH_IRQHandler(0);
}

static void sunxi_ohci_isr(const void *arg)
{
	ARG_UNUSED(arg);
	OHCI_IRQHandler(0);
}

uintptr_t usb_sunxi_ehci_base(void)
{
	return DT_REG_ADDR_BY_NAME(HCI_NODE, ehci);
}

void usb_hc_low_level_init(struct usbh_bus *bus)
{
	const struct device *clk = DEVICE_DT_GET(DT_CLOCKS_CTLR_BY_NAME(HCI_NODE, ehci));
	const struct device *phy = DEVICE_DT_GET(DT_PHANDLE(HCI_NODE, phys));
	const struct reset_dt_spec rst_ehci = RESET_DT_SPEC_GET_BY_IDX(HCI_NODE, 0);
	const struct reset_dt_spec rst_ohci = RESET_DT_SPEC_GET_BY_IDX(HCI_NODE, 1);
	static bool connected;

	ARG_UNUSED(bus);

	if (!device_is_ready(clk) || !device_is_ready(rst_ehci.dev) || !device_is_ready(phy)) {
		LOG_ERR("clock, reset or phy not ready");
		return;
	}

	clock_control_on(clk, (clock_control_subsys_t)(uintptr_t)DT_CLOCKS_CELL_BY_NAME(
				      HCI_NODE, ehci, clkid));
	clock_control_on(clk, (clock_control_subsys_t)(uintptr_t)DT_CLOCKS_CELL_BY_NAME(
				      HCI_NODE, ohci, clkid));
	reset_line_deassert_dt(&rst_ehci);
	reset_line_deassert_dt(&rst_ohci);

	if (sunxi_usb_phy_acquire(phy, SUNXI_USB_PHY_HOST)) {
		LOG_ERR("cannot take the phy");
		return;
	}

	if (!connected) {
		IRQ_CONNECT(DT_IRQN_BY_IDX(HCI_NODE, 0),
			    DT_IRQ_BY_IDX(HCI_NODE, 0, priority), sunxi_ehci_isr, NULL, 0);
		IRQ_CONNECT(DT_IRQN_BY_IDX(HCI_NODE, 1),
			    DT_IRQ_BY_IDX(HCI_NODE, 1, priority), sunxi_ohci_isr, NULL, 0);
		connected = true;
	}
	irq_enable(DT_IRQN_BY_IDX(HCI_NODE, 0));
	irq_enable(DT_IRQN_BY_IDX(HCI_NODE, 1));
}

void usb_hc_low_level_deinit(struct usbh_bus *bus)
{
	const struct device *phy = DEVICE_DT_GET(DT_PHANDLE(HCI_NODE, phys));

	ARG_UNUSED(bus);

	irq_disable(DT_IRQN_BY_IDX(HCI_NODE, 0));
	irq_disable(DT_IRQN_BY_IDX(HCI_NODE, 1));
	sunxi_usb_phy_release(phy, SUNXI_USB_PHY_HOST);
}

/* The speed of the device behind a root port, from the port status */
uint8_t usbh_get_port_speed(struct usbh_bus *bus, const uint8_t port)
{
	uint32_t regval = EHCI_HCOR->portsc[port - 1];

	if ((regval & EHCI_PORTSC_LSTATUS_MASK) == EHCI_PORTSC_LSTATUS_KSTATE) {
		return USB_SPEED_LOW;
	}

	return (regval & EHCI_PORTSC_PE) ? USB_SPEED_HIGH : USB_SPEED_FULL;
}

#endif /* DT_HAS_COMPAT_STATUS_OKAY(allwinner_sunxi_ehci) */
