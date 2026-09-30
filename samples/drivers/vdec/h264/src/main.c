/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Decode an embedded H.264 clip with the hardware decoder. Every frame is
 * checked against the CRC32 of the host decode (the decoding process is exactly
 * specified, so the frames must be identical) and shown on the display at the
 * frame rate of the clip.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/vdec.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#include "test_h264.h"
#include "vdec_show.h"

#define FRAME_MS	33

static uint32_t frame_crc(const struct vdec_frame *f)
{
	uint32_t crc = 0;

	for (int y = 0; y < f->height; y++) {
		crc = crc32_ieee_update(crc, f->plane[0] + y * f->stride[0], f->width);
	}
	for (int y = 0; y < f->height / 2; y++) {
		crc = crc32_ieee_update(crc, f->plane[1] + y * f->stride[1], f->width);
	}

	return crc;
}

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ve));
	const struct vdec_stream_config cfg = {
		.codec = VDEC_CODEC_H264,
		.format = VDEC_FORMAT_NV12,
	};
	struct vdec_frame frame, shown = {0};
	struct vdec_stream *stream;
	size_t off = 0, used;
	int frames = 0, bad = 0, ret;
	int64_t start, next;

	if (!device_is_ready(dev)) {
		printk("decoder not ready\n");
		return 0;
	}
	ret = vdec_stream_open(dev, &cfg, &stream);
	if (ret != 0) {
		printk("cannot open the stream: %d\n", ret);
		return 0;
	}

	start = k_uptime_get();
	next = start;
	while (true) {
		ret = vdec_stream_get_frame(dev, stream, &frame);
		if (ret == -EAGAIN && off < sizeof(test_h264)) {
			ret = vdec_stream_feed(dev, stream, test_h264 + off, sizeof(test_h264) - off,
					       frames * FRAME_MS, &used);
			if (ret != 0 && ret != -EAGAIN) {
				printk("feed failed: %d\n", ret);
				break;
			}
			off += used;
			if (off >= sizeof(test_h264)) {
				vdec_stream_flush(dev, stream);
			}
			continue;
		}
		if (ret == -ENODATA) {
			break;
		}
		if (ret != 0) {
			printk("get_frame failed: %d (frame %d, fed %zu of %zu)\n", ret, frames, off,
			       sizeof(test_h264));
			break;
		}

		if (frames < ARRAY_SIZE(test_h264_crc)) {
			uint32_t crc = frame_crc(&frame);

			if (crc != test_h264_crc[frames]) {
				bad++;
				printk("frame %d: crc %08x, expected %08x\n", frames, crc,
				       test_h264_crc[frames]);
			}
		} else {
			bad++;
		}
		frames++;

		/* show it at the clip's pace and let go of the one shown before */
		next += FRAME_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));
		vdec_show(&frame, false);
		if (shown.priv != NULL) {
			vdec_frame_release(dev, &shown);
		}
		shown = frame;
	}

	printk("h264 decode done: %d frames in %lld ms, %d bad\n", frames, k_uptime_get() - start,
	       bad);
	k_sleep(K_FOREVER);

	return 0;
}
