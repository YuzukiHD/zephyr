/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "usbh_core.h"
#include "usbh_msc.h"
#include "usbh_serial.h"

#include <string.h>

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

static uint8_t tx_buf[64] USB_MEM_ALIGNX;
static uint8_t rx_buf[64] USB_MEM_ALIGNX;

/* Write a pattern to the serial adapter once a second and read it back (TX and RX pins shorted) */
static void serial_probe(const char *name)
{
	struct usbh_serial_termios termios = {
		.baudrate = 115200,
		.databits = USBH_SERIAL_DATABITS_8,
		.parity = USBH_SERIAL_PARITY_NONE,
		.stopbits = USBH_SERIAL_STOPBITS_1,
		.rtscts = false,
		.rx_timeout = 500,
	};
	struct usbh_serial *serial = usbh_serial_open(name, USBH_SERIAL_O_RDWR);
	int ret, rx = 0;

	if (serial == NULL) {
		printk("serial: cannot open %s\n", name);
		return;
	}

	ret = usbh_serial_control(serial, USBH_SERIAL_CMD_SET_ATTR, &termios);
	printk("serial: set 115200 8N1: %d\n", ret);

	for (int round = 0; usbh_find_class_instance(name) != NULL; round++) {
		int want;

		/* drop what is left from earlier rounds (late or partial echoes) */
		while (usbh_serial_read(serial, rx_buf, sizeof(rx_buf)) > 0) {
		}

		memset(rx_buf, 0, sizeof(rx_buf));
		snprintk((char *)tx_buf, sizeof(tx_buf), "F101 OHCI serial test %d", round);
		want = strlen((char *)tx_buf);

		ret = usbh_serial_write(serial, tx_buf, want);
		printk("serial: write %d bytes: %d\n", want, ret);

		/* the echo can arrive in pieces */
		rx = 0;
		for (int try = 0; try < 3 && rx < want; try++) {
			ret = usbh_serial_read(serial, rx_buf + rx, sizeof(rx_buf) - rx);
			if (ret > 0) {
				rx += ret;
			}
		}
		printk("serial: read %d: \"%s\" %s\n", rx, rx > 0 ? (char *)rx_buf : "",
		       (rx == want && !memcmp(rx_buf, tx_buf, want)) ? "MATCH" : "no match");
		k_msleep(1000);
	}

	usbh_serial_close(serial);
}

int main(void)
{
	struct usbh_msc *msc, *last = NULL;
	bool serial_done = false;
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

		if (!serial_done && usbh_find_class_instance("/dev/ttyACM0") != NULL) {
			k_msleep(500);
			serial_probe("/dev/ttyACM0");
			serial_done = true;
		}
		k_msleep(500);
	}

	return 0;
}
