/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* I2S / PCM / TDM controller, one transmit and one receive data line */

#define DT_DRV_COMPAT allwinner_sunxi_i2s

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

#define I2S_CTL		0x00
#define I2S_FMT0	0x04
#define I2S_FMT1	0x08
#define I2S_RXFIFO	0x10
#define I2S_FIFOCTL	0x14
#define I2S_INTCTL	0x1c
#define I2S_TXFIFO	0x20
#define I2S_CLKDIV	0x24
#define I2S_TXCNT	0x28
#define I2S_RXCNT	0x2c
#define I2S_CHCFG	0x30
#define I2S_TX0CHSEL	0x34
#define I2S_TX0CHMAP0	0x44
#define I2S_TX0CHMAP1	0x48
#define I2S_RXCHSEL	0x64
#define I2S_RXCHMAP0	0x68

#define CTL_GLOBAL_EN	BIT(0)
#define CTL_RXEN	BIT(1)
#define CTL_TXEN	BIT(2)
#define CTL_LOOP	BIT(3)
#define CTL_MODE_SHIFT	4
#define CTL_MODE_MASK	(3U << CTL_MODE_SHIFT)
#define CTL_SDO0_EN	BIT(8)
#define CTL_LRCK_OUT	BIT(17)
#define CTL_BCLK_OUT	BIT(18)

#define FMT0_SLOT_WIDTH_MASK	0x7U
#define FMT0_SAMPLE_SHIFT	4
#define FMT0_SAMPLE_MASK	(0x7U << FMT0_SAMPLE_SHIFT)
#define FMT0_BCLK_INV		BIT(7)
#define FMT0_LRCK_PERIOD_SHIFT	8
#define FMT0_LRCK_PERIOD_MASK	(0x3ffU << FMT0_LRCK_PERIOD_SHIFT)
#define FMT0_LRCK_INV		BIT(19)
#define FMT0_LRCK_WIDTH		BIT(30)

#define FIFOCTL_FRX		BIT(24)
#define FIFOCTL_FTX		BIT(25)
#define FIFOCTL_TXTL_SHIFT	12
#define FIFOCTL_RXTL_SHIFT	4
#define FIFOCTL_TXIM		BIT(2)
#define FIFOCTL_RXOM_MASK	0x3U

#define INTCTL_TXDRQEN		BIT(7)
#define INTCTL_RXDRQEN		BIT(3)

#define CLKDIV_MCLKOUT_EN	BIT(8)
#define CLKDIV_BCLK_SHIFT	4
#define CLKDIV_MCLK_SHIFT	0

#define CHSEL_OFFSET_SHIFT	20
#define CHSEL_CHSEL_SHIFT	16

struct i2s_cfg {
	uintptr_t base;
	const struct device *clock_dev;
	uint32_t bus_clock_id;
	struct reset_dt_spec reset;
	const struct device *dma;
	const struct pinctrl_dev_config *pcfg;
	uint32_t clock_reg;
	uint32_t tx_channel;
	uint32_t rx_channel;
	uint8_t periods;
	uint8_t slot_width;
	uint16_t mclk_fs;
};

struct i2s_data {
	struct sunxi_i2s_stream tx;
	struct sunxi_i2s_stream rx;
	bool clk_held;
};

static inline uint32_t rd(const struct device *dev, uint32_t off)
{
	const struct i2s_cfg *cfg = dev->config;

	return sys_read32(cfg->base + off);
}

static inline void wr(const struct device *dev, uint32_t off, uint32_t val)
{
	const struct i2s_cfg *cfg = dev->config;

	sys_write32(val, cfg->base + off);
}

static inline void upd(const struct device *dev, uint32_t off, uint32_t mask, uint32_t val)
{
	wr(dev, off, (rd(dev, off) & ~mask) | (val & mask));
}

/* divider ratios of the MCLK and BCLK dividers and the register code of each */
static int div_code(uint32_t ratio)
{
	static const uint16_t table[] = {1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 176, 192};

	for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
		if (table[i] == ratio) {
			return i + 1;
		}
	}

	return -EINVAL;
}

static int slot_width_code(uint32_t width)
{
	switch (width) {
	case 8:
		return 1;
	case 12:
		return 2;
	case 16:
		return 3;
	case 20:
		return 4;
	case 24:
		return 5;
	case 28:
		return 6;
	case 32:
		return 7;
	default:
		return -EINVAL;
	}
}

static void release_clock(const struct device *dev)
{
	const struct i2s_cfg *cfg = dev->config;
	struct i2s_data *data = dev->data;

	if (data->clk_held) {
		upd(dev, I2S_CTL, CTL_GLOBAL_EN, 0U);
		sunxi_audio_module_clk_off(cfg->clock_reg);
		sunxi_audio_pll_put();
		data->clk_held = false;
	}
}

