/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Play an H.264 elementary stream (Annex B) from the FAT file system on the SD
 * card: the file is read in chunks, handed to the hardware decoder, and each
 * decoded frame is scanned out by the display engine at the frame rate.
 */

#include <ff.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/display_sunxi.h>
#include <zephyr/drivers/vdec.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "overlay.h"

#define MOUNT_PT	"/SD:"
#define CHUNK_SIZE	(128 * 1024)
#define LATE_RESYNC_MS	150
#define REPORT_MS	5000

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = MOUNT_PT,
};

/* Offset of the last Annex B start code (with its leading zero) in the data, -1 if none */
static int last_start_code(const uint8_t *p, size_t len)
{
	for (size_t i = len; i >= 4; i--) {
		if (p[i - 3] == 0 && p[i - 2] == 0 && p[i - 1] == 1) {
			size_t pos = i - 3;

			return (pos > 0 && p[pos - 1] == 0) ? (int)pos - 1 : (int)pos;
		}
	}

	return -1;
}

static void show(const struct device *disp, const struct vdec_frame *f)
{
	struct display_sunxi_yuv yuv = {
		.y = f->plane[0],
		.uv = f->plane[1],
		.width = f->width,
		.height = f->height,
		.stride_y = f->stride[0],
		.stride_uv = f->stride[1],
		.bt709 = true,
	};

	display_sunxi_show_yuv(disp, &yuv);
}

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(ve));
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	const struct vdec_stream_config cfg = {
		.codec = VDEC_CODEC_H264,
		.format = VDEC_FORMAT_NV12,
		.buffer_size = 1024 * 1024,
	};
	struct vdec_frame frame, shown = {0};
	struct vdec_stream *stream;
	struct fs_file_t file;
	uint8_t *buf;
	size_t have = 0;
	bool eof = false, flushed = false;
	int frames = 0, late = 0, stalls = 0, ret;
	int64_t start, last_report, bytes_total = 0;
	int report_frames = 0;

	if (!device_is_ready(dev) || !device_is_ready(disp)) {
		printk("decoder or display not ready\n");
		return 0;
	}
	ret = fs_mount(&mp);
	if (ret != 0) {
		printk("cannot mount the card: %d\n", ret);
		return 0;
	}
	fs_file_t_init(&file);
	ret = fs_open(&file, CONFIG_SAMPLE_PLAYER_FILE, FS_O_READ);
	if (ret != 0) {
		printk("cannot open %s: %d\n", CONFIG_SAMPLE_PLAYER_FILE, ret);
		return 0;
	}
	buf = aligned_alloc(64, CHUNK_SIZE);
	if (buf == NULL) {
		printk("no memory\n");
		return 0;
	}
	ret = vdec_stream_open(dev, &cfg, &stream);
	if (ret != 0) {
		printk("cannot open the stream: %d\n", ret);
		return 0;
	}

	printk("playing %s\n", CONFIG_SAMPLE_PLAYER_FILE);
	start = last_report = k_uptime_get();
	overlay_start();
	while (true) {
		int64_t t0 = k_uptime_ticks();

		ret = vdec_stream_get_frame(dev, stream, &frame);
		play_stats.decode_us += k_ticks_to_us_near32((uint32_t)(k_uptime_ticks() - t0));
		if (ret == 0) {
			int64_t due = start + (int64_t)frames * 1000 * CONFIG_SAMPLE_PLAYER_FPS_DEN /
						      CONFIG_SAMPLE_PLAYER_FPS_NUM;
			int64_t now = k_uptime_get();

			if (IS_ENABLED(CONFIG_SAMPLE_PLAYER_REALTIME)) {
				if (now > due + LATE_RESYNC_MS) {
					late++;
					play_stats.late = late;
					start += now - due;
				} else if (now < due) {
					k_sleep(K_TIMEOUT_ABS_MS(due));
				}
			}
			show(disp, &frame);
			if (shown.priv != NULL) {
				vdec_frame_release(dev, &shown);
			}
			shown = frame;
			play_stats.width = frame.width;
			play_stats.height = frame.height;
			play_stats.frames = frames + 1;
			frames++;
			report_frames++;
			stalls = 0;

			now = k_uptime_get();
			if (now - last_report >= REPORT_MS) {
				int ms = (int)(now - last_report);

				printk("%d frames, %d.%02d fps, %d late, read %lld KiB\n", frames,
				       report_frames * 1000 / ms,
				       (report_frames * 100000 / ms) % 100, late,
				       bytes_total / 1024);
				last_report = now;
				report_frames = 0;
			}
			continue;
		}
		if (ret == -ENODATA) {
			break;
		}
		if (ret == -EBUSY) {
			k_msleep(5);
			continue;
		}
		if (ret != -EAGAIN) {
			printk("decode error %d after %d frames\n", ret, frames);
			break;
		}

		/* the decoder wants data */
		if (!eof && have < CHUNK_SIZE) {
			ssize_t n = fs_read(&file, buf + have, CHUNK_SIZE - have);

			if (n < 0) {
				printk("read error %d\n", (int)n);
				break;
			}
			if (n == 0) {
				eof = true;
			}
			have += n;
			bytes_total += n;
			play_stats.sd_bytes += n;
		}
		if (have > 0) {
			/* only whole NAL units: up to the last start code, or all at the end */
			int cut = eof ? (int)have : last_start_code(buf, have);
			size_t used = 0;

			if (cut > 0) {
				ret = vdec_stream_feed(dev, stream, buf, cut, -1, &used);
				if (ret != 0 && ret != -EAGAIN) {
					printk("feed error %d\n", ret);
					break;
				}
			}
			if (used > 0) {
				memmove(buf, buf + used, have - used);
				have -= used;
				stalls = 0;
			} else if (++stalls > 1000) {
				printk("stuck: nothing decodes or fits, %d frames\n", frames);
				break;
			}
		}
		if (eof && have == 0 && !flushed) {
			vdec_stream_flush(dev, stream);
			flushed = true;
		}
	}

	{
		int64_t ms = k_uptime_get() - start;

		printk("done: %d frames in %lld ms (%d.%02d fps), %d late\n", frames, ms,
		       (int)(frames * 1000LL / ms), (int)(frames * 100000LL / ms) % 100, late);
	}
	k_sleep(K_FOREVER);

	return 0;
}
