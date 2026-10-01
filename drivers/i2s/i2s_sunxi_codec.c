/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Digital side of the on-chip audio codec: the DAC (TX) and ADC (RX) FIFOs
 * with their DMA streams, as an I2S device. The analog front end is the
 * AUDIO_CODEC_SUNXI driver (child node "analog").
 */

#define DT_DRV_COMPAT allwinner_sunxi_codec

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

#include "i2s_sunxi_clk.h"
#include "i2s_sunxi_stream.h"

LOG_MODULE_DECLARE(i2s_sunxi, CONFIG_I2S_LOG_LEVEL);

/* digital registers */
#define DAC_FIFO_CTL		0x10
#define DAC_FIFO_STA		0x14
#define DAC_TXDATA		0x20
#define DAC_CNT			0x24
#define ADC_FIFO_CTL		0x30
#define ADC_FIFO_STA		0x38
#define ADC_RXDATA		0x40
#define ADC_CNT			0x44
#define ADC_DIG_CTL		0x50

#define DAC_FS_SHIFT		29
#define DAC_FIFO_MODE_SHIFT	24
#define DAC_MONO_EN		BIT(6)
#define DAC_TX_SAMPLE_24	BIT(5)
#define DAC_DRQ_EN		BIT(4)
#define DAC_FIFO_FLUSH		BIT(0)
#define DAC_STA_CLEAR		(BIT(3) | BIT(2) | BIT(1))

#define ADC_FS_SHIFT		29
#define ADC_EN			BIT(28)
#define ADC_RX_FIFO_16		BIT(24)
#define ADC_RX_SAMPLE_24	BIT(16)
#define ADC_DRQ_EN		BIT(3)
#define ADC_FIFO_FLUSH		BIT(0)
#define ADC_STA_CLEAR		(BIT(3) | BIT(1))
#define ADC1_CHANNEL_EN		BIT(0)

struct codec_config {
	uintptr_t base;
	const struct device *clock_dev;
	uint32_t bus_clock_id;
	struct reset_dt_spec reset;
	const struct device *dma;
	uint32_t dac_clock_reg;
	uint32_t adc_clock_reg;
	uint32_t tx_channel;
	uint32_t rx_channel;
	uint8_t periods;
};

struct codec_data {
	struct sunxi_i2s_stream tx;
	struct sunxi_i2s_stream rx;
	bool tx_clk;
	bool rx_clk;
};

#define DAC_DPC			0x00
#define DAC_DIG_EN		BIT(31)

static inline uint32_t rd(const struct device *dev, uint32_t off)
{
	const struct codec_config *cfg = dev->config;

	return sys_read32(cfg->base + off);
}

static inline void wr(const struct device *dev, uint32_t off, uint32_t val)
{
	const struct codec_config *cfg = dev->config;

	sys_write32(val, cfg->base + off);
}

static inline void upd(const struct device *dev, uint32_t off, uint32_t mask, uint32_t val)
{
	wr(dev, off, (rd(dev, off) & ~mask) | (val & mask));
}

/* sample rate code of the FIFO control registers, for rates scaled to the 48 kHz family */
static int rate_code(uint32_t rate)
{
	switch (rate) {
	case 48000:
		return 0;
	case 32000:
		return 1;
	case 24000:
		return 2;
	case 16000:
		return 3;
	case 12000:
		return 4;
	case 8000:
		return 5;
	case 192000:
		return 6;
	case 96000:
		return 7;
	default:
		return -EINVAL;
	}
}

static int codec_prepare_clock(const struct device *dev, bool tx, uint32_t rate, int *code)
{
	const struct codec_config *cfg = dev->config;
	struct codec_data *data = dev->data;
	int fam = sunxi_audio_family(rate);
	uint32_t norm = rate;
	bool *held = tx ? &data->tx_clk : &data->rx_clk;
	int ret;

	if (fam < 0) {
		return -EINVAL;
	}
	if (fam == SUNXI_AUDIO_FAMILY_44K1) {
		/* the 44.1 kHz family runs the same dividers from a 22.5792 MHz clock */
		norm = (uint32_t)(((uint64_t)rate * 48000U + 22050U) / 44100U);
	}
	*code = rate_code(norm);
	if (*code < 0) {
		return -EINVAL;
	}
	if (!tx && *code > 0 && norm > 48000U) {
		return -EINVAL;
	}

	if (*held) {
		sunxi_audio_module_clk_off(tx ? cfg->dac_clock_reg : cfg->adc_clock_reg);
		sunxi_audio_pll_put();
		*held = false;
	}
	ret = sunxi_audio_pll_get(fam);
	if (ret != 0) {
		return ret;
	}
	ret = sunxi_audio_module_clk_set(tx ? cfg->dac_clock_reg : cfg->adc_clock_reg,
					 sunxi_audio_base_mclk(fam), NULL);
	if (ret != 0) {
		sunxi_audio_pll_put();
		return ret;
	}
	*held = true;

	return 0;
}

