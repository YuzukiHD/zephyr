/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Decode an embedded PNG with the hardware decoder, report the timing and a
 * CRC of the RGBA pixels (a PNG is lossless, so the CRC must equal the one of
 * the host decode), then show it blended over black.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/vdec.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#include "test_png.h"
#include "vdec_show.h"

static uint32_t picture_crc(const struct vdec_frame *f)
{
	uint32_t crc = 0;

	for (int y = 0; y < f->height; y++) {
		crc = crc32_ieee_update(crc, f->plane[0] + y * f->stride[0], f->width * 4);
	}

	return crc;
}

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ve));
	struct vdec_frame frame;
	int64_t t;
	int ret;

	if (!device_is_ready(dev)) {
		printk("decoder not ready\n");
		return 0;
	}

	for (int i = 0; i < 3; i++) {
		t = k_uptime_get();
		ret = vdec_decode_image(dev, VDEC_CODEC_PNG, test_png, sizeof(test_png),
					VDEC_FORMAT_RGBA8888, &frame);
		t = k_uptime_get() - t;
		if (ret != 0) {
			printk("decode %d failed: %d\n", i, ret);
			return 0;
		}
		printk("decode %d: %dx%d stride %d in %lld ms, crc %08x\n", i, frame.width,
		       frame.height, frame.stride[0], t, picture_crc(&frame));
		if (i == 2) {
			if (IS_ENABLED(CONFIG_SAMPLE_VDEC_DUMP)) {
				printk("RGBA %d %d (bytes A, B, G, R)\n", frame.width, frame.height);
				for (int y = 0; y < frame.height; y++) {
					printk("R%03d:", y);
					for (int x = 0; x < frame.width * 4; x++) {
						printk("%02x", frame.plane[0][y * frame.stride[0] + x]);
					}
					printk("\n");
				}
			}
			printk("show: %d\n", vdec_show(&frame, true));
			printk("png decode done\n");
			k_sleep(K_FOREVER);
		}
		vdec_frame_release(dev, &frame);
	}

	return 0;
}
