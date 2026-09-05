/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT allwinner_sunxi_dma

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(dma_sunxi, CONFIG_DMA_LOG_LEVEL);

#define SUNXI_DMA_COMMON_GATE	0x28U
#define SUNXI_DMA_SECURE	0x20U
#define SUNXI_DMA_IRQ_EN	0x00U
#define SUNXI_DMA_IRQ_STAT	0x10U
#define SUNXI_DMA_CH_STRIDE	0x40U
#define SUNXI_DMA_CH_BASE	0x100U
#define SUNXI_DMA_ENABLE	0x00U
#define SUNXI_DMA_PAUSE	0x04U
#define SUNXI_DMA_LLI_ADDR	0x08U
#define SUNXI_DMA_CNT	0x18U

#define SUNXI_DMA_IRQ_GROUPS	2U
#define SUNXI_DMA_CHANNELS	12U
#define SUNXI_DMA_IRQ_QUEUE	BIT(2)
#define SUNXI_DMA_IRQ_TIMEOUT	BIT(3)

#define SUNXI_DMA_CH_START	1U
#define SUNXI_DMA_CH_STOP	0U
#define SUNXI_DMA_CH_PAUSE	1U
#define SUNXI_DMA_CH_RESUME	0U

#define SUNXI_DMA_LINK_END	0xfffff800U
#define SUNXI_DMA_MAX_BLOCK	0x00ffffffU

#define SUNXI_DMA_SRC_WIDTH(n)	((n) << 9)
#define SUNXI_DMA_SRC_BURST(n)	((n) << 6)
#define SUNXI_DMA_SRC_IO	BIT(8)
#define SUNXI_DMA_SRC_LINEAR	0U
#define SUNXI_DMA_SRC_DRQ(n)	((n) << 0)
#define SUNXI_DMA_DST_WIDTH(n)	((n) << 25)
#define SUNXI_DMA_DST_BURST(n)	((n) << 22)
#define SUNXI_DMA_DST_IO	BIT(24)
#define SUNXI_DMA_DST_LINEAR	0U
#define SUNXI_DMA_DST_DRQ(n)	((n) << 16)

#define SUNXI_DMA_DRQ_SDRAM	0U
struct sunxi_dma_lli {
	uint32_t cfg;
	uint32_t src;
	uint32_t dst;
	uint32_t len;
	uint32_t para;
	uint32_t next;
};

struct sunxi_dma_channel {
	struct sunxi_dma_lli lli __aligned(64);
	struct dma_config config;
	struct dma_block_config block;
	dma_callback_t callback;
	void *user_data;
	uint32_t size;
	bool configured;
	bool busy;
};

struct sunxi_dma_config {
	uintptr_t base;
	const struct device *clock_dev;
	uint32_t bus_clock_id;
	uint32_t mbus_clock_id;
	struct reset_dt_spec reset;
	uint32_t channels;
	uint32_t requests;
};

struct sunxi_dma_data {
	struct k_spinlock lock;
	struct sunxi_dma_channel channel[SUNXI_DMA_CHANNELS];
};

static inline uintptr_t sunxi_dma_ch_addr(const struct sunxi_dma_config *cfg,
						 uint32_t channel, uint32_t offset)
{
	return cfg->base + SUNXI_DMA_CH_BASE + channel * SUNXI_DMA_CH_STRIDE + offset;
}

static inline uint32_t sunxi_dma_read(const struct sunxi_dma_config *cfg,
					     uint32_t offset)
{
	return sys_read32(cfg->base + offset);
}

static inline void sunxi_dma_write(const struct sunxi_dma_config *cfg,
					   uint32_t offset, uint32_t value)
{
	sys_write32(value, cfg->base + offset);
}

static inline uint32_t sunxi_dma_width(uint32_t bytes)
{
	switch (bytes) {
	case 1U:
		return 0U;
	case 2U:
		return 1U;
	case 4U:
		return 2U;
	default:
		return UINT32_MAX;
	}
}