static int codec_configure(const struct device *dev, enum i2s_dir dir, const struct i2s_config *cfg)
{
	struct codec_data *data = dev->data;
	bool tx = dir == I2S_DIR_TX;
	struct sunxi_i2s_stream *s = tx ? &data->tx : &data->rx;
	uint8_t width;
	int code = 0, ret;

	if (dir != I2S_DIR_TX && dir != I2S_DIR_RX) {
		return -ENOSYS;
	}
	if (cfg->frame_clk_freq == 0U) {
		ret = sunxi_i2s_stream_configure(s, cfg, 2);
		if (ret == 0) {
			const struct codec_config *c = dev->config;
			bool *held = tx ? &data->tx_clk : &data->rx_clk;

			if (*held) {
				sunxi_audio_module_clk_off(tx ? c->dac_clock_reg : c->adc_clock_reg);
				sunxi_audio_pll_put();
				*held = false;
			}
		}
		return ret;
	}
	if (s->state != I2S_STATE_NOT_READY && s->state != I2S_STATE_READY) {
		return -EINVAL;
	}
	if (cfg->channels < 1U || cfg->channels > (tx ? 2U : 1U)) {
		return -EINVAL;
	}
	switch (cfg->word_size) {
	case 16:
		width = 2;
		break;
	case 24:
	case 32:
		width = 4;
		break;
	default:
		return -EINVAL;
	}
	ret = codec_prepare_clock(dev, tx, cfg->frame_clk_freq, &code);
	if (ret != 0) {
		return ret;
	}

	if (tx) {
		uint32_t v = (uint32_t)code << DAC_FS_SHIFT;

		v |= width == 2 ? (3U << DAC_FIFO_MODE_SHIFT) : DAC_TX_SAMPLE_24;
		if (cfg->channels == 1U) {
			v |= DAC_MONO_EN;
		}
		upd(dev, DAC_FIFO_CTL, (7U << DAC_FS_SHIFT) | (3U << DAC_FIFO_MODE_SHIFT) |
		    DAC_TX_SAMPLE_24 | DAC_MONO_EN, v);
	} else {
		uint32_t v = (uint32_t)code << ADC_FS_SHIFT;

		v |= width == 2 ? ADC_RX_FIFO_16 : ADC_RX_SAMPLE_24;
		upd(dev, ADC_FIFO_CTL, (7U << ADC_FS_SHIFT) | ADC_RX_FIFO_16 | ADC_RX_SAMPLE_24, v);
	}

	return sunxi_i2s_stream_configure(s, cfg, width);
}

static const struct i2s_config *codec_config_get(const struct device *dev, enum i2s_dir dir)
{
	struct codec_data *data = dev->data;
	struct sunxi_i2s_stream *s = dir == I2S_DIR_TX ? &data->tx : &data->rx;

	return s->state == I2S_STATE_NOT_READY ? NULL : &s->cfg;
}

static int codec_start(const struct device *dev, struct sunxi_i2s_stream *s)
{
	if (s->dir == I2S_DIR_TX) {
		upd(dev, DAC_DPC, DAC_DIG_EN, DAC_DIG_EN);
		upd(dev, DAC_FIFO_CTL, DAC_FIFO_FLUSH, DAC_FIFO_FLUSH);
		wr(dev, DAC_FIFO_STA, DAC_STA_CLEAR);
		wr(dev, DAC_CNT, 0U);
		upd(dev, DAC_FIFO_CTL, DAC_DRQ_EN, DAC_DRQ_EN);
	} else {
		upd(dev, ADC_DIG_CTL, ADC1_CHANNEL_EN, ADC1_CHANNEL_EN);
		upd(dev, ADC_FIFO_CTL, ADC_FIFO_FLUSH | ADC_EN, ADC_FIFO_FLUSH | ADC_EN);
		wr(dev, ADC_FIFO_STA, ADC_STA_CLEAR);
		wr(dev, ADC_CNT, 0U);
		upd(dev, ADC_FIFO_CTL, ADC_DRQ_EN, ADC_DRQ_EN);
	}

	return 0;
}

