/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT allwinner_sunxi_spi

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi/spi_sunxi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_sunxi, CONFIG_SPI_LOG_LEVEL);

#include "spi_context.h"

#define SPI_GC		0x04
#define SPI_TC		0x08
#define SPI_INT_CTL	0x10
#define SPI_INT_STA	0x14
#define SPI_FIFO_CTL	0x18
#define SPI_FIFO_STA	0x1c
#define SPI_CLK_CTL	0x24
#define SPI_SDC		0x28
#define SPI_BURST_CNT	0x30
#define SPI_TX_CNT	0x34
#define SPI_BCC		0x38
#define SPI_TXDATA	0x200
#define SPI_RXDATA	0x300

#define GC_EN		BIT(0)
#define GC_MODE		BIT(1)
#define GC_MODE_SEL	BIT(2)
#define GC_TP_EN	BIT(7)
#define SAMP_DL_SW_EN	BIT(7)
#define SAMP_DL_SW	GENMASK(5, 0)
#define GC_SRST	BIT(31)

#define TC_CPHA		BIT(0)
#define TC_CPOL		BIT(1)
#define TC_SPOL		BIT(2)
#define TC_SSCTL	BIT(3)
#define TC_SS_SEL	GENMASK(5, 4)
#define TC_SS_OWNER	BIT(6)
#define TC_SS_LEVEL	BIT(7)
#define TC_DHB		BIT(8)
#define TC_DDB		BIT(9)
#define TC_FBS		BIT(12)
#define TC_SDC		BIT(11)
#define TC_SDM		BIT(13)
#define TC_SDC1		BIT(15)
#define TC_XCH		BIT(31)

#define INT_STA_ERR	(BIT(8) | BIT(9) | BIT(10))
#define INT_STA_TC	BIT(12)

#define FIFO_RX_DRQ_EN	BIT(8)
#define FIFO_RX_TRIG	32U
#define FIFO_TX_RST	BIT(31)
#define FIFO_RX_RST	BIT(15)
#define FIFO_TX_CNT	GENMASK(23, 16)
#define FIFO_RX_CNT	GENMASK(7, 0)

#define BCC_STC		GENMASK(23, 0)
#define BCC_DBC		GENMASK(27, 24)
#define BCC_DUAL	BIT(28)
#define BCC_QUAD	BIT(29)

/* sample mode (delay in half cycles) -> TC bits SDM, SDC, SDC1 */
static const uint16_t sample_mode_bits[SUNXI_SPI_SAMPLE_MODES] = {
	TC_SDM,
	0,
	TC_SDC,
	TC_SDM | TC_SDC,
	TC_SDM | TC_SDC1,
	TC_SDC1,
	TC_SDC | TC_SDC1,
};

#define SPI_HIGH_FREQ	60000000U	/* from here on the delay line is used */
#define CCU_PLL_PERI	0x0020U

#define CCU_BASE	0x02001000U
#define SPI_MODULE_HZ	24000000U
#define SPI_FIFO_DEPTH	64U
#define SPI_TIMEOUT_US	100000U
#define SPI_MAX_BURST	0xFFFFFFU

struct sunxi_spi_config {
	const struct pinctrl_dev_config *pcfg;
	const struct device *clock_dev;
	uint32_t base;
	uint32_t module_clock_id;
	uint32_t bus_clock_id;
	uint32_t clock_reg;
	struct reset_dt_spec reset;
	uint32_t clock_frequency;
#ifdef CONFIG_SPI_SUNXI_DMA
	const struct device *dma;	/* NULL: no DMA in the node */
	uint32_t dma_channel;
	uint32_t dma_slot;
#endif
};

struct sunxi_spi_data {
	struct spi_context ctx;
	bool quad;		/* data phase with four wires */
	bool sample_set;	/* a sample point was chosen with sunxi_spi_set_sample() */
	uint8_t sample_mode;
	uint8_t sample_delay;
#ifdef CONFIG_SPI_SUNXI_DMA
	struct k_sem dma_done;
	int dma_status;
#endif
};

static inline uint32_t sunxi_spi_read(const struct sunxi_spi_config *cfg,
				uint32_t offset)
{
	return sys_read32(cfg->base + offset);
}