static int sunxi_dma_burst(uint32_t bytes)
{
	switch (bytes) {
	case 1U:
	case 0U:
		return 0;
	case 4U:
		return 1;
	case 8U:
		return 2;
	case 16U:
		return 3;
	default:
		return -EINVAL;
	}
}

static inline uint32_t sunxi_dma_irq_group(uint32_t channel)
{
	return channel / 8U;
}

static inline uint32_t sunxi_dma_irq_shift(uint32_t channel)
{
	return (channel % 8U) * 4U;
}

static void sunxi_dma_irq_enable_channel(const struct sunxi_dma_config *cfg,
						 uint32_t channel, bool enable)
{
	uint32_t group = sunxi_dma_irq_group(channel);
	uint32_t shift = sunxi_dma_irq_shift(channel);
	uint32_t mask = (SUNXI_DMA_IRQ_QUEUE | SUNXI_DMA_IRQ_TIMEOUT) << shift;
	uint32_t reg = sunxi_dma_read(cfg, SUNXI_DMA_IRQ_EN + group * 4U);

	if (enable) {
		reg |= mask;
	} else {
		reg &= ~mask;
	}
	sunxi_dma_write(cfg, SUNXI_DMA_IRQ_EN + group * 4U, reg);
}

static int sunxi_dma_configure_lli(struct sunxi_dma_channel *chan)
{
	const struct dma_config *config = &chan->config;
	const struct dma_block_config *block = &chan->block;
	uint32_t src_width = sunxi_dma_width(config->source_data_size);
	uint32_t dst_width = sunxi_dma_width(config->dest_data_size);
	int src_burst = sunxi_dma_burst(config->source_burst_length);
	int dst_burst = sunxi_dma_burst(config->dest_burst_length);
	uint32_t cfg;

	if (src_width == UINT32_MAX || dst_width == UINT32_MAX ||
		src_burst < 0 || dst_burst < 0) {
		return -EINVAL;
	}

	cfg = SUNXI_DMA_SRC_WIDTH(src_width) | SUNXI_DMA_SRC_BURST(src_burst) |
		SUNXI_DMA_DST_WIDTH(dst_width) | SUNXI_DMA_DST_BURST(dst_burst);

	switch (config->channel_direction) {
	case MEMORY_TO_MEMORY:
		cfg |= SUNXI_DMA_SRC_DRQ(SUNXI_DMA_DRQ_SDRAM) |
			SUNXI_DMA_DST_DRQ(SUNXI_DMA_DRQ_SDRAM) |
			SUNXI_DMA_SRC_LINEAR | SUNXI_DMA_DST_LINEAR;
		break;
	case MEMORY_TO_PERIPHERAL:
		cfg |= SUNXI_DMA_SRC_DRQ(SUNXI_DMA_DRQ_SDRAM) |
			SUNXI_DMA_DST_DRQ(config->dma_slot) |
			SUNXI_DMA_SRC_LINEAR | SUNXI_DMA_DST_IO;
		break;
	case PERIPHERAL_TO_MEMORY:
		cfg |= SUNXI_DMA_SRC_DRQ(config->dma_slot) |
			SUNXI_DMA_DST_DRQ(SUNXI_DMA_DRQ_SDRAM) |
			SUNXI_DMA_SRC_IO | SUNXI_DMA_DST_LINEAR;
		break;
	default:
		return -ENOTSUP;
	}

	if (block->source_addr_adj == DMA_ADDR_ADJ_NO_CHANGE) {
		cfg |= SUNXI_DMA_SRC_IO;
	} else if (block->source_addr_adj != DMA_ADDR_ADJ_INCREMENT) {
		return -ENOTSUP;
	}
	if (block->dest_addr_adj == DMA_ADDR_ADJ_NO_CHANGE) {
		cfg |= SUNXI_DMA_DST_IO;
	} else if (block->dest_addr_adj != DMA_ADDR_ADJ_INCREMENT) {
		return -ENOTSUP;
	}

	chan->lli.cfg = cfg;
	chan->lli.src = (uint32_t)block->source_address;
	chan->lli.dst = (uint32_t)block->dest_address;
	chan->lli.len = block->block_size;
	chan->lli.para = 64U;
	chan->lli.next = SUNXI_DMA_LINK_END;
	chan->size = block->block_size;

	return 0;
}