/* Rate, format and clock direction are shared by both directions */
static int program_common(const struct device *dev, const struct i2s_config *c)
{
	const struct i2s_cfg *cfg = dev->config;
	struct i2s_data *data = dev->data;
	uint32_t fs = c->frame_clk_freq;
	uint32_t fmt = c->format & I2S_FMT_DATA_FORMAT_MASK;
	bool pcm = fmt == I2S_FMT_DATA_FORMAT_PCM_SHORT || fmt == I2S_FMT_DATA_FORMAT_PCM_LONG;
	bool delayed = fmt == I2S_FMT_DATA_FORMAT_I2S || fmt == I2S_FMT_DATA_FORMAT_PCM_SHORT;
	uint32_t slots = c->channels < 2U && !pcm ? 2U : c->channels;
	uint32_t module, bclk, mode, lrck_period, mclk_out;
	bool bclk_inv = (c->format & I2S_FMT_BIT_CLK_INV) != 0U;
	bool lrck_inv = (c->format & I2S_FMT_FRAME_CLK_INV) != 0U;
	int fam, sw, mdiv, bdiv, ret;

	fam = sunxi_audio_family(fs);
	sw = slot_width_code(cfg->slot_width);
	if (fam < 0 || sw < 0) {
		return -EINVAL;
	}
	module = sunxi_audio_base_mclk(fam);
	/* bit clock rate: slots of the frame at the slot width */
	bclk = fs * slots * cfg->slot_width;
	bdiv = div_code(module / bclk);
	mclk_out = fs * cfg->mclk_fs;
	mdiv = div_code(module / mclk_out);
	if (module % bclk != 0U || bdiv < 0 || module % mclk_out != 0U || mdiv < 0) {
		LOG_ERR("no divider for %u Hz frames, %u slots of %u bits, mclk %u*fs", fs, slots,
			cfg->slot_width, cfg->mclk_fs);
		return -EINVAL;
	}

	switch (fmt) {
	case I2S_FMT_DATA_FORMAT_I2S:
	case I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED:
		mode = 1;
		break;
	case I2S_FMT_DATA_FORMAT_RIGHT_JUSTIFIED:
		mode = 2;
		break;
	case I2S_FMT_DATA_FORMAT_PCM_SHORT:
	case I2S_FMT_DATA_FORMAT_PCM_LONG:
		mode = 0;
		break;
	default:
		return -EINVAL;
	}
	lrck_period = (pcm ? slots * cfg->slot_width : (slots / 2U) * cfg->slot_width) - 1U;

	if (!data->clk_held) {
		ret = sunxi_audio_pll_get(fam);
		if (ret != 0) {
			return ret;
		}
		ret = sunxi_audio_module_clk_set(cfg->clock_reg, module, NULL);
		if (ret != 0) {
			sunxi_audio_pll_put();
			return ret;
		}
		data->clk_held = true;
		upd(dev, I2S_CTL, CTL_GLOBAL_EN, CTL_GLOBAL_EN);
	}

	upd(dev, I2S_CTL, CTL_MODE_MASK | CTL_BCLK_OUT | CTL_LRCK_OUT,
	    (mode << CTL_MODE_SHIFT) |
	    ((c->options & I2S_OPT_BIT_CLK_SLAVE) != 0U ? 0U : CTL_BCLK_OUT) |
	    ((c->options & I2S_OPT_FRAME_CLK_SLAVE) != 0U ? 0U : CTL_LRCK_OUT));
	upd(dev, I2S_CTL, CTL_LOOP, (c->options & I2S_OPT_LOOPBACK) != 0U ? CTL_LOOP : 0U);
	upd(dev, I2S_FMT0, FMT0_SLOT_WIDTH_MASK | FMT0_LRCK_PERIOD_MASK | FMT0_BCLK_INV |
	    FMT0_LRCK_INV | FMT0_LRCK_WIDTH,
	    (uint32_t)sw | (lrck_period << FMT0_LRCK_PERIOD_SHIFT) |
	    (bclk_inv ? FMT0_BCLK_INV : 0U) |
	    /* the polarity bit is relative to the frame start edge in the PCM modes */
	    ((lrck_inv ^ pcm) ? FMT0_LRCK_INV : 0U) |
	    (fmt == I2S_FMT_DATA_FORMAT_PCM_LONG ? FMT0_LRCK_WIDTH : 0U));
	upd(dev, I2S_CLKDIV, 0xffU | CLKDIV_MCLKOUT_EN,
	    ((uint32_t)bdiv << CLKDIV_BCLK_SHIFT) | ((uint32_t)mdiv << CLKDIV_MCLK_SHIFT) |
	    CLKDIV_MCLKOUT_EN);
	/* data starts one bit clock after the frame edge in I2S and PCM short frame */
	upd(dev, I2S_TX0CHSEL, 3U << CHSEL_OFFSET_SHIFT, (delayed ? 1U : 0U) << CHSEL_OFFSET_SHIFT);
	upd(dev, I2S_RXCHSEL, 3U << CHSEL_OFFSET_SHIFT, (delayed ? 1U : 0U) << CHSEL_OFFSET_SHIFT);

	/* straight channel maps */
	wr(dev, I2S_TX0CHMAP0, 0xfedcba98U);
	wr(dev, I2S_TX0CHMAP1, 0x76543210U);
	wr(dev, I2S_RXCHMAP0, 0x0f0e0d0cU);
	wr(dev, I2S_RXCHMAP0 + 4, 0x0b0a0908U);
	wr(dev, I2S_RXCHMAP0 + 8, 0x07060504U);
	wr(dev, I2S_RXCHMAP0 + 12, 0x03020100U);

	return 0;
}