static inline void sunxi_spi_write(const struct sunxi_spi_config *cfg,
				 uint32_t offset, uint32_t value)
{
	sys_write32(value, cfg->base + offset);
}

static uint32_t sunxi_spi_pll_peri_1x(void)
{
	uint32_t pll = sys_read32(CCU_BASE + CCU_PLL_PERI);
	uint32_t n = FIELD_GET(GENMASK(15, 8), pll) + 1U;
	uint32_t p0 = FIELD_GET(GENMASK(18, 16), pll) + 1U;
	uint32_t m = (pll & BIT(1)) ? 2U : 1U;

	return SPI_MODULE_HZ / m / p0 * n / 2U;
}

static int sunxi_spi_set_clock(const struct sunxi_spi_config *cfg,
				       uint32_t frequency)
{
	uint32_t n = 0U;
	uint32_t reg;

	if (frequency < 3000U) {
		return -EINVAL;
	}

	if (frequency > SPI_MODULE_HZ) {
		/*
		 * PLL_PERI_1X divided by M (1..16) and P (1,2,4,8) in the CCU, the
		 * controller's own divider is left at 1: SCK = module clock.
		 */
		uint32_t src = sunxi_spi_pll_peri_1x(), best = 0, best_m = 0, best_p = 0;

		for (uint32_t p = 0; p < 4U; p++) {
			for (uint32_t m = 0; m < 16U; m++) {
				uint32_t rate = src / (m + 1U) / BIT(p);

				if (rate <= frequency && rate > best) {
					best = rate;
					best_m = m;
					best_p = p;
				}
			}
		}
		if (best == 0U) {
			return -EINVAL;
		}
		sys_write32(BIT(31) | (1U << 24) | (best_p << 8) | best_m,
			    CCU_BASE + cfg->clock_reg);
		reg = sunxi_spi_read(cfg, SPI_CLK_CTL);
		reg &= ~(GENMASK(11, 8) | BIT(12) | GENMASK(7, 0));
		sunxi_spi_write(cfg, SPI_CLK_CTL, reg);

		return 0;
	}

	/* The controller clock divider is SPI_MODULE_HZ / 2^N. */
	while (n < 15U && (SPI_MODULE_HZ >> n) > frequency) {
		n++;
	}
	reg = BIT(31) | (n << 8); /* HOSC source, module clock enabled */
	sys_write32(reg, CCU_BASE + cfg->clock_reg);

	reg = sunxi_spi_read(cfg, SPI_CLK_CTL);
	reg &= ~(GENMASK(11, 8) | BIT(12) | GENMASK(7, 0));
	reg |= n << 8;
	sunxi_spi_write(cfg, SPI_CLK_CTL, reg);

	return 0;
}

static int sunxi_spi_reset_fifo(const struct sunxi_spi_config *cfg)
{
	uint32_t reg = sunxi_spi_read(cfg, SPI_FIFO_CTL);

	reg |= FIFO_TX_RST | FIFO_RX_RST;
	reg &= ~(GENMASK(23, 16) | GENMASK(7, 0));
	reg |= (32U << 16) | 32U;
	sunxi_spi_write(cfg, SPI_FIFO_CTL, reg);

	for (uint32_t elapsed = 0; elapsed < SPI_TIMEOUT_US; elapsed++) {
		if ((sunxi_spi_read(cfg, SPI_FIFO_CTL) & (FIFO_TX_RST | FIFO_RX_RST)) == 0U) {
			return 0;
		}
		k_busy_wait(1);
	}

	return -ETIMEDOUT;
}