static int sunxi_dma_config(const struct device *dev, uint32_t channel,
				    struct dma_config *config)
{
	const struct sunxi_dma_config *cfg = dev->config;
	struct sunxi_dma_data *data = dev->data;
	struct sunxi_dma_channel *chan;
	int ret = 0;

	if (channel >= cfg->channels || channel >= SUNXI_DMA_CHANNELS ||
		config == NULL || config->head_block == NULL ||
		config->block_count != 1U || config->dma_slot >= cfg->requests) {
		return -EINVAL;
	}
	if (config->head_block->block_size == 0U ||
		config->head_block->block_size > SUNXI_DMA_MAX_BLOCK ||
		config->head_block->source_gather_en ||
		config->head_block->dest_scatter_en ||
		config->head_block->source_reload_en ||
		config->head_block->dest_reload_en || config->cyclic) {
		return -ENOTSUP;
	}
	if ((config->channel_direction != MEMORY_TO_MEMORY) &&
		(config->channel_direction != MEMORY_TO_PERIPHERAL) &&
		(config->channel_direction != PERIPHERAL_TO_MEMORY)) {
		return -ENOTSUP;
	}
	if (config->source_data_size != config->dest_data_size ||
		(config->source_data_size != 1U && config->source_data_size != 2U &&
		 config->source_data_size != 4U)) {
		return -EINVAL;
	}

	chan = &data->channel[channel];
	K_SPINLOCK(&data->lock) {
		if (chan->busy) {
			ret = -EBUSY;
		} else {
			chan->config = *config;
			chan->block = *config->head_block;
			chan->callback = config->dma_callback;
			chan->user_data = config->user_data;
			ret = sunxi_dma_configure_lli(chan);
			chan->configured = (ret == 0);
		}
	}

	return ret;
}

static int sunxi_dma_start(const struct device *dev, uint32_t channel)
{
	const struct sunxi_dma_config *cfg = dev->config;
	struct sunxi_dma_data *data = dev->data;
	struct sunxi_dma_channel *chan;

	if (channel >= cfg->channels || channel >= SUNXI_DMA_CHANNELS) {
		return -EINVAL;
	}
	chan = &data->channel[channel];
	if (!chan->configured) {
		return -EINVAL;
	}

	sys_cache_data_flush_range(&chan->lli, sizeof(chan->lli));
	if (chan->config.channel_direction != PERIPHERAL_TO_MEMORY) {
		sys_cache_data_flush_range((void *)(uintptr_t)chan->lli.src,
					    chan->lli.len);
	}
	chan->busy = true;
	sunxi_dma_irq_enable_channel(cfg, channel, true);
	sys_write32((uint32_t)(uintptr_t)&chan->lli,
		    sunxi_dma_ch_addr(cfg, channel, SUNXI_DMA_LLI_ADDR));
	sys_write32(SUNXI_DMA_CH_START,
		    sunxi_dma_ch_addr(cfg, channel, SUNXI_DMA_ENABLE));

	return 0;
}

