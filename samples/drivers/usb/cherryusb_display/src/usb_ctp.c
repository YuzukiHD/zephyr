/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The capacitive touch panel (FT5x46 family, I2C address 0x38 on I2C1) as the touch screen of the
 * USB display: a thread polls the controller and turns its points into reports of the HID touch
 * interface.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>

#include "usb_ctp.h"
#include "usb_touch.h"

#define CTP_ADDR	0x38
#define CTP_MAX_X	1024
#define CTP_MAX_Y	600
#define CTP_POLL_MS	10

#define REG_TD_STATUS	0x02
#define POINT_BYTES	6
#define POINTS		USB_TOUCH_MAX_CONTACTS

static const struct device *const bus = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static const struct device *const pe = DEVICE_DT_GET(DT_NODELABEL(gpioe));

/* PE0 is SCL, PE1 is SDA */
static bool swap_xy = IS_ENABLED(CONFIG_SAMPLE_USB_DISPLAY_CTP_SWAP);
static bool flip_x = IS_ENABLED(CONFIG_SAMPLE_USB_DISPLAY_CTP_FLIP_X);
static bool flip_y = IS_ENABLED(CONFIG_SAMPLE_USB_DISPLAY_CTP_FLIP_Y);

/* the status register and the points behind it: [0] count, then 6 bytes per point */
static int ctp_points(uint8_t *raw)
{
	uint8_t reg = REG_TD_STATUS;

	return i2c_write_read(bus, CTP_ADDR, &reg, 1, raw, 1 + POINTS * POINT_BYTES);
}

/* log the devices that answer on the touch panel bus and, if none, the levels of its lines */
static void usb_ctp_check(void)
{
	int found = 0, scl, sda;

	if (!device_is_ready(bus)) {
		printk("usb ctp: the I2C1 bus is not ready\n");
		return;
	}
	for (uint8_t addr = 0x08; addr < 0x78; addr++) {
		uint8_t b;

		if (i2c_read(bus, &b, 1, addr) == 0) {
			printk("usb ctp: device at 0x%02x\n", addr);
			found++;
		}
	}
	printk("usb ctp: %d device(s) on I2C1\n", found);
	if (found == 0 && device_is_ready(pe)) {
		/* the line levels without the controller: a line held low by the wiring shows here */
		gpio_pin_configure(pe, 0, GPIO_INPUT);
		gpio_pin_configure(pe, 1, GPIO_INPUT);
		scl = gpio_pin_get_raw(pe, 0);
		sda = gpio_pin_get_raw(pe, 1);
		printk("usb ctp: PE0 (SCL) = %d, PE1 (SDA) = %d as GPIO inputs\n", scl, sda);
	}
}

static void scale(uint32_t x, uint32_t y, uint16_t *ox, uint16_t *oy)
{
	if (swap_xy) {
		uint32_t t = x;

		x = y;
		y = t;
	}
	if (flip_x) {
		x = CTP_MAX_X - 1 - x;
	}
	if (flip_y) {
		y = CTP_MAX_Y - 1 - y;
	}
	x = MIN(x, CTP_MAX_X - 1);
	y = MIN(y, CTP_MAX_Y - 1);
	*ox = x * USB_TOUCH_MAX / (CTP_MAX_X - 1);
	*oy = y * USB_TOUCH_MAX / (CTP_MAX_Y - 1);
}

static void ctp_thread(void *a, void *b, void *c)
{
	struct usb_touch_point out[POINTS], last[POINTS];
	bool was_down[POINTS] = {false};
	uint8_t raw[1 + POINTS * POINT_BYTES];

	/* a bus that does not answer makes every address wait for its time-out: not before the USB device is up */
	k_sleep(K_SECONDS(2));
	usb_ctp_check();
	memset(last, 0, sizeof(last));
	for (;;) {
		bool down[POINTS] = {false};
		int n, count = 0;
		bool changed = false;

		k_msleep(CTP_POLL_MS);
		if (ctp_points(raw) != 0) {
			continue;
		}
		n = raw[0] & 0x0F;
		if (n > POINTS) {
			n = 0;
		}
		memset(out, 0, sizeof(out));
		for (int i = 0; i < n; i++) {
			const uint8_t *p = &raw[1 + i * POINT_BYTES];
			int id = p[2] >> 4;
			uint32_t x = ((p[0] & 0x0F) << 8) | p[1];
			uint32_t y = ((p[2] & 0x0F) << 8) | p[3];

			if (id >= POINTS || (p[0] >> 6) == 1) { /* lifted */
				continue;
			}
			down[id] = true;
			out[id].id = id;
			out[id].down = true;
			scale(x, y, &out[id].x, &out[id].y);
		}
		for (int i = 0; i < POINTS; i++) {
			if (!down[i] && was_down[i]) {
				/* one report with the tip switch off */
				out[i] = last[i];
				out[i].down = false;
				changed = true;
			}
			if (down[i] && (!was_down[i] || out[i].x != last[i].x ||
					out[i].y != last[i].y)) {
				changed = true;
			}
			if (down[i] || was_down[i]) {
				count = i + 1;
			}
		}
		if (!changed) {
			continue;
		}
		/* the report holds the slots 0..count-1; free slots are sent as not touching */
		for (int i = 0; i < count; i++) {
			if (!down[i] && !was_down[i]) {
				memset(&out[i], 0, sizeof(out[i]));
				out[i].id = i;
			}
		}
		while (usb_touch_send(out, count) == -EBUSY) {
			k_msleep(1);
		}
		for (int i = 0; i < POINTS; i++) {
			was_down[i] = down[i];
			if (down[i]) {
				last[i] = out[i];
			}
		}
	}
}

K_THREAD_STACK_DEFINE(ctp_stack, 2048);
static struct k_thread ctp_thread_data;

void usb_ctp_start(void)
{
	if (!device_is_ready(bus)) {
		return;
	}
	k_thread_create(&ctp_thread_data, ctp_stack, K_THREAD_STACK_SIZEOF(ctp_stack), ctp_thread,
			NULL, NULL, NULL, 9, 0, K_NO_WAIT);
	k_thread_name_set(&ctp_thread_data, "ctp");
}
