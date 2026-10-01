/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* One-wire audio: S/PDIF (IEC 60958) transmitter and receiver */

#define DT_DRV_COMPAT allwinner_sunxi_owa

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

#include "i2s_sunxi_clk.h"
#include "i2s_sunxi_stream.h"

LOG_MODULE_DECLARE(i2s_sunxi, CONFIG_I2S_LOG_LEVEL);

#define OWA_CTL		0x00
#define OWA_TXCFG	0x04
#define OWA_RXCFG	0x08
#define OWA_INT_STA	0x0c
#define OWA_RXFIFO	0x10
#define OWA_FIFO_CTL	0x14
#define OWA_INT		0x1c
#define OWA_TXFIFO	0x20
#define OWA_TXCNT	0x24
#define OWA_RXCNT	0x28
#define OWA_TXCH_STA0	0x2c
#define OWA_TXCH_STA1	0x30
#define OWA_RXCH_STA0	0x34
#define OWA_RXCH_STA1	0x38

#define CTL_RESET	BIT(0)
#define CTL_GEN_EN	BIT(1)
#define CTL_LOOP_EN	BIT(2)

#define TXCFG_TXEN		BIT(0)
#define TXCFG_CHAN_STA_EN	BIT(1)
#define TXCFG_SAMPLE_SHIFT	2
#define TXCFG_DIV_SHIFT		4
#define TXCFG_DIV_MASK		(0x1fU << TXCFG_DIV_SHIFT)
#define TXCFG_SINGLE_MOD	BIT(31)

#define RXCFG_RXEN		BIT(0)
#define RXCFG_CHSR_CP		BIT(1)

#define FIFO_RXOM_MASK		0x3U
#define FIFO_TXIM		BIT(2)
#define FIFO_RXTL_SHIFT		4
#define FIFO_TXTL_SHIFT		12
#define FIFO_FRX		BIT(29)
#define FIFO_FTX		BIT(30)

#define INT_RXDRQEN		BIT(2)
#define INT_TXDRQEN		BIT(7)

#define CHSTA0_CHNUM_SHIFT	20
#define CHSTA0_FREQ_SHIFT	24
#define CHSTA1_WORDLEN_MASK	0xfU
#define CHSTA1_ORIFREQ_SHIFT	4

struct owa_cfg {
	uintptr_t base;
	const struct device *clock_dev;
	uint32_t bus_clock_id;
	struct reset_dt_spec reset;
	const struct device *dma;
	const struct pinctrl_dev_config *pcfg;
	uint32_t tx_clock_reg;
	uint32_t rx_clock_reg;
	uint32_t tx_channel;
	uint32_t rx_channel;
	uint8_t periods;
};

struct owa_data {
	struct sunxi_i2s_stream tx;
	struct sunxi_i2s_stream rx;
	bool tx_clk;
	bool rx_clk;
};

/* channel status codes of the sampling frequency and the original sampling frequency */
static const struct {
	uint32_t rate;
	uint8_t freq;
	uint8_t orig;
} rates[] = {
	{22050, 0x4, 0xb},
	{24000, 0x6, 0x9},
	{32000, 0x3, 0xc},
	{44100, 0x0, 0xf},
	{48000, 0x2, 0xd},
	{88200, 0x8, 0x7},
	{96000, 0xa, 0x5},
	{176400, 0xc, 0x3},
	{192000, 0xe, 0x1},
};

static inline uint32_t rd(const struct device *dev, uint32_t off)
{
	const struct owa_cfg *cfg = dev->config;

	return sys_read32(cfg->base + off);
}

static inline void wr(const struct device *dev, uint32_t off, uint32_t val)
{
	const struct owa_cfg *cfg = dev->config;

	sys_write32(val, cfg->base + off);
}

static inline void upd(const struct device *dev, uint32_t off, uint32_t mask, uint32_t val)
{
	wr(dev, off, (rd(dev, off) & ~mask) | (val & mask));
}

static void release_clock(const struct device *dev, bool tx)
{
	const struct owa_cfg *cfg = dev->config;
	struct owa_data *data = dev->data;
	bool *held = tx ? &data->tx_clk : &data->rx_clk;

	if (*held) {
		sunxi_audio_module_clk_off(tx ? cfg->tx_clock_reg : cfg->rx_clock_reg);
		sunxi_audio_pll_put();
		*held = false;
	}
}