static int sunxi_spi_configure(const struct device *dev,
				       const struct spi_config *config)
{
	const struct sunxi_spi_config *cfg = dev->config;
	struct sunxi_spi_data *data = dev->data;
	uint32_t gc;
	uint32_t tc;
	int ret;

	if (SPI_OP_MODE_GET(config->operation) != SPI_OP_MODE_MASTER ||
	    (config->operation & SPI_MODE_LOOP) != 0U ||
	    (config->operation & SPI_HOLD_ON_CS) != 0U ||
	    SPI_WORD_SIZE_GET(config->operation) != 8U ||
	    config->slave >= 4U) {
		return -ENOTSUP;
	}
#ifdef CONFIG_SPI_EXTENDED_MODES
	if ((config->operation & SPI_LINES_MASK) != SPI_LINES_SINGLE &&
	    (config->operation & SPI_LINES_MASK) != SPI_LINES_QUAD) {
		return -ENOTSUP;
	}
	data->quad = (config->operation & SPI_LINES_MASK) == SPI_LINES_QUAD;
#endif

	ret = sunxi_spi_set_clock(cfg, config->frequency);
	if (ret != 0) {
		return ret;
	}

	gc = sunxi_spi_read(cfg, SPI_GC);
	gc |= GC_EN | GC_MODE | GC_TP_EN;
	gc &= ~GC_MODE_SEL; /* legacy sample timing */
	if (data->sample_set || config->frequency > SPI_HIGH_FREQ) {
		gc |= GC_MODE_SEL; /* sample delay line */
	}
	sunxi_spi_write(cfg, SPI_GC, gc);

	tc = sunxi_spi_read(cfg, SPI_TC);
	tc &= ~(TC_CPHA | TC_CPOL | TC_SPOL | TC_SSCTL | TC_SS_SEL |
		TC_SS_OWNER | TC_SS_LEVEL | TC_FBS | TC_SDC1);
	tc &= ~TC_SDC;
	tc |= TC_DDB | (config->slave << 4) | TC_SS_LEVEL;
	if (data->sample_set) {
		tc |= sample_mode_bits[data->sample_mode];
	} else if (config->frequency > SPI_HIGH_FREQ) {
		tc |= sample_mode_bits[2];	/* one cycle */
	} else {
		tc |= TC_SDM;
	}
	if ((config->operation & SPI_MODE_CPHA) != 0U) {
		tc |= TC_CPHA;
	}
	if ((config->operation & SPI_MODE_CPOL) != 0U) {
		tc |= TC_CPOL;
	}
	if ((config->operation & SPI_TRANSFER_LSB) != 0U) {
		tc |= TC_FBS;
	}
	if ((config->operation & SPI_CS_ACTIVE_HIGH) == 0U) {
		tc |= TC_SPOL;
	}
	if (spi_cs_is_gpio(config)) {
		tc |= TC_SS_OWNER;
	}
	sunxi_spi_write(cfg, SPI_TC, tc);
	sunxi_spi_write(cfg, SPI_SDC, data->sample_set ? (SAMP_DL_SW_EN | data->sample_delay) : 0U);

	sunxi_spi_write(cfg, SPI_INT_CTL, 0U);
	sunxi_spi_write(cfg, SPI_INT_STA, 0xffffffffU);
	return sunxi_spi_reset_fifo(cfg);
}

static void sunxi_spi_set_counters(const struct sunxi_spi_config *cfg, bool quad,
					uint32_t tx_len, uint32_t rx_len)
{
	uint32_t burst = tx_len + rx_len;
	uint32_t stc = tx_len;
	uint32_t reg;

	/* Equal TX/RX lengths are a normal full-duplex burst. For NOR-style
	 * command/read transactions, DHB discards the command-phase RX bytes. */
	reg = sunxi_spi_read(cfg, SPI_TC);
	reg &= ~TC_DHB;
	if (tx_len != rx_len) {
		reg |= TC_DHB;
	}
	sunxi_spi_write(cfg, SPI_TC, reg);

	if (tx_len == rx_len) {
		burst = tx_len;
		stc = tx_len;
	}
	sunxi_spi_write(cfg, SPI_BURST_CNT, burst & SPI_MAX_BURST);
	sunxi_spi_write(cfg, SPI_TX_CNT, tx_len & SPI_MAX_BURST);
	reg = sunxi_spi_read(cfg, SPI_BCC);
	reg &= ~(BCC_STC | BCC_DBC | BCC_DUAL | BCC_QUAD);
	reg |= stc & BCC_STC;
	/* the command bytes go out on one wire (STC), the data of a read on four */
	if (quad && rx_len != 0U && tx_len != 0U && tx_len != rx_len) {
		reg |= BCC_QUAD;
	}
	sunxi_spi_write(cfg, SPI_BCC, reg);
}

#ifdef CONFIG_SPI_SUNXI_DMA
static void sunxi_spi_dma_cb(const struct device *dma, void *user_data, uint32_t channel,
			     int status)
{
	struct sunxi_spi_data *data = user_data;

	ARG_UNUSED(dma);
	ARG_UNUSED(channel);
	data->dma_status = status;
	k_sem_give(&data->dma_done);
}

