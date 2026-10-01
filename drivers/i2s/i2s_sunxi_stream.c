/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/logging/log.h>

#include "i2s_sunxi_stream.h"

LOG_MODULE_REGISTER(i2s_sunxi, CONFIG_I2S_LOG_LEVEL);

void sunxi_i2s_stream_init(struct sunxi_i2s_stream *s, const struct device *owner,
			   const struct sunxi_i2s_stream_ops *ops, const struct device *dma,
			   uint32_t channel, uint32_t slot, uintptr_t fifo, enum i2s_dir dir,
			   uint8_t periods)
{
	s->owner = owner;
	s->ops = ops;
	s->dma = dma;
	s->channel = channel;
	s->slot = slot;
	s->fifo = fifo;
	s->dir = dir;
	s->periods = periods;
	s->burst = 4;
	s->state = I2S_STATE_NOT_READY;
	k_msgq_init(&s->queue, (char *)s->items, sizeof(struct sunxi_i2s_item),
		    CONFIG_I2S_SUNXI_QUEUE_LEN);
}

static void queue_purge(struct sunxi_i2s_stream *s)
{
	struct sunxi_i2s_item item;

	while (k_msgq_get(&s->queue, &item, K_NO_WAIT) == 0) {
		k_mem_slab_free(s->cfg.mem_slab, item.block);
	}
}

static uint8_t *period_ptr(struct sunxi_i2s_stream *s, uint32_t idx)
{
	return s->ring + (size_t)idx * s->period_size;
}

/* Put the next queued block (or silence) into a period of a TX ring; true if data was used */
static bool tx_fill(struct sunxi_i2s_stream *s, uint32_t idx)
{
	struct sunxi_i2s_item item;
	uint8_t *p = period_ptr(s, idx);
	bool data = k_msgq_get(&s->queue, &item, K_NO_WAIT) == 0;

	if (data) {
		memcpy(p, item.block, MIN(item.size, s->period_size));
		if (item.size < s->period_size) {
			memset(p + item.size, 0, s->period_size - item.size);
		}
		k_mem_slab_free(s->cfg.mem_slab, item.block);
	} else {
		memset(p, 0, s->period_size);
	}
	sys_cache_data_flush_range(p, s->period_size);

	return data;
}

static void hw_stop(struct sunxi_i2s_stream *s)
{
	if (s->ops->stop != NULL) {
		s->ops->stop(s->owner, s);
	}
	dma_stop(s->dma, s->channel);
}

static void stream_halt(struct sunxi_i2s_stream *s, enum i2s_state next)
{
	hw_stop(s);
	s->stopping = false;
	s->draining = false;
	s->state = next;
}

static void dma_cb(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	struct sunxi_i2s_stream *s = user_data;
	k_spinlock_key_t key = k_spin_lock(&s->lock);
	uint32_t idx = s->done++ % s->periods;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel);

	if (status < 0) {
		LOG_ERR("DMA error %d", status);
		stream_halt(s, I2S_STATE_ERROR);
		goto out;
	}
	if (s->state != I2S_STATE_RUNNING && s->state != I2S_STATE_STOPPING) {
		goto out;
	}
	s->blocks++;

	if (s->dir == I2S_DIR_TX) {
		bool data;

		if (s->stopping) {
			stream_halt(s, I2S_STATE_READY);
			goto out;
		}
		data = tx_fill(s, idx);
		if (s->draining) {
			if (data) {
				s->drain_left = s->periods;
			} else if (--s->drain_left == 0U) {
				stream_halt(s, I2S_STATE_READY);
			}
		} else if (!data) {
			/* the application did not keep up: silence went out */
			s->underruns++;
			stream_halt(s, I2S_STATE_ERROR);
		}
	} else {
		struct sunxi_i2s_item item;
		uint8_t *p;

		/*
		 * The completion of a period is reported while the last words of it can
		 * still be on their way to memory: take the period before the one just
		 * finished, it is certainly complete.
		 */
		if (s->stopping) {
			stream_halt(s, I2S_STATE_READY);
			goto out;
		}
		if (s->done < 2U) {
			goto out;
		}
		p = period_ptr(s, (idx + s->periods - 1U) % s->periods);
		sys_cache_data_invd_range(p, s->period_size);
		if (k_mem_slab_alloc(s->cfg.mem_slab, &item.block, K_NO_WAIT) != 0) {
			s->underruns++;
			stream_halt(s, I2S_STATE_ERROR);
			goto out;
		}
		memcpy(item.block, p, s->period_size);
		item.size = s->period_size;
		if (k_msgq_put(&s->queue, &item, K_NO_WAIT) != 0) {
			k_mem_slab_free(s->cfg.mem_slab, item.block);
			s->underruns++;
			stream_halt(s, I2S_STATE_ERROR);
		}
	}