static int i2s_sunxi_configure(const struct device *dev, enum i2s_dir dir, const struct i2s_config *c)
{
	struct i2s_data *data = dev->data;
	bool tx = dir == I2S_DIR_TX;
	struct sunxi_i2s_stream *s = tx ? &data->tx : &data->rx;
	struct sunxi_i2s_stream *other = tx ? &data->rx : &data->tx;
	uint8_t width;
	uint32_t res;
	int ret;

	if (dir != I2S_DIR_TX && dir != I2S_DIR_RX) {
		return -ENOSYS;
	}
	if (c->frame_clk_freq == 0U) {
		ret = sunxi_i2s_stream_configure(s, c, 2);
		if (ret == 0 && other->state == I2S_STATE_NOT_READY) {
			release_clock(dev);
		}
		return ret;
	}
	if (s->state != I2S_STATE_NOT_READY && s->state != I2S_STATE_READY) {
		return -EINVAL;
	}
	if (c->channels < 1U || c->channels > 16U) {
		return -EINVAL;
	}
	if (other->state != I2S_STATE_NOT_READY &&
	    other->cfg.frame_clk_freq != c->frame_clk_freq) {
		LOG_ERR("TX and RX share the frame clock");
		return -EINVAL;
	}
	switch (c->word_size) {
	case 16:
		width = 2;
		res = 3;
		break;
	case 24:
		width = 4;
		res = 5;
		break;
	case 32:
		width = 4;
		res = 7;
		break;
	default:
		return -EINVAL;
	}
	ret = program_common(dev, c);
	if (ret != 0) {
		return ret;
	}
	upd(dev, I2S_FMT0, FMT0_SAMPLE_MASK, res << FMT0_SAMPLE_SHIFT);
	if (tx) {
		uint32_t slots = c->channels < 2U ? 2U : c->channels;

		upd(dev, I2S_FIFOCTL, FIFOCTL_TXIM | (0x7fU << FIFOCTL_TXTL_SHIFT),
		    FIFOCTL_TXIM | (0x40U << FIFOCTL_TXTL_SHIFT));
		upd(dev, I2S_CHCFG, 0xfU, c->channels - 1U);
		wr(dev, I2S_TX0CHSEL, (rd(dev, I2S_TX0CHSEL) & (3U << CHSEL_OFFSET_SHIFT)) |
		   ((slots - 1U) << CHSEL_CHSEL_SHIFT) | ((1U << c->channels) - 1U));
	} else {
		upd(dev, I2S_FIFOCTL, FIFOCTL_RXOM_MASK | (0x7fU << FIFOCTL_RXTL_SHIFT),
		    1U | (0x1fU << FIFOCTL_RXTL_SHIFT));
		upd(dev, I2S_CHCFG, 0xfU << 4, (c->channels - 1U) << 4);
		upd(dev, I2S_RXCHSEL, 0xfU << CHSEL_CHSEL_SHIFT, (c->channels - 1U) << CHSEL_CHSEL_SHIFT);
	}

	return sunxi_i2s_stream_configure(s, c, width);
}

static const struct i2s_config *i2s_sunxi_config_get(const struct device *dev, enum i2s_dir dir)
{
	struct i2s_data *data = dev->data;
	struct sunxi_i2s_stream *s = dir == I2S_DIR_TX ? &data->tx : &data->rx;

	return s->state == I2S_STATE_NOT_READY ? NULL : &s->cfg;
}