/*
 * Start the DMA for the bulk of a long read. Returns the number of bytes it
 * will move (0: not used, the CPU does it all). The rest is below the FIFO
 * trigger level at the end and is read by the CPU.
 */
static uint32_t sunxi_spi_dma_rx_start(const struct device *dev, struct spi_context *ctx,
				       uint32_t rx_len)
{
	const struct sunxi_spi_config *cfg = dev->config;
	struct sunxi_spi_data *data = dev->data;
	uint32_t n = ROUND_DOWN(rx_len, FIFO_RX_TRIG);
	struct dma_block_config blk = {
		.source_address = cfg->base + SPI_RXDATA,
		.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT,
	};
	struct dma_config dc = {
		.dma_slot = cfg->dma_slot,
		.channel_direction = PERIPHERAL_TO_MEMORY,
		.complete_callback_en = 1U,
		.source_data_size = 4U,
		.dest_data_size = 4U,
		.source_burst_length = 8U,
		.dest_burst_length = 8U,
		.block_count = 1U,
		.head_block = &blk,
		.dma_callback = sunxi_spi_dma_cb,
		.user_data = data,
	};

	if (cfg->dma == NULL || !device_is_ready(cfg->dma) || rx_len < CONFIG_SPI_SUNXI_DMA_MIN_LEN ||
	    !spi_context_rx_buf_on(ctx) || ctx->rx_len != rx_len ||
	    ((uintptr_t)ctx->rx_buf & 63U) != 0U || n == 0U) {
		return 0;
	}
	blk.dest_address = (uint32_t)(uintptr_t)ctx->rx_buf;
	blk.block_size = n;
	sys_cache_data_flush_and_invd_range(ctx->rx_buf, ROUND_UP(n, 64U));
	k_sem_reset(&data->dma_done);
	if (dma_config(cfg->dma, cfg->dma_channel, &dc) != 0 ||
	    dma_start(cfg->dma, cfg->dma_channel) != 0) {
		return 0;
	}
	sunxi_spi_write(cfg, SPI_FIFO_CTL, sunxi_spi_read(cfg, SPI_FIFO_CTL) | FIFO_RX_DRQ_EN);

	return n;
}

static int sunxi_spi_dma_rx_finish(const struct device *dev, struct spi_context *ctx, uint32_t n)
{
	const struct sunxi_spi_config *cfg = dev->config;
	struct sunxi_spi_data *data = dev->data;
	int ret = k_sem_take(&data->dma_done, K_MSEC(2000));

	if (ret != 0) {
		dma_stop(cfg->dma, cfg->dma_channel);
		ret = -ETIMEDOUT;
	} else if (data->dma_status < 0) {
		ret = -EIO;
	}
	sunxi_spi_write(cfg, SPI_FIFO_CTL, sunxi_spi_read(cfg, SPI_FIFO_CTL) & ~FIFO_RX_DRQ_EN);
	sys_cache_data_invd_range(ctx->rx_buf, ROUND_UP(n, 64U));
	if (ret == 0) {
		spi_context_update_rx(ctx, 1, n);
	}

	return ret;
}
#endif

