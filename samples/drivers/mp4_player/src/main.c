/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Plays an MP4 file from the SD card: the H.264 track goes through the video
 * engine onto the video plane, the AAC track is decoded in software and played
 * by the codec. The audio is the master clock: a picture waits until the sound
 * has reached its time stamp, a picture that is too late is dropped.
 */

#include <errno.h>
#include <pvmp4audiodecoder_api.h>
#include <ff.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/display_sunxi.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/i2s_sunxi.h>
#include <zephyr/drivers/vdec.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/mp4.h>
#include <zephyr/sys/printk.h>

#include "overlay.h"

#define MOUNT_PT	"/SD:"
#define AAC_FRAMES	1024
#define PCM_BLOCK	(AAC_FRAMES * 2 * sizeof(int16_t))
#define PCM_BLOCKS	12
#define PREFILL		6
/* pictures later than this behind the sound are not shown */
#define DROP_US		(2 * 33367)
/* pictures are put up this much before their time, the display waits for a refresh */
#define LEAD_US		8000
#define REPORT_MS	5000

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = MOUNT_PT,
};

/*
 * A file with a read window: the samples of a track are small and spread over
 * the file, a seek in FAT walks the cluster chain, so a window is read at a
 * time and the samples are cut out of it.
 */
struct reader {
	struct fs_file_t f;
	uint8_t *win;
	size_t win_size;
	uint64_t win_off;
	size_t win_len;
};

static struct mp4 mp4;
static struct reader vfile, afile;

K_MEM_SLAB_DEFINE_STATIC(pcm_slab, PCM_BLOCK, PCM_BLOCKS, 4);
K_THREAD_STACK_DEFINE(audio_stack, 8192);
static struct k_thread audio_thread;

static const struct device *const codec_i2s = DEVICE_DT_GET(DT_NODELABEL(audio_codec));
static const struct device *const codec_ctl = DEVICE_DT_GET(DT_NODELABEL(codec_analog));

static volatile bool audio_running;
static volatile bool audio_done;
static volatile uint32_t audio_underruns;
static volatile uint32_t audio_frames_out;

static int file_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	struct reader *r = ctx;
	int ret;
	ssize_t n;

	if (len > r->win_size) {
		ret = fs_seek(&r->f, (off_t)off, FS_SEEK_SET);
		n = ret == 0 ? fs_read(&r->f, buf, len) : ret;
		return n == (ssize_t)len ? 0 : (n < 0 ? (int)n : -EIO);
	}
	if (off < r->win_off || off + len > r->win_off + r->win_len) {
		ret = fs_seek(&r->f, (off_t)off, FS_SEEK_SET);
		if (ret != 0) {
			return ret;
		}
		n = fs_read(&r->f, r->win, r->win_size);
		if (n < (ssize_t)len) {
			r->win_len = 0;
			return n < 0 ? (int)n : -EIO;
		}
		r->win_off = off;
		r->win_len = n;
	}
	memcpy(buf, r->win + (off - r->win_off), len);

	return 0;
}

/* ---- audio ---------------------------------------------------------------------------- */

/* Position of the sound in microseconds, -1 before it started */
static int64_t audio_clock_us(void)
{
	uint32_t blocks, cycle;

	if (!audio_running) {
		return -1;
	}
	i2s_sunxi_codec_tx_position(codec_i2s, &blocks, &cycle);

	return (int64_t)blocks * AAC_FRAMES * 1000000 / mp4.audio.sample_rate +
	       (int64_t)k_cyc_to_us_floor64(k_cycle_get_32() - cycle);
}