static int i2s_start(const struct device *dev, struct sunxi_i2s_stream *s)
{
	if (s->dir == I2S_DIR_TX) {
		upd(dev, I2S_FIFOCTL, FIFOCTL_FTX, FIFOCTL_FTX);
		wr(dev, I2S_TXCNT, 0U);
		upd(dev, I2S_INTCTL, INTCTL_TXDRQEN, INTCTL_TXDRQEN);
		upd(dev, I2S_CTL, CTL_SDO0_EN | CTL_TXEN, CTL_SDO0_EN | CTL_TXEN);
	} else {
		upd(dev, I2S_FIFOCTL, FIFOCTL_FRX, FIFOCTL_FRX);
		wr(dev, I2S_RXCNT, 0U);
		upd(dev, I2S_CTL, CTL_RXEN, CTL_RXEN);
		upd(dev, I2S_INTCTL, INTCTL_RXDRQEN, INTCTL_RXDRQEN);
	}

	return 0;
}

static void i2s_stop(const struct device *dev, struct sunxi_i2s_stream *s)
{
	if (s->dir == I2S_DIR_TX) {
		upd(dev, I2S_INTCTL, INTCTL_TXDRQEN, 0U);
		upd(dev, I2S_CTL, CTL_SDO0_EN | CTL_TXEN, 0U);
	} else {
		upd(dev, I2S_INTCTL, INTCTL_RXDRQEN, 0U);
		upd(dev, I2S_CTL, CTL_RXEN, 0U);
	}
}

static const struct sunxi_i2s_stream_ops i2s_stream_ops = {
	.start = i2s_start,
	.stop = i2s_stop,
};

static int i2s_sunxi_read(const struct device *dev, void **mem_block, size_t *size)
{
	struct i2s_data *data = dev->data;

	return sunxi_i2s_stream_read(&data->rx, mem_block, size);
}

static int i2s_sunxi_write(const struct device *dev, void *mem_block, size_t size)
{
	struct i2s_data *data = dev->data;

	return sunxi_i2s_stream_write(&data->tx, mem_block, size);
}

static int i2s_sunxi_trigger(const struct device *dev, enum i2s_dir dir, enum i2s_trigger_cmd cmd)
{
	struct i2s_data *data = dev->data;

	switch (dir) {
	case I2S_DIR_TX:
		return sunxi_i2s_stream_trigger(&data->tx, cmd);
	case I2S_DIR_RX:
		return sunxi_i2s_stream_trigger(&data->rx, cmd);
	default:
		return -ENOSYS;
	}
}

static int i2s_sunxi_init(const struct device *dev)
{
	const struct i2s_cfg *cfg = dev->config;
	struct i2s_data *data = dev->data;
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

	sunxi_i2s_stream_init(&data->tx, dev, &i2s_stream_ops, cfg->dma, cfg->tx_channel, 3,
			      cfg->base + I2S_TXFIFO, I2S_DIR_TX, cfg->periods);
	sunxi_i2s_stream_init(&data->rx, dev, &i2s_stream_ops, cfg->dma, cfg->rx_channel, 3,
			      cfg->base + I2S_RXFIFO, I2S_DIR_RX, cfg->periods);

	return 0;
}

static DEVICE_API(i2s, i2s_sunxi_api) = {
	.configure = i2s_sunxi_configure,
	.config_get = i2s_sunxi_config_get,
	.read = i2s_sunxi_read,
	.write = i2s_sunxi_write,
	.trigger = i2s_sunxi_trigger,
};

#define I2S_PCFG(n) COND_CODE_1(DT_INST_PINCTRL_HAS_NAME(n, default),			\
				(PINCTRL_DT_INST_DEV_CONFIG_GET(n)), (NULL))

#define I2S_INIT(n)									\
	IF_ENABLED(DT_INST_PINCTRL_HAS_NAME(n, default), (PINCTRL_DT_INST_DEFINE(n);))	\
	static const struct i2s_cfg i2s_cfg_##n = {					\
		.base = DT_INST_REG_ADDR(n),						\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),			\
		.bus_clock_id = DT_INST_CLOCKS_CELL(n, clkid),				\
		.reset = RESET_DT_SPEC_INST_GET(n),					\
		.dma = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(n, tx)),			\
		.pcfg = I2S_PCFG(n),							\
		.clock_reg = DT_INST_PROP(n, clock_reg),				\
		.tx_channel = DT_INST_DMAS_CELL_BY_NAME(n, tx, channel),		\
		.rx_channel = DT_INST_DMAS_CELL_BY_NAME(n, rx, channel),		\
		.periods = DT_INST_PROP(n, block_count),				\
		.slot_width = DT_INST_PROP(n, slot_width),				\
		.mclk_fs = DT_INST_PROP(n, mclk_fs),					\
	};										\
	static struct i2s_data i2s_data_##n;						\
	DEVICE_DT_INST_DEFINE(n, i2s_sunxi_init, NULL, &i2s_data_##n, &i2s_cfg_##n,	\
			      POST_KERNEL, CONFIG_I2S_INIT_PRIORITY, &i2s_sunxi_api);

DT_INST_FOREACH_STATUS_OKAY(I2S_INIT)