static int owa_configure(const struct device *dev, enum i2s_dir dir, const struct i2s_config *c)
{
	const struct owa_cfg *cfg = dev->config;
	struct owa_data *data = dev->data;
	bool tx = dir == I2S_DIR_TX;
	struct sunxi_i2s_stream *s = tx ? &data->tx : &data->rx;
	uint32_t base_mclk, div, sample_code;
	uint8_t width, freq = 0, orig = 0;
	bool found = false;
	int fam, ret;

	if (dir != I2S_DIR_TX && dir != I2S_DIR_RX) {
		return -ENOSYS;
	}
	if (c->frame_clk_freq == 0U) {
		ret = sunxi_i2s_stream_configure(s, c, 2);
		if (ret == 0) {
			release_clock(dev, tx);
		}
		return ret;
	}
	if (s->state != I2S_STATE_NOT_READY && s->state != I2S_STATE_READY) {
		return -EINVAL;
	}
	if (c->channels < 1U || c->channels > 2U) {
		return -EINVAL;
	}
	for (size_t i = 0; i < ARRAY_SIZE(rates); i++) {
		if (rates[i].rate == c->frame_clk_freq) {
			freq = rates[i].freq;
			orig = rates[i].orig;
			found = true;
		}
	}
	fam = sunxi_audio_family(c->frame_clk_freq);
	if (!found || fam < 0) {
		return -EINVAL;
	}
	switch (c->word_size) {
	case 16:
		width = 2;
		sample_code = 0;
		break;
	case 24:
		width = 4;
		sample_code = 2;
		break;
	default:
		return -EINVAL;
	}
	base_mclk = sunxi_audio_base_mclk(fam);
	div = base_mclk / (128U * c->frame_clk_freq);
	if (div < 1U || div > 32U || base_mclk % (128U * c->frame_clk_freq) != 0U) {
		return -EINVAL;
	}

	release_clock(dev, tx);
	ret = sunxi_audio_pll_get(fam);
	if (ret != 0) {
		return ret;
	}
	/* the receiver clock has the audio PLL outputs at the sources 1 (/2) and 2 (/5) */
	ret = sunxi_audio_module_clk_set_mux(tx ? cfg->tx_clock_reg : cfg->rx_clock_reg, base_mclk,
					     tx ? 0U : 1U, tx ? 1U : 2U, NULL);
	if (ret != 0) {
		sunxi_audio_pll_put();
		return ret;
	}
	if (tx) {
		data->tx_clk = true;
	} else {
		data->rx_clk = true;
	}

	upd(dev, OWA_CTL, CTL_LOOP_EN, (c->options & I2S_OPT_LOOPBACK) != 0U ? CTL_LOOP_EN : 0U);
	if (tx) {
		upd(dev, OWA_TXCFG, (3U << TXCFG_SAMPLE_SHIFT) | TXCFG_DIV_MASK | TXCFG_SINGLE_MOD,
		    (sample_code << TXCFG_SAMPLE_SHIFT) | ((div - 1U) << TXCFG_DIV_SHIFT) |
		    (c->channels == 1U ? TXCFG_SINGLE_MOD : 0U));
		upd(dev, OWA_FIFO_CTL, FIFO_TXIM, width == 2 ? FIFO_TXIM : 0U);
		upd(dev, OWA_TXCH_STA0, 0xfU << CHSTA0_FREQ_SHIFT, (uint32_t)freq << CHSTA0_FREQ_SHIFT);
		upd(dev, OWA_TXCH_STA1, (0xfU << CHSTA1_ORIFREQ_SHIFT) | CHSTA1_WORDLEN_MASK,
		    ((uint32_t)orig << CHSTA1_ORIFREQ_SHIFT) | (width == 2 ? 0x2U : 0xbU));
	} else {
		upd(dev, OWA_FIFO_CTL, FIFO_RXOM_MASK, width == 2 ? 3U : 0U);
		upd(dev, OWA_RXCH_STA0, 0xfU << CHSTA0_FREQ_SHIFT, (uint32_t)freq << CHSTA0_FREQ_SHIFT);
	}

	return sunxi_i2s_stream_configure(s, c, width);
}

static const struct i2s_config *owa_config_get(const struct device *dev, enum i2s_dir dir)
{
	struct owa_data *data = dev->data;
	struct sunxi_i2s_stream *s = dir == I2S_DIR_TX ? &data->tx : &data->rx;

	return s->state == I2S_STATE_NOT_READY ? NULL : &s->cfg;
}

static int owa_start(const struct device *dev, struct sunxi_i2s_stream *s)
{
	/* clear the status, flush the FIFO and enable the engine before the data flows */
	upd(dev, OWA_CTL, CTL_GEN_EN, CTL_GEN_EN);
	wr(dev, OWA_INT_STA, rd(dev, OWA_INT_STA));
	if (s->dir == I2S_DIR_TX) {
		upd(dev, OWA_FIFO_CTL, FIFO_FTX, FIFO_FTX);
		wr(dev, OWA_TXCNT, 0U);
		upd(dev, OWA_TXCFG, TXCFG_TXEN, TXCFG_TXEN);
		upd(dev, OWA_INT, INT_TXDRQEN, INT_TXDRQEN);
	} else {
		upd(dev, OWA_FIFO_CTL, FIFO_FRX, FIFO_FRX);
		wr(dev, OWA_RXCNT, 0U);
		upd(dev, OWA_RXCFG, RXCFG_CHSR_CP, RXCFG_CHSR_CP);
		upd(dev, OWA_INT, INT_RXDRQEN, INT_RXDRQEN);
		upd(dev, OWA_RXCFG, RXCFG_RXEN, RXCFG_RXEN);
	}

	return 0;
}