static void audio_main(void *a, void *b, void *c)
{
	const struct mp4_track *t = &mp4.audio;
	struct i2s_config cfg = {
		.word_size = 16,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = t->sample_rate,
		.mem_slab = &pcm_slab,
		.block_size = PCM_BLOCK,
		.timeout = 2000,
	};
	tPVMP4AudioDecoderExternal ext = {0};
	void *dec = malloc(PVMP4AudioDecoderGetMemRequirements());
	static int16_t pcm[AAC_FRAMES * 2 * 2];
	struct mp4_iter it;
	struct mp4_sample s;
	uint8_t *buf = malloc(t->max_sample);
	uint32_t skip = t->media_start / AAC_FRAMES;
	int queued = 0, ret;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	ext.desiredChannels = 2;
	ext.outputFormat = OUTPUTFORMAT_16PCM_INTERLEAVED;
	ext.aacPlusEnabled = false;
	ext.pOutputBuffer = pcm;
	ext.pOutputBuffer_plus = pcm + AAC_FRAMES * 2;
	if (dec == NULL || buf == NULL || PVMP4AudioDecoderInitLibrary(&ext, dec) != 0) {
		printk("audio: cannot start the AAC decoder\n");
		goto out;
	}
	/* the AudioSpecificConfig of the track tells the decoder the stream layout */
	ext.pInputBuffer = t->extra;
	ext.inputBufferCurrentLength = t->extra_len;
	ext.inputBufferUsedLength = 0;
	ext.remainderBits = 0;
	if (PVMP4AudioDecoderConfig(&ext, dec) != MP4AUDEC_SUCCESS) {
		printk("audio: bad AudioSpecificConfig\n");
		goto out;
	}
	ret = i2s_configure(codec_i2s, I2S_DIR_TX, &cfg);
	if (ret != 0) {
		printk("audio: i2s_configure %d\n", ret);
		goto out;
	}
	audio_codec_start_output(codec_ctl);

	mp4_iter_init(t, &it);
	while (mp4_next(t, &it, &s) == 0) {
		void *blk;
		int16_t *o;

		if (file_read(&afile, s.offset, buf, s.size) != 0) {
			printk("audio: read error at sample %u\n", s.index);
			break;
		}
		play_stats.sd_bytes += s.size;
		ext.pInputBuffer = buf;
		ext.inputBufferCurrentLength = s.size;
		ext.inputBufferUsedLength = 0;
		ext.remainderBits = 0;
		ret = PVMP4AudioDecodeFrame(&ext, dec);
		if (ret != MP4AUDEC_SUCCESS) {
			printk("audio: decode error %d at sample %u\n", ret, s.index);
			continue;
		}
		if (skip > 0U) {
			/* the encoder delay is not part of the programme */
			skip--;
			continue;
		}
		if (k_mem_slab_alloc(&pcm_slab, &blk, K_SECONDS(2)) != 0) {
			printk("audio: no block\n");
			break;
		}
		o = blk;
		/* desiredChannels is 2: a mono stream comes out duplicated */
		memcpy(o, pcm, PCM_BLOCK);
		ret = i2s_write(codec_i2s, blk, PCM_BLOCK);
		if (ret == -EIO) {
			/* the stream ran dry: start it again */
			audio_underruns++;
			audio_running = false;
			k_mem_slab_free(&pcm_slab, blk);
			i2s_trigger(codec_i2s, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
			queued = 0;
			continue;
		} else if (ret != 0) {
			printk("audio: i2s_write %d\n", ret);
			k_mem_slab_free(&pcm_slab, blk);
			break;
		}
		audio_frames_out += AAC_FRAMES;
		if (!audio_running && ++queued >= PREFILL) {
			ret = i2s_trigger(codec_i2s, I2S_DIR_TX, I2S_TRIGGER_START);
			if (ret == 0) {
				audio_running = true;
			}
		}
	}
	i2s_trigger(codec_i2s, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	k_sleep(K_MSEC(300));
	audio_codec_stop_output(codec_ctl);
out:
	free(dec);
	free(buf);
	audio_done = true;
}

/* ---- video ---------------------------------------------------------------------------- */

static uint8_t annexb_head[512];
static size_t annexb_head_len;
static int nal_len_size;

/* SPS and PPS of the avcC record as Annex B */
static int avcc_parameter_sets(const uint8_t *e, uint32_t len)
{
	size_t o = 5, w = 0;
	int n;

	if (len < 7U || e[0] != 1) {
		return -EINVAL;
	}
	nal_len_size = (e[4] & 3) + 1;
	for (int pass = 0; pass < 2; pass++) {
		n = pass == 0 ? (e[o] & 0x1f) : e[o];
		o++;
		for (int i = 0; i < n; i++) {
			size_t l;

			if (o + 2 > len) {
				return -EINVAL;
			}
			l = (e[o] << 8) | e[o + 1];
			o += 2;
			if (o + l > len || w + 4 + l > sizeof(annexb_head)) {
				return -EINVAL;
			}
			annexb_head[w++] = 0;
			annexb_head[w++] = 0;
			annexb_head[w++] = 0;
			annexb_head[w++] = 1;
			memcpy(annexb_head + w, e + o, l);
			w += l;
			o += l;
		}
	}
	annexb_head_len = w;

	return 0;
}

/* Length prefixed NAL units to start code prefixed ones; returns the new size */
static size_t avcc_to_annexb(const uint8_t *in, size_t len, uint8_t *out, bool with_head)
{
	size_t w = 0;

	if (with_head) {
		memcpy(out, annexb_head, annexb_head_len);
		w = annexb_head_len;
	}
	while (len > (size_t)nal_len_size) {
		size_t l = 0;

		for (int i = 0; i < nal_len_size; i++) {
			l = (l << 8) | *in++;
		}
		len -= nal_len_size;
		if (l > len) {
			break;
		}
		out[w++] = 0;
		out[w++] = 0;
		out[w++] = 0;
		out[w++] = 1;
		memcpy(out + w, in, l);
		w += l;
		in += l;
		len -= l;
	}

	return w;
}

static const struct device *vdec_dev;
static const struct device *disp;
static uint32_t shown_count, dropped_count;
static uint32_t t_get, t_read, t_feed, t_show, t_wait;

static inline uint32_t us_since(uint32_t c0)
{
	return (uint32_t)k_cyc_to_us_floor64(k_cycle_get_32() - c0);
}
static int64_t last_offset_us;

static void show(const struct vdec_frame *f)
{
	struct display_sunxi_yuv yuv = {
		.y = f->plane[0],
		.uv = f->plane[1],
		.width = f->width,
		.height = f->height,
		.stride_y = f->stride[0],
		.stride_uv = f->stride[1],
		.bt709 = true,
		/* the picture is up at the next refresh, the decoder does not wait for it */
		.nonblock = true,
	};

	display_sunxi_show_yuv(disp, &yuv);
}

/*
 * Waits for the time of a picture; false when it is too late to show it.
 * Without sound the clock is the time since the start.
 */
static bool wait_for(int64_t pts_us, int64_t start_ms)
{
	for (;;) {
		int64_t now = audio_running || !audio_done ? audio_clock_us() : -1;
		int64_t diff;

		if (now < 0) {
			if (audio_done) {
				now = (k_uptime_get() - start_ms) * 1000;
			} else {
				k_msleep(2);
				continue;
			}
		}
		diff = pts_us - now;
		last_offset_us = -diff;
		if (diff < -DROP_US) {
			return false;
		}
		if (diff <= LEAD_US) {
			return true;
		}
		k_msleep(MAX(1, (int)((diff - LEAD_US) / 1000)));
	}
}

int main(void)
{
	const struct mp4_track *v;
	const struct vdec_stream_config scfg = {
		.codec = VDEC_CODEC_H264,
		.format = VDEC_FORMAT_NV12,
		.buffer_size = 1024 * 1024,
	};
	struct vdec_frame frame, shown = {0}, older = {0};
	struct vdec_stream *stream;
	struct fs_dirent st;
	struct mp4_iter it;
	struct mp4_sample s;
	uint8_t *raw, *ab;
	size_t ab_len = 0, ab_used = 0;
	int64_t pending_pts = -1, start_ms, last_report;
	bool eof = false, flushed = false, first = true;
	uint32_t report_frames = 0, report_drop = 0;
	int ret;

	vdec_dev = DEVICE_DT_GET(DT_NODELABEL(ve));
	disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(vdec_dev) || !device_is_ready(disp) || !device_is_ready(codec_i2s) ||
	    !device_is_ready(codec_ctl)) {
		printk("a device is not ready\n");
		return 0;
	}
	ret = fs_mount(&mp);
	if (ret == 0) {
		ret = fs_stat(CONFIG_SAMPLE_MP4_FILE, &st);
	}
	fs_file_t_init(&vfile.f);
	fs_file_t_init(&afile.f);
	vfile.win_size = 256 * 1024;
	afile.win_size = 64 * 1024;
	vfile.win = aligned_alloc(64, vfile.win_size);
	afile.win = aligned_alloc(64, afile.win_size);
	if (vfile.win == NULL || afile.win == NULL) {
		ret = -ENOMEM;
	}
	if (ret == 0) {
		ret = fs_open(&vfile.f, CONFIG_SAMPLE_MP4_FILE, FS_O_READ);
	}
	if (ret == 0) {
		ret = fs_open(&afile.f, CONFIG_SAMPLE_MP4_FILE, FS_O_READ);
	}
	if (ret != 0) {
		printk("cannot open %s: %d\n", CONFIG_SAMPLE_MP4_FILE, ret);
		return 0;
	}
	ret = mp4_open(&mp4, file_read, &vfile, st.size);
	if (ret != 0 || mp4.video.codec != MP4_CODEC_H264) {
		printk("%s: no usable video track (%d)\n", CONFIG_SAMPLE_MP4_FILE, ret);
		return 0;
	}
	v = &mp4.video;
	ret = avcc_parameter_sets(v->extra, v->extra_len);
	if (ret != 0) {
		printk("bad avcC record\n");
		return 0;
	}
	printk("playing %s: %ux%u, %u frames%s\n", CONFIG_SAMPLE_MP4_FILE, v->width, v->height,
	       v->sample_count, mp4.audio.codec == MP4_CODEC_AAC ? ", with sound" : ", no sound");

	if (mp4.audio.codec == MP4_CODEC_AAC &&
	    (mp4.audio.sample_rate == 48000U || mp4.audio.sample_rate == 44100U)) {
		k_thread_create(&audio_thread, audio_stack, K_THREAD_STACK_SIZEOF(audio_stack),
				audio_main, NULL, NULL, NULL, 3, 0, K_NO_WAIT);
	} else {
		if (mp4.audio.codec == MP4_CODEC_AAC) {
			printk("the audio is %u Hz, only 48000 and 44100 Hz are played\n",
			       mp4.audio.sample_rate);
		}
		audio_done = true;
	}

	ret = vdec_stream_open(vdec_dev, &scfg, &stream);
	if (ret != 0) {
		printk("cannot open the decoder: %d\n", ret);
		return 0;
	}
	raw = malloc(v->max_sample);
	ab = malloc(v->max_sample + 1024);
	if (raw == NULL || ab == NULL) {
		printk("no memory\n");
		return 0;
	}
	mp4_iter_init(v, &it);
	play_stats.width = v->width;
	play_stats.height = v->height;
	overlay_start();
	start_ms = last_report = k_uptime_get();

	while (true) {
		uint32_t c0 = k_cycle_get_32();

		ret = vdec_stream_get_frame(vdec_dev, stream, &frame);
		t_get += us_since(c0);
		if (ret == 0) {
			int64_t pts_us = frame.pts >= 0 ? frame.pts : 0;

			c0 = k_cycle_get_32();
			bool on_time = wait_for(pts_us, start_ms);

			t_wait += us_since(c0);
			if (on_time) {
				c0 = k_cycle_get_32();
				show(&frame);
				t_show += us_since(c0);
				/* the picture before the last may still be scanned out */
				if (older.priv != NULL) {
					vdec_frame_release(vdec_dev, &older);
				}
				older = shown;
				shown = frame;
				shown_count++;
				report_frames++;
				play_stats.frames = shown_count;
			} else {
				vdec_frame_release(vdec_dev, &frame);
				dropped_count++;
				report_drop++;
				play_stats.late = dropped_count;
			}
			if (k_uptime_get() - last_report >= REPORT_MS) {
				int ms = (int)(k_uptime_get() - last_report);

				printk("%u shown (%d.%02d fps), %u dropped, A/V offset %d ms, audio underruns %u\n",
				       shown_count, report_frames * 1000 / ms,
				       (report_frames * 100000 / ms) % 100, dropped_count,
				       (int)(last_offset_us / 1000), audio_underruns);
				printk("  per %d ms: decode %u ms, read+convert %u ms, feed %u ms, show %u ms, wait %u ms\n",
				       ms, t_get / 1000, t_read / 1000, t_feed / 1000, t_show / 1000,
				       t_wait / 1000);
				t_get = t_read = t_feed = t_show = t_wait = 0;
				report_frames = report_drop = 0;
				last_report = k_uptime_get();
			}
			continue;
		}
		if (ret == -ENODATA) {
			break;
		}
		if (ret == -EBUSY) {
			k_msleep(2);
			continue;
		}
		if (ret != -EAGAIN) {
			printk("decode error %d\n", ret);
			break;
		}

		/* the decoder wants data */
		if (ab_used >= ab_len) {
			c0 = k_cycle_get_32();
			if (mp4_next(v, &it, &s) != 0) {
				eof = true;
			} else {
				if (file_read(&vfile, s.offset, raw, s.size) != 0) {
					printk("video: read error at sample %u\n", s.index);
					break;
				}
				play_stats.sd_bytes += s.size;
				ab_len = avcc_to_annexb(raw, s.size, ab, first || s.sync);
				ab_used = 0;
				first = false;
				t_read += us_since(c0);
				pending_pts = (s.pts - v->media_start) * 1000000 / v->timescale;
				if (pending_pts < 0) {
					pending_pts = 0;
				}
			}
		}
		if (ab_used < ab_len) {
			size_t used = 0;

			c0 = k_cycle_get_32();
			ret = vdec_stream_feed(vdec_dev, stream, ab + ab_used, ab_len - ab_used,
					       pending_pts, &used);
			t_feed += us_since(c0);
			if (ret != 0 && ret != -EAGAIN) {
				printk("feed error %d\n", ret);
				ab_used = ab_len;
			} else {
				ab_used += used;
			}
		} else if (eof && !flushed) {
			vdec_stream_flush(vdec_dev, stream);
			flushed = true;
		}
	}
	if (older.priv != NULL) {
		vdec_frame_release(vdec_dev, &older);
	}
	if (shown.priv != NULL) {
		vdec_frame_release(vdec_dev, &shown);
	}
	printk("done: %u shown, %u dropped, %u audio underruns\n", shown_count, dropped_count,
	       audio_underruns);
	while (!audio_done) {
		k_msleep(100);
	}
	k_sleep(K_FOREVER);

	return 0;
}
