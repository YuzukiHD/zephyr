/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "usbh_core.h"

extern uintptr_t usb_sunxi_ehci_base(void);

int main(void)
{
	int ret = usbh_initialize(0, usb_sunxi_ehci_base(), NULL);

	printk("usbh_initialize: %d\n", ret);

	/* enumeration and class drivers log through the CherryUSB log */
	return 0;
}