static void owa_stop(const struct device *dev, struct sunxi_i2s_stream *s)
{
	if (s->dir == I2S_DIR_TX) {
		upd(dev, OWA_TXCFG, TXCFG_TXEN, 0U);
		upd(dev, OWA_INT, INT_TXDRQEN, 0U);
	} else {
		upd(dev, OWA_RXCFG, RXCFG_RXEN, 0U);
		upd(dev, OWA_INT, INT_RXDRQEN, 0U);
	}
}

static const struct sunxi_i2s_stream_ops owa_stream_ops = {
	.start = owa_start,
	.stop = owa_stop,
};

static int owa_read(const struct device *dev, void **mem_block, size_t *size)
{
	struct owa_data *data = dev->data;

	return sunxi_i2s_stream_read(&data->rx, mem_block, size);
}

static int owa_write(const struct device *dev, void *mem_block, size_t size)
{
	struct owa_data *data = dev->data;

	return sunxi_i2s_stream_write(&data->tx, mem_block, size);
}

static int owa_trigger(const struct device *dev, enum i2s_dir dir, enum i2s_trigger_cmd cmd)
{
	struct owa_data *data = dev->data;

	switch (dir) {
	case I2S_DIR_TX:
		return sunxi_i2s_stream_trigger(&data->tx, cmd);
	case I2S_DIR_RX:
		return sunxi_i2s_stream_trigger(&data->rx, cmd);
	default:
		return -ENOSYS;
	}
}

static int owa_init(const struct device *dev)
{
	const struct owa_cfg *cfg = dev->config;
	struct owa_data *data = dev->data;
	int ret;

	if (!device_is_ready(cfg->dma) || !device_is_ready(cfg->clock_dev)) {
		return -ENODEV;
	}
	if (cfg->pcfg != NULL) {
		ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
		if (ret != 0) {
			return ret;
		}
	}
	ret = clock_control_on(cfg->clock_dev, (clock_control_subsys_t)(uintptr_t)cfg->bus_clock_id);
	if (ret != 0) {
		return ret;
	}
	ret = reset_line_toggle_dt(&cfg->reset);
	if (ret != 0) {
		return ret;
	}

	/* FIFO levels, channel status generated by the hardware, two channels in the status */
	upd(dev, OWA_FIFO_CTL, (0xffU << FIFO_TXTL_SHIFT) | (0x7fU << FIFO_RXTL_SHIFT),
	    (0x40U << FIFO_TXTL_SHIFT) | (0x20U << FIFO_RXTL_SHIFT));
	upd(dev, OWA_TXCFG, TXCFG_CHAN_STA_EN, TXCFG_CHAN_STA_EN);
	wr(dev, OWA_TXCH_STA0, 2U << CHSTA0_CHNUM_SHIFT);
	wr(dev, OWA_RXCH_STA0, 2U << CHSTA0_CHNUM_SHIFT);
	upd(dev, OWA_CTL, CTL_RESET | CTL_GEN_EN, CTL_RESET);

	sunxi_i2s_stream_init(&data->tx, dev, &owa_stream_ops, cfg->dma, cfg->tx_channel, 2,
			      cfg->base + OWA_TXFIFO, I2S_DIR_TX, cfg->periods);
	sunxi_i2s_stream_init(&data->rx, dev, &owa_stream_ops, cfg->dma, cfg->rx_channel, 2,
			      cfg->base + OWA_RXFIFO, I2S_DIR_RX, cfg->periods);
	data->tx.burst = 8;
	data->rx.burst = 8;

	return 0;
}

static DEVICE_API(i2s, owa_api) = {
	.configure = owa_configure,
	.config_get = owa_config_get,
	.read = owa_read,
	.write = owa_write,
	.trigger = owa_trigger,
};

#define OWA_PCFG(n) COND_CODE_1(DT_INST_PINCTRL_HAS_NAME(n, default),			\
				(PINCTRL_DT_INST_DEV_CONFIG_GET(n)), (NULL))

#define OWA_INIT(n)									\
	IF_ENABLED(DT_INST_PINCTRL_HAS_NAME(n, default), (PINCTRL_DT_INST_DEFINE(n);))	\
	static const struct owa_cfg owa_cfg_##n = {					\
		.base = DT_INST_REG_ADDR(n),						\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),			\
		.bus_clock_id = DT_INST_CLOCKS_CELL(n, clkid),				\
		.reset = RESET_DT_SPEC_INST_GET(n),					\
		.dma = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(n, tx)),			\
		.pcfg = OWA_PCFG(n),							\
		.tx_clock_reg = DT_INST_PROP(n, tx_clock_reg),				\
		.rx_clock_reg = DT_INST_PROP(n, rx_clock_reg),				\
		.tx_channel = DT_INST_DMAS_CELL_BY_NAME(n, tx, channel),		\
		.rx_channel = DT_INST_DMAS_CELL_BY_NAME(n, rx, channel),		\
		.periods = DT_INST_PROP(n, block_count),				\
	};										\
	static struct owa_data owa_data_##n;						\
	DEVICE_DT_INST_DEFINE(n, owa_init, NULL, &owa_data_##n, &owa_cfg_##n,		\
			      POST_KERNEL, CONFIG_I2S_INIT_PRIORITY, &owa_api);

DT_INST_FOREACH_STATUS_OKAY(OWA_INIT)
