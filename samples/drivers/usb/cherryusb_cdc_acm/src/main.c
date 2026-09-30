/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

extern void cdc_acm_init(uint8_t busid, uintptr_t reg_base);
extern uintptr_t usb_sunxi_otg_base(void);
extern void cdc_acm_data_send_with_dtr_test(uint8_t busid);

int main(void)
{
	cdc_acm_init(0, usb_sunxi_otg_base());
	printk("CherryUSB CDC ACM started\n");

	/* stream the demo buffer to the host while a terminal has the port open */
	while (true) {
		cdc_acm_data_send_with_dtr_test(0);
		k_msleep(100);
	}

	return 0;
}