static int sunxi_spi_run_transfer(const struct device *dev, bool quad,
					struct spi_context *ctx,
					uint32_t tx_len, uint32_t rx_len)
{
	const struct sunxi_spi_config *cfg = dev->config;
#ifdef CONFIG_SPI_SUNXI_DMA
	uint32_t dma_n = 0U;
#endif
	uint32_t deadline = 0U;
	bool started = false;

	if (tx_len + rx_len > SPI_MAX_BURST) {
		return -EMSGSIZE;
	}

	sunxi_spi_set_counters(cfg, quad, tx_len, rx_len);
	while (deadline++ < SPI_TIMEOUT_US) {
		while (spi_context_tx_on(ctx) &&
		       FIELD_GET(FIFO_TX_CNT, sunxi_spi_read(cfg, SPI_FIFO_STA)) < SPI_FIFO_DEPTH) {
			uint8_t value = spi_context_tx_buf_on(ctx) ? *ctx->tx_buf : 0xffU;

			sys_write8(value, cfg->base + SPI_TXDATA);
			spi_context_update_tx(ctx, 1, 1);
		}

		if (!started) {
#ifdef CONFIG_SPI_SUNXI_DMA
			if (!spi_context_tx_on(ctx)) {
				dma_n = sunxi_spi_dma_rx_start(dev, ctx, rx_len);
			}
#endif
			sunxi_spi_write(cfg, SPI_TC, sunxi_spi_read(cfg, SPI_TC) | TC_XCH);
			started = true;
#ifdef CONFIG_SPI_SUNXI_DMA
			if (dma_n != 0U) {
				int ret = sunxi_spi_dma_rx_finish(dev, ctx, dma_n);

				if (ret != 0) {
					sunxi_spi_write(cfg, SPI_INT_STA, 0xffffffffU);
					return ret;
				}
			}
#endif
		}

		uint32_t cnt;

		/* drain what is there: the count is read once for the whole burst */
		while ((cnt = FIELD_GET(FIFO_RX_CNT, sunxi_spi_read(cfg, SPI_FIFO_STA))) != 0U) {
			/* a 32 bit read pops four bytes: straight into an aligned buffer */
			while (cnt >= 4U && spi_context_rx_buf_on(ctx) && ctx->rx_len >= 4U &&
			       ((uintptr_t)ctx->rx_buf & 3U) == 0U) {
				*(uint32_t *)ctx->rx_buf = sys_read32(cfg->base + SPI_RXDATA);
				spi_context_update_rx(ctx, 1, 4);
				cnt -= 4U;
			}
			while (cnt-- != 0U) {
				uint8_t value = sys_read8(cfg->base + SPI_RXDATA);

				if (spi_context_rx_on(ctx)) {
					if (spi_context_rx_buf_on(ctx)) {
						*ctx->rx_buf = value;
					}
					spi_context_update_rx(ctx, 1, 1);
				}
			}
		}

		if ((sunxi_spi_read(cfg, SPI_INT_STA) & INT_STA_ERR) != 0U) {
			sunxi_spi_write(cfg, SPI_INT_STA, 0xffffffffU);
			return -EIO;
		}
		if (!spi_context_tx_on(ctx) && !spi_context_rx_on(ctx) &&
		    (sunxi_spi_read(cfg, SPI_INT_STA) & INT_STA_TC) != 0U &&
		    FIELD_GET(FIFO_RX_CNT, sunxi_spi_read(cfg, SPI_FIFO_STA)) == 0U) {
			sunxi_spi_write(cfg, SPI_INT_STA, 0xffffffffU);
			return 0;
		}
		k_busy_wait(1);
	}

	sunxi_spi_write(cfg, SPI_INT_STA, 0xffffffffU);
	return -ETIMEDOUT;
}

static int sunxi_spi_transceive(const struct device *dev,
					const struct spi_config *config,
					const struct spi_buf_set *tx_bufs,
					const struct spi_buf_set *rx_bufs)
{
	const struct sunxi_spi_config *cfg = dev->config;
	struct sunxi_spi_data *data = dev->data;
	struct spi_context *ctx = &data->ctx;
	uint32_t tx_len;
	uint32_t rx_len;
	int ret;

	spi_context_lock(ctx, false, NULL, NULL, config);
	ctx->config = config;
	ret = sunxi_spi_configure(dev, config);
	if (ret != 0) {
		spi_context_release(ctx, ret);
		return ret;
	}

	spi_context_buffers_setup(ctx, tx_bufs, rx_bufs, 1);
	tx_len = spi_context_total_tx_len(ctx);
	rx_len = spi_context_total_rx_len(ctx);
	if (tx_len == 0U && rx_len == 0U) {
		spi_context_release(ctx, 0);
		return 0;
	}

	if (spi_cs_is_gpio(config)) {
		spi_context_cs_control(ctx, true);
	}
	ret = sunxi_spi_run_transfer(dev, data->quad, ctx, tx_len, rx_len);
	if (spi_cs_is_gpio(config)) {
		spi_context_cs_control(ctx, false);
	} else {
		/* Native CS is released by the end of the hardware burst. */
		sunxi_spi_write(cfg, SPI_TC, sunxi_spi_read(cfg, SPI_TC) | TC_SS_LEVEL);
	}

	spi_context_complete(ctx, dev, ret);
	ret = spi_context_wait_for_completion(ctx);
	spi_context_release(ctx, ret);
	return ret;
}

