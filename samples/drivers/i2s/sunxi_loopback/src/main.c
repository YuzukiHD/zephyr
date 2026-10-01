/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Transmits a counting pattern on I2S0 with the internal loopback on and
 * checks that the receiver gets the same frames without a gap or a repeat.
 */

#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define RATE		48000
#define FRAMES		480
#define BLOCK_SIZE	(FRAMES * 2 * sizeof(int16_t))
#define TX_BLOCKS	8
#define RX_BLOCKS	8
#define TEST_BLOCKS	200

K_MEM_SLAB_DEFINE_STATIC(tx_slab, BLOCK_SIZE, TX_BLOCKS, 4);
K_MEM_SLAB_DEFINE_STATIC(rx_slab, BLOCK_SIZE, RX_BLOCKS, 4);

static const struct device *const i2s = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static uint16_t counter;

static int send_block(void)
{
	void *block;
	int16_t *p;

	if (k_mem_slab_alloc(&tx_slab, &block, K_MSEC(500)) != 0) {
		return -ENOMEM;
	}
	p = block;
	for (int i = 0; i < FRAMES; i++) {
		p[2 * i] = (int16_t)counter;
		p[2 * i + 1] = (int16_t)~counter;
		counter++;
	}

	return i2s_write(i2s, block, BLOCK_SIZE);
}

int main(void)
{
	struct i2s_config cfg = {
		.word_size = 16,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_LOOPBACK,
		.frame_clk_freq = RATE,
		.block_size = BLOCK_SIZE,
		.timeout = 1000,
	};
	uint32_t errors = 0, frames = 0, gaps = 0, zeros = 0;
	int expect = -1;
	int64_t t0;
	int ret;

	if (!device_is_ready(i2s)) {
		printk("i2s0 not ready\n");
		return 0;
	}
	cfg.mem_slab = &tx_slab;
	ret = i2s_configure(i2s, I2S_DIR_TX, &cfg);
	printk("tx configure: %d\n", ret);
	cfg.mem_slab = &rx_slab;
	ret |= i2s_configure(i2s, I2S_DIR_RX, &cfg);
	printk("rx configure: %d\n", ret);
	if (ret != 0) {
		return 0;
	}

	/* the DMA ring takes the first blocks, the rest is slack against scheduling delays */
	for (int i = 0; i < 7; i++) {
		send_block();
	}
	ret = i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_START);
	ret |= i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_START);
	printk("start: %d\n", ret);
	t0 = k_uptime_get();

	for (int b = 0; b < TEST_BLOCKS && ret == 0; b++) {
		void *block;
		size_t size;
		const int16_t *p;

		ret = send_block();
		if (ret != 0) {
			printk("write: %d\n", ret);
			break;
		}
		ret = i2s_read(i2s, &block, &size);
		if (ret != 0) {
			printk("read: %d at block %d\n", ret, b);
			break;
		}
		p = block;
		for (size_t i = 0; i < size / 4; i++) {
			uint16_t l = (uint16_t)p[2 * i], r = (uint16_t)p[2 * i + 1];

			if (expect < 0) {
				if (r == (uint16_t)~l && l != 0U) {
					expect = l;
				} else {
					continue;
				}
			}
			if (l != (uint16_t)expect || r != (uint16_t)~expect) {
				errors++;
				if (l == 0U && r == 0U) {
					zeros++;
				} else if (gaps++ < 12U) {
					printk("block %d frame %zu (#%u): got %04x/%04x want %04x/%04x\n",
					       b, i, frames, l, r, (uint16_t)expect, (uint16_t)~expect);
				}
				if (r == (uint16_t)~l) {
					expect = l;
				}
			}
			expect = (expect + 1) & 0xffff;
			frames++;
		}
		k_mem_slab_free(&rx_slab, block);
	}
	printk("%u frames compared in %d ms, %u mismatches (%u slips, %u silent frames)\n", frames,
	       (int)(k_uptime_get() - t0), errors, gaps, zeros);
	i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_DROP);
	i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_DROP);
	printk("loopback %s\n", errors == 0U && frames > 0U ? "PASS" : "FAIL");
	k_sleep(K_FOREVER);

	return 0;
}
