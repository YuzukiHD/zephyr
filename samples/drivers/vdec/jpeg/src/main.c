/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Decode an embedded JPEG with the hardware decoder, report the timing, a
 * checksum of every plane and dump the NV12 picture on the console so it can
 * be compared with a host decode.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/vdec.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "test_jpeg.h"

static uint32_t checksum(const uint8_t *p, int stride, int w, int h)
{
	uint32_t sum = 0;

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			sum = sum * 31 + p[y * stride + x];
		}
	}

	return sum;
}

static void dump_plane(const char *tag, const uint8_t *p, int stride, int w, int h)
{
	for (int y = 0; y < h; y++) {
		printk("%s%03d:", tag, y);
		for (int x = 0; x < w; x++) {
			printk("%02x", p[y * stride + x]);
		}
		printk("\n");
	}
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
		ret = vdec_decode_image(dev, VDEC_CODEC_JPEG, test_jpeg, sizeof(test_jpeg),
					VDEC_FORMAT_NV12, &frame);
		t = k_uptime_get() - t;
		if (ret != 0) {
			printk("decode %d failed: %d\n", i, ret);
			return 0;
		}
		printk("decode %d: %dx%d stride %d/%d in %lld ms\n", i, frame.width, frame.height,
		       frame.stride[0], frame.stride[1], t);
		printk("Y crc %08x, C crc %08x\n",
		       checksum(frame.plane[0], frame.stride[0], frame.width, frame.height),
		       checksum(frame.plane[1], frame.stride[1], frame.width, frame.height / 2));

		if (IS_ENABLED(CONFIG_SAMPLE_VDEC_DUMP) && i == 2) {
			printk("NV12 %d %d\n", frame.width, frame.height);
			dump_plane("Y", frame.plane[0], frame.stride[0], frame.width, frame.height);
			dump_plane("C", frame.plane[1], frame.stride[1], frame.width, frame.height / 2);
		}
		vdec_frame_release(dev, &frame);
	}
	printk("jpeg decode done\n");

	return 0;
}
