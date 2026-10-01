/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Plays a sine tone on the on-chip codec and measures the real frame rate. */

#include <errno.h>
#include <stdint.h>
#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

#define RATE		CONFIG_SAMPLE_TONE_RATE
#define FRAMES		(RATE / 50)		/* 20 ms blocks */
#define BLOCK_SIZE	(FRAMES * 2 * sizeof(int16_t))
#define BLOCKS		6
#define TONE_HZ		440

K_MEM_SLAB_DEFINE_STATIC(slab, BLOCK_SIZE, BLOCKS, 4);

static const struct device *const codec_i2s = DEVICE_DT_GET(DT_NODELABEL(audio_codec));
static const struct device *const analog = DEVICE_DT_GET(DT_NODELABEL(codec_analog));

static int16_t table[RATE / TONE_HZ + 1];

static void fill(int16_t *out, uint32_t *phase, uint32_t period)
{
	for (int i = 0; i < FRAMES; i++) {
		out[2 * i] = table[*phase];
		out[2 * i + 1] = table[*phase];
		*phase = (*phase + 1) % period;
	}
}

int main(void)
{
	struct i2s_config cfg = {
		.word_size = 16,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = RATE,
		.mem_slab = &slab,
		.block_size = BLOCK_SIZE,
		.timeout = 1000,
	};
	uint32_t period = RATE / TONE_HZ, phase = 0;
	int64_t t0, t_mark = 0;
	int ret, blocks = 0, mark_blocks = 0;

	if (!device_is_ready(codec_i2s) || !device_is_ready(analog)) {
		printk("codec not ready\n");
		return 0;
	}
	for (uint32_t i = 0; i < period; i++) {
		/* Bhaskara's sine approximation on each half wave, good to 0.2 percent */
		double x = (double)(i % (period / 2)) / (period / 2) * 3.14159265358979;
		double y = 16.0 * x * (3.14159265358979 - x) /
			   (5.0 * 3.14159265358979 * 3.14159265358979 -
			    4.0 * x * (3.14159265358979 - x));

		table[i] = (int16_t)(12000.0 * (i < period / 2 ? y : -y));
	}

	ret = i2s_configure(codec_i2s, I2S_DIR_TX, &cfg);
	printk("configure: %d\n", ret);
	if (ret != 0) {
		return 0;
	}
	audio_codec_start_output(analog);

	for (int i = 0; i < 3; i++) {
		void *block;

		k_mem_slab_alloc(&slab, &block, K_FOREVER);
		fill(block, &phase, period);
		ret = i2s_write(codec_i2s, block, BLOCK_SIZE);
		if (ret != 0) {
			printk("write: %d\n", ret);
			return 0;
		}
	}
	ret = i2s_trigger(codec_i2s, I2S_DIR_TX, I2S_TRIGGER_START);
	printk("start: %d\n", ret);
	if (ret != 0) {
		return 0;
	}
	t0 = k_uptime_get();
	while (k_uptime_get() - t0 < CONFIG_SAMPLE_TONE_SECONDS * 1000) {
		void *block;

		if (k_mem_slab_alloc(&slab, &block, K_MSEC(1000)) != 0) {
			printk("no block\n");
			break;
		}
		fill(block, &phase, period);
		ret = i2s_write(codec_i2s, block, BLOCK_SIZE);
		if (ret != 0) {
			printk("write failed: %d after %d blocks\n", ret, blocks);
			k_mem_slab_free(&slab, block);
			break;
		}
		blocks++;
		if (t_mark == 0 && k_uptime_get() - t0 > 2000) {
			t_mark = k_uptime_get();
			mark_blocks = blocks;
		}
	}
	{
		int64_t ms = k_uptime_get() - t0;

		int64_t dt = k_uptime_get() - t_mark;

		printk("%d blocks of %d frames in %d ms\n", blocks, FRAMES, (int)ms);
		printk("steady state: %d Hz (%d blocks in %d ms)\n",
		       (int)((blocks - mark_blocks) * FRAMES * 1000LL / dt), blocks - mark_blocks,
		       (int)dt);
		printk("codec: DPC %08x VOL %08x FIFO_CTL %08x FIFO_STA %08x\n",
		       sys_read32(0x02030000), sys_read32(0x02030004), sys_read32(0x02030010),
		       sys_read32(0x02030014));
		printk("analog: DAC %08x RAMP %08x HP2 %08x; PLL_AUDIO1 %08x DAC clk %08x\n",
		       sys_read32(0x02030310), sys_read32(0x0203031c), sys_read32(0x02030340),
		       sys_read32(0x02001080), sys_read32(0x02001a50));
	}
	i2s_trigger(codec_i2s, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	k_sleep(K_MSEC(200));
	audio_codec_stop_output(analog);

	/* capture from the microphone input: level of five seconds in 200 ms steps */
	{
		struct i2s_config rx = cfg;
		int64_t rt0;
		int n = 0;

		rx.channels = 1;
		rx.block_size = RATE / 5 * sizeof(int16_t);
		K_MEM_SLAB_DEFINE_STATIC(rx_slab, RATE / 5 * sizeof(int16_t), 6, 4);
		rx.mem_slab = &rx_slab;
		ret = i2s_configure(codec_i2s, I2S_DIR_RX, &rx);
		printk("rx configure: %d\n", ret);
		audio_codec_route_input(analog, AUDIO_CHANNEL_ALL, 1);
		ret = i2s_trigger(codec_i2s, I2S_DIR_RX, I2S_TRIGGER_START);
		printk("rx start: %d\n", ret);
		rt0 = k_uptime_get();
		while (ret == 0 && n < 25) {
			void *block;
			size_t size;
			int16_t *p;
			int32_t min = 32767, max = -32768;
			int64_t sum = 0;

			ret = i2s_read(codec_i2s, &block, &size);
			if (ret != 0) {
				printk("rx read: %d\n", ret);
				break;
			}
			p = block;
			for (size_t i = 0; i < size / 2; i++) {
				min = MIN(min, p[i]);
				max = MAX(max, p[i]);
				sum += p[i];
			}
			printk("rx %2d: %zu bytes min %d max %d mean %d\n", n, size, min, max,
			       (int)(sum / (int)(size / 2)));
			k_mem_slab_free(&rx_slab, block);
			n++;
		}
		printk("rx %d blocks in %d ms\n", n, (int)(k_uptime_get() - rt0));
		i2s_trigger(codec_i2s, I2S_DIR_RX, I2S_TRIGGER_DROP);
	}
	printk("done\n");
	k_sleep(K_FOREVER);

	return 0;
}