int sunxi_spi_set_sample(const struct device *dev, uint8_t mode, uint8_t delay)
{
	struct sunxi_spi_data *data = dev->data;

	if (mode >= SUNXI_SPI_SAMPLE_MODES || delay > SAMP_DL_SW) {
		return -EINVAL;
	}
	data->sample_mode = mode;
	data->sample_delay = delay;
	data->sample_set = true;

	return 0;
}

static int sunxi_spi_init(const struct device *dev)
{
	const struct sunxi_spi_config *cfg = dev->config;
	struct sunxi_spi_data *data = dev->data;
	clock_control_subsys_t module =
		(clock_control_subsys_t)(uintptr_t)cfg->module_clock_id;
	clock_control_subsys_t bus =
		(clock_control_subsys_t)(uintptr_t)cfg->bus_clock_id;
	struct spi_config init_cfg = {
		.frequency = cfg->clock_frequency,
		.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8),
		.slave = 0,
	};
	int ret;

#ifdef CONFIG_SPI_SUNXI_DMA
	k_sem_init(&data->dma_done, 0, 1);
#endif
	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret != 0) {
		return ret;
	}
	ret = clock_control_on(cfg->clock_dev, module);
	if (ret != 0) {
		return ret;
	}
	ret = clock_control_on(cfg->clock_dev, bus);
	if (ret != 0) {
		return ret;
	}
	ret = reset_line_toggle_dt(&cfg->reset);
	if (ret != 0) {
		return ret;
	}
	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		return ret;
	}
	ret = sunxi_spi_configure(dev, &init_cfg);
	if (ret != 0) {
		return ret;
	}

	spi_context_unlock_unconditionally(&data->ctx);
	return 0;
}

static int sunxi_spi_release(const struct device *dev,
				     const struct spi_config *config)
{
	ARG_UNUSED(config);
	spi_context_unlock_unconditionally(&((struct sunxi_spi_data *)dev->data)->ctx);
	return 0;
}

static DEVICE_API(spi, sunxi_spi_api) = {
	.transceive = sunxi_spi_transceive,
	.release = sunxi_spi_release,
#ifdef CONFIG_SPI_RTIO
	.iodev_submit = spi_rtio_iodev_default_submit,
#endif
};

#ifdef CONFIG_SPI_SUNXI_DMA
#define SUNXI_SPI_DMA_CFG(inst) \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, dmas), \
		    (.dma = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(inst, rx)), \
		     .dma_channel = DT_INST_DMAS_CELL_BY_NAME(inst, rx, channel), \
		     .dma_slot = DT_INST_PROP(inst, dma_slot),), ())
#else
#define SUNXI_SPI_DMA_CFG(inst)
#endif

#define SUNXI_SPI_INIT(inst) \
	PINCTRL_DT_INST_DEFINE(inst); \
	static struct sunxi_spi_data sunxi_spi_data_##inst = { \
		SPI_CONTEXT_INIT_LOCK(sunxi_spi_data_##inst, ctx), \
		SPI_CONTEXT_INIT_SYNC(sunxi_spi_data_##inst, ctx), \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(inst), ctx) \
	}; \
	static const struct sunxi_spi_config sunxi_spi_cfg_##inst = { \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst), \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(inst)), \
		.base = DT_INST_REG_ADDR(inst), \
		.module_clock_id = DT_INST_CLOCKS_CELL_BY_NAME(inst, mod, clkid), \
		.bus_clock_id = DT_INST_CLOCKS_CELL_BY_NAME(inst, bus, clkid), \
		.clock_reg = DT_INST_PROP(inst, clock_reg), \
		.reset = RESET_DT_SPEC_INST_GET(inst), \
		.clock_frequency = DT_INST_PROP(inst, clock_frequency), \
		SUNXI_SPI_DMA_CFG(inst) \
	}; \
	SPI_DEVICE_DT_INST_DEFINE(inst, sunxi_spi_init, NULL, \
		&sunxi_spi_data_##inst, &sunxi_spi_cfg_##inst, POST_KERNEL, \
		CONFIG_SPI_SUNXI_INIT_PRIORITY, &sunxi_spi_api);

DT_INST_FOREACH_STATUS_OKAY(SUNXI_SPI_INIT)
