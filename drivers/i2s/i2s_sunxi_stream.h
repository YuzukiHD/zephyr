/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * One direction of an audio interface (codec DAC / ADC, I2S0, OWA) on top of
 * the DMA ring. The user blocks of the I2S API are copied into the periods of
 * a cyclic DMA transfer (TX) or out of them (RX); the DMA driver calls back
 * after every period.
 */

#ifndef ZEPHYR_DRIVERS_I2S_I2S_SUNXI_STREAM_H_
#define ZEPHYR_DRIVERS_I2S_I2S_SUNXI_STREAM_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>

struct sunxi_i2s_stream;

struct sunxi_i2s_stream_ops {
	/* the interface side starts / stops pulling data, DMA is already armed / still running */
	int (*start)(const struct device *dev, struct sunxi_i2s_stream *s);
	void (*stop)(const struct device *dev, struct sunxi_i2s_stream *s);
};

struct sunxi_i2s_item {
	void *block;
	size_t size;
};

struct sunxi_i2s_stream {
	const struct device *owner;
	const struct device *dma;
	const struct sunxi_i2s_stream_ops *ops;
	uintptr_t fifo;
	uint32_t channel;
	uint32_t slot;
	enum i2s_dir dir;
	uint8_t periods;
	uint8_t burst;		/* DMA burst length in FIFO accesses */

	struct i2s_config cfg;
	enum i2s_state state;
	struct k_spinlock lock;

	uint8_t *ring;
	size_t period_size;
	uint32_t done;
	uint8_t width;
	bool stopping;
	bool draining;
	uint8_t drain_left;

	struct k_msgq queue;
	struct sunxi_i2s_item items[CONFIG_I2S_SUNXI_QUEUE_LEN];

	uint32_t blocks;
	uint32_t underruns;
};

void sunxi_i2s_stream_init(struct sunxi_i2s_stream *s, const struct device *owner,
			   const struct sunxi_i2s_stream_ops *ops, const struct device *dma,
			   uint32_t channel, uint32_t slot, uintptr_t fifo, enum i2s_dir dir,
			   uint8_t periods);

/* Takes the configuration; @p width is the bus width in bytes (2 or 4) of one FIFO access */
int sunxi_i2s_stream_configure(struct sunxi_i2s_stream *s, const struct i2s_config *cfg,
			       uint8_t width);
int sunxi_i2s_stream_trigger(struct sunxi_i2s_stream *s, enum i2s_trigger_cmd cmd);
int sunxi_i2s_stream_write(struct sunxi_i2s_stream *s, void *block, size_t size);
int sunxi_i2s_stream_read(struct sunxi_i2s_stream *s, void **block, size_t *size);

#endif /* ZEPHYR_DRIVERS_I2S_I2S_SUNXI_STREAM_H_ */