static int sunxi_dma_stop(const struct device *dev, uint32_t channel)
{
	const struct sunxi_dma_config *cfg = dev->config;
	struct sunxi_dma_data *data = dev->data;

	if (channel >= cfg->channels || channel >= SUNXI_DMA_CHANNELS) {
		return -EINVAL;
	}
	sys_write32(SUNXI_DMA_CH_PAUSE,
		    sunxi_dma_ch_addr(cfg, channel, SUNXI_DMA_PAUSE));
	sys_write32(SUNXI_DMA_CH_STOP,
		    sunxi_dma_ch_addr(cfg, channel, SUNXI_DMA_ENABLE));
	sys_write32(SUNXI_DMA_CH_RESUME,
		    sunxi_dma_ch_addr(cfg, channel, SUNXI_DMA_PAUSE));
	sunxi_dma_irq_enable_channel(cfg, channel, false);
	data->channel[channel].busy = false;

	return 0;
}

static int sunxi_dma_reload(const struct device *dev, uint32_t channel,
				    uint32_t src, uint32_t dst, size_t size)
{
	struct sunxi_dma_data *data = dev->data;
	const struct sunxi_dma_config *cfg = dev->config;
	struct sunxi_dma_channel *chan;

	if (channel >= cfg->channels || channel >= SUNXI_DMA_CHANNELS || size == 0U ||
		size > SUNXI_DMA_MAX_BLOCK) {
		return -EINVAL;
	}
	chan = &data->channel[channel];
	if (chan->busy) {
		return -EBUSY;
	}
	chan->block.source_address = src;
	chan->block.dest_address = dst;
	chan->block.block_size = size;
	return sunxi_dma_configure_lli(chan);
}

static int sunxi_dma_get_status(const struct device *dev, uint32_t channel,
					struct dma_status *status)
{
	const struct sunxi_dma_config *cfg = dev->config;
	struct sunxi_dma_data *data = dev->data;

	if (channel >= cfg->channels || channel >= SUNXI_DMA_CHANNELS || status == NULL) {
		return -EINVAL;
	}
	status->busy = data->channel[channel].busy;
	status->dir = data->channel[channel].config.channel_direction;
	status->pending_length = sys_read32(sunxi_dma_ch_addr(cfg, channel, SUNXI_DMA_CNT));
	status->free = 0U;
	status->write_position = 0U;
	status->read_position = 0U;
	status->total_copied = data->channel[channel].size - status->pending_length;

	return 0;
}