static void codec_stop(const struct device *dev, struct sunxi_i2s_stream *s)
{
	if (s->dir == I2S_DIR_TX) {
		upd(dev, DAC_FIFO_CTL, DAC_DRQ_EN, 0U);
	} else {
		upd(dev, ADC_FIFO_CTL, ADC_DRQ_EN | ADC_EN, 0U);
		upd(dev, ADC_DIG_CTL, ADC1_CHANNEL_EN, 0U);
	}
}

static const struct sunxi_i2s_stream_ops codec_stream_ops = {
	.start = codec_start,
	.stop = codec_stop,
};

static int codec_read(const struct device *dev, void **mem_block, size_t *size)
{
	struct codec_data *data = dev->data;

	return sunxi_i2s_stream_read(&data->rx, mem_block, size);
}

static int codec_write(const struct device *dev, void *mem_block, size_t size)
{
	struct codec_data *data = dev->data;

	return sunxi_i2s_stream_write(&data->tx, mem_block, size);
}

static int codec_trigger(const struct device *dev, enum i2s_dir dir, enum i2s_trigger_cmd cmd)
{
	struct codec_data *data = dev->data;

	switch (dir) {
	case I2S_DIR_TX:
		return sunxi_i2s_stream_trigger(&data->tx, cmd);
	case I2S_DIR_RX:
		return sunxi_i2s_stream_trigger(&data->rx, cmd);
	default:
		return -ENOSYS;
	}
}

static int codec_init(const struct device *dev)
{
	const struct codec_config *cfg = dev->config;
	struct codec_data *data = dev->data;
	int ret;

	if (!device_is_ready(cfg->dma) || !device_is_ready(cfg->clock_dev)) {
		return -ENODEV;
	}
	ret = clock_control_on(cfg->clock_dev, (clock_control_subsys_t)(uintptr_t)cfg->bus_clock_id);
	if (ret != 0) {
		return ret;
	}
	ret = reset_line_toggle_dt(&cfg->reset);
	if (ret != 0) {
		return ret;
	}

	/* volume control on, drop the first samples of a capture while the input settles */
	upd(dev, ADC_DIG_CTL, BIT(16), BIT(16));
	upd(dev, 0x04, BIT(16), BIT(16));
	upd(dev, ADC_FIFO_CTL, BIT(25) | (3U << 26), BIT(25) | (2U << 26));

	sunxi_i2s_stream_init(&data->tx, dev, &codec_stream_ops, cfg->dma, cfg->tx_channel, 7,
			      cfg->base + DAC_TXDATA, I2S_DIR_TX, cfg->periods);
	sunxi_i2s_stream_init(&data->rx, dev, &codec_stream_ops, cfg->dma, cfg->rx_channel, 7,
			      cfg->base + ADC_RXDATA, I2S_DIR_RX, cfg->periods);

	return 0;
}

static DEVICE_API(i2s, codec_api) = {
	.configure = codec_configure,
	.config_get = codec_config_get,
	.read = codec_read,
	.write = codec_write,
	.trigger = codec_trigger,
};

#define CODEC_INIT(n)								\
	static const struct codec_config codec_cfg_##n = {			\
		.base = DT_INST_REG_ADDR(n),					\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),		\
		.bus_clock_id = DT_INST_CLOCKS_CELL(n, clkid),			\
		.reset = RESET_DT_SPEC_INST_GET(n),				\
		.dma = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(n, tx)),		\
		.dac_clock_reg = DT_INST_PROP(n, dac_clock_reg),		\
		.adc_clock_reg = DT_INST_PROP(n, adc_clock_reg),		\
		.tx_channel = DT_INST_DMAS_CELL_BY_NAME(n, tx, channel),	\
		.rx_channel = DT_INST_DMAS_CELL_BY_NAME(n, rx, channel),	\
		.periods = DT_INST_PROP(n, block_count),			\
	};									\
	static struct codec_data codec_data_##n;				\
	DEVICE_DT_INST_DEFINE(n, codec_init, NULL, &codec_data_##n, &codec_cfg_##n,	\
			      POST_KERNEL, CONFIG_I2S_INIT_PRIORITY, &codec_api);

DT_INST_FOREACH_STATUS_OKAY(CODEC_INIT)