out:
	k_spin_unlock(&s->lock, key);
}

int sunxi_i2s_stream_configure(struct sunxi_i2s_stream *s, const struct i2s_config *cfg,
			       uint8_t width)
{
	if (s->state != I2S_STATE_NOT_READY && s->state != I2S_STATE_READY) {
		return -EINVAL;
	}
	if (cfg->frame_clk_freq == 0U) {
		queue_purge(s);
		k_free(s->ring);
		s->ring = NULL;
		s->state = I2S_STATE_NOT_READY;
		return 0;
	}
	if (cfg->mem_slab == NULL || cfg->block_size == 0U || (cfg->block_size % 4U) != 0U ||
	    cfg->block_size > cfg->mem_slab->info.block_size) {
		return -EINVAL;
	}

	queue_purge(s);
	k_free(s->ring);
	s->period_size = ROUND_UP(cfg->block_size, 64U);
	s->ring = k_aligned_alloc(64, (size_t)s->periods * s->period_size);
	if (s->ring == NULL) {
		s->state = I2S_STATE_NOT_READY;
		return -ENOMEM;
	}
	s->cfg = *cfg;
	s->width = width;
	s->state = I2S_STATE_READY;

	return 0;
}

static int dma_arm(struct sunxi_i2s_stream *s)
{
	struct dma_block_config blocks[CONFIG_DMA_SUNXI_MAX_BLOCKS] = {0};
	bool tx = s->dir == I2S_DIR_TX;
	struct dma_config cfg = {
		.dma_slot = s->slot,
		.channel_direction = tx ? MEMORY_TO_PERIPHERAL : PERIPHERAL_TO_MEMORY,
		.complete_callback_en = 1U,
		.source_data_size = s->width,
		.dest_data_size = s->width,
		.source_burst_length = s->burst,
		.dest_burst_length = s->burst,
		.block_count = s->periods,
		.cyclic = 1U,
		.head_block = blocks,
		.dma_callback = dma_cb,
		.user_data = s,
	};

	if (s->periods > CONFIG_DMA_SUNXI_MAX_BLOCKS) {
		return -ENOTSUP;
	}
	for (uint32_t i = 0; i < s->periods; i++) {
		uint32_t mem = (uint32_t)(uintptr_t)period_ptr(s, i);

		blocks[i].block_size = s->period_size;
		if (tx) {
			blocks[i].source_address = mem;
			blocks[i].dest_address = (uint32_t)s->fifo;
			blocks[i].source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
			blocks[i].dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
		} else {
			blocks[i].source_address = (uint32_t)s->fifo;
			blocks[i].dest_address = mem;
			blocks[i].source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
			blocks[i].dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
		}
		blocks[i].next_block = i + 1U < s->periods ? &blocks[i + 1U] : NULL;
	}

	return dma_config(s->dma, s->channel, &cfg);
}