static int sunxi_dma_get_attribute(const struct device *dev, uint32_t type,
					   uint32_t *value)
{
	if (value == NULL) {
		return -EINVAL;
	}
	switch (type) {
	case DMA_ATTR_BUFFER_ADDRESS_ALIGNMENT:
	case DMA_ATTR_BUFFER_SIZE_ALIGNMENT:
	case DMA_ATTR_COPY_ALIGNMENT:
		*value = 4U;
		return 0;
	case DMA_ATTR_MAX_BLOCK_COUNT:
		*value = 1U;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static bool sunxi_dma_chan_filter(const struct device *dev, int channel,
					  void *filter_param)
{
	const struct sunxi_dma_config *cfg = dev->config;
	ARG_UNUSED(filter_param);
	return channel >= 0 && (uint32_t)channel < cfg->channels;
}

static void sunxi_dma_chan_release(const struct device *dev, uint32_t channel)
{
	(void)sunxi_dma_stop(dev, channel);
}

static void sunxi_dma_isr(const struct device *dev)
{
	const struct sunxi_dma_config *cfg = dev->config;
	struct sunxi_dma_data *data = dev->data;
	uint32_t status[SUNXI_DMA_IRQ_GROUPS];

	status[0] = sunxi_dma_read(cfg, SUNXI_DMA_IRQ_STAT);
	status[1] = sunxi_dma_read(cfg, SUNXI_DMA_IRQ_STAT + 4U);
	sunxi_dma_write(cfg, SUNXI_DMA_IRQ_STAT, status[0]);
	sunxi_dma_write(cfg, SUNXI_DMA_IRQ_STAT + 4U, status[1]);

	for (uint32_t channel = 0U; channel < cfg->channels; channel++) {
		uint32_t event = (status[sunxi_dma_irq_group(channel)] >>
				  sunxi_dma_irq_shift(channel)) & 0xFU;
		dma_callback_t callback = NULL;
		void *user_data = NULL;
		int callback_status = DMA_STATUS_COMPLETE;

		if (event == 0U) {
			continue;
		}
		if ((event & SUNXI_DMA_IRQ_TIMEOUT) != 0U) {
			callback_status = -ETIMEDOUT;
		} else if ((event & (SUNXI_DMA_IRQ_QUEUE | BIT(0) | BIT(1))) == 0U) {
			callback_status = -EIO;
		}
		K_SPINLOCK(&data->lock) {
			data->channel[channel].busy = false;
			if (data->channel[channel].config.complete_callback_en &&
			    data->channel[channel].callback != NULL) {
				callback = data->channel[channel].callback;
				user_data = data->channel[channel].user_data;
			}
		}
		sunxi_dma_irq_enable_channel(cfg, channel, false);
		if (callback != NULL) {
			callback(dev, user_data, channel, callback_status);
		}
	}
}

static int sunxi_dma_init(const struct device *dev)
{
	const struct sunxi_dma_config *cfg = dev->config;
	int ret;

	ret = clock_control_on(cfg->clock_dev,
		(clock_control_subsys_t)(uintptr_t)cfg->bus_clock_id);
	if (ret != 0) {
		return ret;
	}
	ret = clock_control_on(cfg->clock_dev,
		(clock_control_subsys_t)(uintptr_t)cfg->mbus_clock_id);
	if (ret != 0) {
		return ret;
	}
	ret = reset_line_toggle_dt(&cfg->reset);
	if (ret != 0) {
		return ret;
	}

	/* Disable automatic gating and mark every channel non-secure. */
	sunxi_dma_write(cfg, SUNXI_DMA_COMMON_GATE, 0x7U);
	sunxi_dma_write(cfg, SUNXI_DMA_SECURE, GENMASK(SUNXI_DMA_CHANNELS - 1U, 0));
	for (uint32_t group = 0U; group < SUNXI_DMA_IRQ_GROUPS; group++) {
		sunxi_dma_write(cfg, SUNXI_DMA_IRQ_EN + group * 4U, 0U);
		sunxi_dma_write(cfg, SUNXI_DMA_IRQ_STAT + group * 4U, 0xffffffffU);
	}

	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), sunxi_dma_isr,
		   DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));

	return 0;
}

static DEVICE_API(dma, sunxi_dma_api) = {
	.config = sunxi_dma_config,
	.reload = sunxi_dma_reload,
	.start = sunxi_dma_start,
	.stop = sunxi_dma_stop,
	.get_status = sunxi_dma_get_status,
	.get_attribute = sunxi_dma_get_attribute,
	.chan_filter = sunxi_dma_chan_filter,
	.chan_release = sunxi_dma_chan_release,
};

#define SUNXI_DMA_INIT(inst) \
	static const struct sunxi_dma_config sunxi_dma_cfg_##inst = { \
		.base = DT_INST_REG_ADDR(inst), \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(inst, bus)), \
		.bus_clock_id = DT_INST_CLOCKS_CELL_BY_NAME(inst, bus, clkid), \
		.mbus_clock_id = DT_INST_CLOCKS_CELL_BY_NAME(inst, mbus, clkid), \
		.reset = RESET_DT_SPEC_INST_GET(inst), \
		.channels = DT_INST_PROP(inst, dma_channels), \
		.requests = DT_INST_PROP(inst, dma_requests), \
	}; \
	static struct sunxi_dma_data sunxi_dma_data_##inst __nocache __aligned(64); \
	DEVICE_DT_INST_DEFINE(inst, sunxi_dma_init, NULL, &sunxi_dma_data_##inst, \
		&sunxi_dma_cfg_##inst, PRE_KERNEL_1, CONFIG_DMA_SUNXI_INIT_PRIORITY, \
		&sunxi_dma_api);

DT_INST_FOREACH_STATUS_OKAY(SUNXI_DMA_INIT)
