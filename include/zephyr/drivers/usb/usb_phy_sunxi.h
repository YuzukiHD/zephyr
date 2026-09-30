/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Allwinner sunxi USB 2.0 PHY
 *
 * One UTMI PHY serves the OTG device controller and the EHCI/OHCI host
 * controllers, one at a time. A controller driver acquires the PHY for its
 * role before it starts and releases it when it stops.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_USB_USB_PHY_SUNXI_H_
#define ZEPHYR_INCLUDE_DRIVERS_USB_USB_PHY_SUNXI_H_

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Controller that owns the PHY */
enum sunxi_usb_phy_role {
	/** OTG controller in device mode (VBUS valid is forced, no VBUS is driven) */
	SUNXI_USB_PHY_DEVICE,
	/** EHCI/OHCI host controllers (VBUS is driven) */
	SUNXI_USB_PHY_HOST,
};

/**
 * @brief Route the PHY to a controller and power it
 *
 * @retval 0 acquired
 * @retval -EBUSY the PHY is used by the other role
 * @retval -ENOTSUP role not supported yet
 */
int sunxi_usb_phy_acquire(const struct device *dev, enum sunxi_usb_phy_role role);

/** @brief Power the PHY down and give it up */
void sunxi_usb_phy_release(const struct device *dev, enum sunxi_usb_phy_role role);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_USB_USB_PHY_SUNXI_H_ */