static int stream_start(struct sunxi_i2s_stream *s)
{
	int ret;

	s->done = 0U;
	s->stopping = false;
	s->draining = false;
	if (s->dir == I2S_DIR_TX) {
		if (k_msgq_num_used_get(&s->queue) == 0U) {
			return -EIO;
		}
		for (uint32_t i = 0; i < s->periods; i++) {
			(void)tx_fill(s, i);
		}
	} else {
		memset(s->ring, 0, (size_t)s->periods * s->period_size);
		sys_cache_data_flush_and_invd_range(s->ring, (size_t)s->periods * s->period_size);
	}
	ret = dma_arm(s);
	if (ret != 0) {
		return ret;
	}
	s->state = I2S_STATE_RUNNING;
	ret = dma_start(s->dma, s->channel);
	if (ret == 0 && s->ops->start != NULL) {
		ret = s->ops->start(s->owner, s);
	}
	if (ret != 0) {
		dma_stop(s->dma, s->channel);
		s->state = I2S_STATE_READY;
	}

	return ret;
}

int sunxi_i2s_stream_trigger(struct sunxi_i2s_stream *s, enum i2s_trigger_cmd cmd)
{
	k_spinlock_key_t key = k_spin_lock(&s->lock);
	int ret = 0;

	switch (cmd) {
	case I2S_TRIGGER_START:
		if (s->state != I2S_STATE_READY) {
			ret = -EIO;
		} else {
			ret = stream_start(s);
		}
		break;
	case I2S_TRIGGER_STOP:
		if (s->state != I2S_STATE_RUNNING) {
			ret = -EIO;
		} else {
			/* the block in flight is finished first */
			s->stopping = true;
			s->state = I2S_STATE_STOPPING;
		}
		break;
	case I2S_TRIGGER_DRAIN:
		if (s->state != I2S_STATE_RUNNING) {
			ret = -EIO;
		} else if (s->dir == I2S_DIR_TX) {
			s->draining = true;
			s->drain_left = s->periods;
			s->state = I2S_STATE_STOPPING;
		} else {
			s->stopping = true;
			s->state = I2S_STATE_STOPPING;
		}
		break;
	case I2S_TRIGGER_DROP:
		if (s->state == I2S_STATE_NOT_READY) {
			ret = -EIO;
		} else {
			if (s->state == I2S_STATE_RUNNING || s->state == I2S_STATE_STOPPING) {
				hw_stop(s);
			}
			queue_purge(s);
			s->stopping = false;
			s->draining = false;
			s->state = I2S_STATE_READY;
		}
		break;
	case I2S_TRIGGER_PREPARE:
		if (s->state != I2S_STATE_ERROR) {
			ret = -EIO;
		} else {
			queue_purge(s);
			s->state = I2S_STATE_READY;
		}
		break;
	default:
		ret = -EINVAL;
	}
	k_spin_unlock(&s->lock, key);

	return ret;
}

int sunxi_i2s_stream_write(struct sunxi_i2s_stream *s, void *block, size_t size)
{
	struct sunxi_i2s_item item = {.block = block, .size = size};

	if (s->state != I2S_STATE_READY && s->state != I2S_STATE_RUNNING) {
		return -EIO;
	}
	if (size > s->cfg.block_size) {
		return -EINVAL;
	}
	if (k_msgq_put(&s->queue, &item, SYS_TIMEOUT_MS(s->cfg.timeout)) != 0) {
		return -EAGAIN;
	}

	return 0;
}

int sunxi_i2s_stream_read(struct sunxi_i2s_stream *s, void **block, size_t *size)
{
	struct sunxi_i2s_item item;
	int ret;

	if (s->state == I2S_STATE_NOT_READY) {
		return -EIO;
	}
	ret = k_msgq_get(&s->queue, &item, s->state == I2S_STATE_ERROR ? K_NO_WAIT :
			 SYS_TIMEOUT_MS(s->cfg.timeout));
	if (ret != 0) {
		return s->state == I2S_STATE_READY || s->state == I2S_STATE_ERROR ? -EIO : -EAGAIN;
	}
	*block = item.block;
	*size = item.size;

	return 0;
}
