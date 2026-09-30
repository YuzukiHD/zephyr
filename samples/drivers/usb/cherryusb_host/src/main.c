/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "usbh_core.h"
#include "usbh_msc.h"

extern uintptr_t usb_sunxi_ehci_base(void);

static uint8_t sector[512 * 8] USB_MEM_ALIGNX;

/* Read the first sectors of a mass storage device and check the MBR signature */
static void msc_probe(struct usbh_msc *msc)
{
	int ret = usbh_msc_scsi_init(msc);
	int64_t t0;
	uint32_t i;

	printk("msc: scsi init %d: %u blocks of %u bytes (%u MiB)\n", ret,
	       (unsigned int)msc->blocknum, msc->blocksize,
	       (unsigned int)((uint64_t)msc->blocknum * msc->blocksize >> 20));
	if (ret < 0) {
		return;
	}

	ret = usbh_msc_scsi_read10(msc, 0, sector, 1);
	printk("msc: read sector 0: %d, signature %02x%02x\n", ret, sector[510], sector[511]);

	/* 2 MiB sequential read, 4 KiB per transfer */
	t0 = k_uptime_get();
	for (i = 0; i < 512 && ret == 0; i++) {
		ret = usbh_msc_scsi_read10(msc, i * 8, sector, 8);
	}
	t0 = k_uptime_get() - t0;
	printk("msc: read 2 MiB: %d in %d ms\n", ret, (int)t0);
}

int main(void)
{
	struct usbh_msc *msc, *last = NULL;
	int ret = usbh_initialize(0, usb_sunxi_ehci_base(), NULL);

	printk("usbh_initialize: %d\n", ret);

	/* enumeration and class drivers log through the CherryUSB log */
	for (;;) {
		msc = usbh_find_class_instance("/dev/sda");
		if (msc != NULL && msc != last) {
			k_msleep(500);
			msc_probe(msc);
		}
		last = msc;
		k_msleep(500);
	}

	return 0;
}
