/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT allwinner_sunxi_dbi

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(mipi_dbi_sunxi, CONFIG_MIPI_DBI_LOG_LEVEL);

#define SPI_GCR			0x04U
#define SPI_TC			0x08U
#define SPI_INT_STA		0x14U
#define SPI_FIFO_CTL		0x18U
#define SPI_FIFO_STA		0x1cU
#define SPI_CLK_CTL		0x24U
#define SPI_BURST_CNT		0x30U
#define SPI_TX_CNT		0x34U
#define SPI_BCC			0x38U
#define DBI_CTRL0		0x100U
#define DBI_CTRL1		0x104U
#define DBI_CTRL2		0x108U
#define DBI_SIZE		0x110U
#define DBI_INT			0x120U
#define DBI_TXFIFO		0x200U

#define SPI_GCR_ENABLE		BIT(0)
#define SPI_GCR_MASTER		BIT(1)
#define SPI_GCR_DBI_MODE	BIT(3)
#define SPI_GCR_DBI_ENABLE	BIT(4)
#define SPI_GCR_SOFT_RESET	BIT(31)

#define SPI_TC_SS_LEVEL		BIT(7)
#define SPI_TC_SS_OWNER		BIT(6)
#define SPI_TC_SPOL		BIT(2)
#define SPI_TC_DHB		BIT(8)
#define SPI_TC_SDM		BIT(13)
#define SPI_TC_XCH		BIT(31)

#define SPI_FIFO_TX_RESET	BIT(31)
#define SPI_FIFO_TX_DRQEN	BIT(24)
#define SPI_FIFO_TX_COUNT	GENMASK(23, 16)
#define SPI_FIFO_DEPTH		64U

#define DBI_CTRL0_INTERFACE	GENMASK(10, 8)
#define DBI_CTRL0_FORMAT	GENMASK(14, 12)
#define DBI_CTRL1_DCX_DATA	BIT(22)
#define DBI_CTRL2_HRDY_BYPASS	BIT(31)
#define DBI_CTRL2_DCX_PIN	BIT(5)
#define DBI_CTRL2_DMA_ENABLE	BIT(15)
#define DBI_INT_STATUS_MASK	GENMASK(14, 8)

#define SUNXI_DBI_MODULE_HZ	24000000U
#define SUNXI_DBI_MAX_BURST	0x00ffffffU
#define SUNXI_DBI_INTERFACE_D2LI	4U

struct sunxi_dbi_config {
	const struct pinctrl_dev_config *pcfg;
	const struct device *clock_dev;
	uint32_t module_clock_id;
	uint32_t bus_clock_id;
	struct reset_dt_spec reset;
	struct gpio_dt_spec reset_gpio;
	struct gpio_dt_spec cs_gpio;
	uintptr_t base;
	uint32_t clock_reg;
	uint32_t clock_frequency;
	const struct device *dma_dev;
	uint32_t dma_channel;
};

struct sunxi_dbi_data {
	struct k_mutex lock;
	struct k_sem dma_sem;
	int dma_status;
};

static inline uint32_t dbi_read(const struct sunxi_dbi_config *cfg,
				uint32_t offset)
{
	return sys_read32(cfg->base + offset);
}

static inline void dbi_write(const struct sunxi_dbi_config *cfg,
				 uint32_t offset, uint32_t value)
{
	sys_write32(value, cfg->base + offset);
}

static int dbi_wait_fifo_space(const struct sunxi_dbi_config *cfg)
{
	for (uint32_t elapsed = 0U; elapsed < CONFIG_MIPI_DBI_SUNXI_TIMEOUT_US;
	     elapsed++) {
		if (FIELD_GET(SPI_FIFO_TX_COUNT, dbi_read(cfg, SPI_FIFO_STA)) <
		    SPI_FIFO_DEPTH) {
			return 0;
		}
		k_busy_wait(1U);
	}

	return -ETIMEDOUT;
}

static int dbi_wait_complete(const struct sunxi_dbi_config *cfg)
{
	for (uint32_t elapsed = 0U; elapsed < CONFIG_MIPI_DBI_SUNXI_TIMEOUT_US;
	     elapsed++) {
		uint32_t fifo = dbi_read(cfg, SPI_FIFO_STA);
		if (FIELD_GET(SPI_FIFO_TX_COUNT, fifo) == 0U &&
		    dbi_read(cfg, SPI_BURST_CNT) == 0U) {
			dbi_write(cfg, SPI_INT_STA, 0xffffffffU);
			dbi_write(cfg, DBI_INT, DBI_INT_STATUS_MASK);
			return 0;
		}
		k_busy_wait(1U);
	}

	dbi_write(cfg, SPI_INT_STA, 0xffffffffU);
	dbi_write(cfg, DBI_INT, DBI_INT_STATUS_MASK);
	return -ETIMEDOUT;
}

static int dbi_set_clock(const struct sunxi_dbi_config *cfg)
{
	uint32_t divider = 0U;
	uint32_t reg;

	if (cfg->clock_frequency < 1000U ||
		cfg->clock_frequency > SUNXI_DBI_MODULE_HZ) {
		return -EINVAL;
	}

	/* The F101 SPI module uses HOSC24M divided by powers of two. */
	while (divider < 15U &&
	       (SUNXI_DBI_MODULE_HZ >> divider) > cfg->clock_frequency) {
		divider++;
	}
	sys_write32(BIT(31) | (divider << 8),
			0x02001000U + cfg->clock_reg);

	reg = dbi_read(cfg, SPI_CLK_CTL);
	reg &= ~(GENMASK(11, 8) | BIT(12) | GENMASK(7, 0));
	reg |= divider << 8;
	dbi_write(cfg, SPI_CLK_CTL, reg);

	return 0;
}

static int dbi_reset_fifo(const struct sunxi_dbi_config *cfg)
{
	uint32_t reg = dbi_read(cfg, SPI_FIFO_CTL);

	reg |= SPI_FIFO_TX_RESET;
	reg &= ~(GENMASK(23, 16) | GENMASK(7, 0));
	reg |= (32U << 16) | 32U;
	dbi_write(cfg, SPI_FIFO_CTL, reg);

	for (uint32_t elapsed = 0U; elapsed < CONFIG_MIPI_DBI_SUNXI_TIMEOUT_US;
	     elapsed++) {
		if ((dbi_read(cfg, SPI_FIFO_CTL) & SPI_FIFO_TX_RESET) == 0U) {
			return 0;
		}
		k_busy_wait(1U);
	}

	return -ETIMEDOUT;
}

static int dbi_set_cs(const struct sunxi_dbi_config *cfg, bool active)
{
	if (cfg->cs_gpio.port == NULL) {
		return 0;
	}

	/* GPIO_ACTIVE_LOW maps logical active=1 to the CE low level. */
	return gpio_pin_set_dt(&cfg->cs_gpio, active ? 1 : 0);
}

static int dbi_transfer_chunk(const struct sunxi_dbi_config *cfg,
				      const uint8_t *buf, size_t len, bool final)
{
	uint32_t reg;
	int ret;

	if (buf == NULL || len == 0U || len > SUNXI_DBI_MAX_BURST) {
		return -EINVAL;
	}

	ret = dbi_reset_fifo(cfg);
	if (ret != 0) {
		return ret;
	}
	ret = dbi_set_cs(cfg, true);
	if (ret != 0) {
		return ret;
	}

	/* DBI and SPI counters count bytes in the current transaction. */
	dbi_write(cfg, SPI_BURST_CNT, len);
	dbi_write(cfg, SPI_TX_CNT, len);
	dbi_write(cfg, SPI_BCC, len & GENMASK(23, 0));
	dbi_write(cfg, SPI_INT_STA, 0xffffffffU);
	dbi_write(cfg, DBI_INT, DBI_INT_STATUS_MASK);

	/* Assert the controller's active CS level for this transaction. */
	reg = dbi_read(cfg, SPI_TC) & ~(SPI_TC_SS_LEVEL | SPI_TC_XCH);
	dbi_write(cfg, SPI_TC, reg);

	for (size_t i = 0U; i < MIN(len, SPI_FIFO_DEPTH); i++) {
		ret = dbi_wait_fifo_space(cfg);
		if (ret != 0) {
			goto out_cs;
		}
		sys_write8(buf[i], cfg->base + DBI_TXFIFO);
	}

	reg = dbi_read(cfg, SPI_TC);
	dbi_write(cfg, SPI_TC, reg | SPI_TC_XCH);

	for (size_t i = SPI_FIFO_DEPTH; i < len; i++) {
		ret = dbi_wait_fifo_space(cfg);
		if (ret != 0) {
			goto out_cs;
		}
		sys_write8(buf[i], cfg->base + DBI_TXFIFO);
	}

	ret = dbi_wait_complete(cfg);

out_cs:
	reg = dbi_read(cfg, SPI_TC);
	dbi_write(cfg, SPI_TC, reg | SPI_TC_SS_LEVEL);
	(void)dbi_set_cs(cfg, false);
	ARG_UNUSED(final);

	return ret;
}

static int dbi_transfer_dma(const struct sunxi_dbi_config *cfg,
				    struct sunxi_dbi_data *data,
				    const uint8_t *buf, size_t len, bool final);

static int dbi_transfer(const struct sunxi_dbi_config *cfg,
				struct sunxi_dbi_data *data,
				const uint8_t *buf, size_t len)
{
	if (cfg->dma_dev != NULL && device_is_ready(cfg->dma_dev) &&
	    len > SPI_FIFO_DEPTH) {
		return dbi_transfer_dma(cfg, data, buf, len, true);
	}

	while (len != 0U) {
		size_t chunk = MIN(len, SPI_FIFO_DEPTH);
		int ret = dbi_transfer_chunk(cfg, buf, chunk, chunk == len);

		if (ret == -ETIMEDOUT && chunk <= SPI_FIFO_DEPTH) {
			/* A single-byte DBI burst can miss the first XCH edge on F101. */
			ret = dbi_transfer_chunk(cfg, buf, chunk, chunk == len);
		}

		if (ret != 0) {
			/* Always release CS when a chunk fails. */
			uint32_t reg = dbi_read(cfg, SPI_TC);
			dbi_write(cfg, SPI_TC, reg | SPI_TC_SS_LEVEL);
			return ret;
		}
		buf += chunk;
		len -= chunk;
	}

	return 0;
}

static void sunxi_dbi_dma_callback(const struct device *dev, void *user_data,
					   uint32_t channel, int status)
{
	struct sunxi_dbi_data *data = user_data;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel);
	data->dma_status = status;
	k_sem_give(&data->dma_sem);
}

static int dbi_transfer_dma(const struct sunxi_dbi_config *cfg,
				    struct sunxi_dbi_data *data,
				    const uint8_t *buf, size_t len, bool final)
{
	struct dma_block_config block = {
		.source_address = (uint32_t)(uintptr_t)buf,
		.dest_address = (uint32_t)(cfg->base + DBI_TXFIFO),
		.block_size = len,
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
	};
	struct dma_config dma_cfg = {
		.dma_slot = 23U,
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.complete_callback_en = 1U,
		.source_data_size = 1U,
		.dest_data_size = 1U,
		.source_burst_length = 16U,
		.dest_burst_length = 16U,
		.block_count = 1U,
		.head_block = &block,
		.user_data = data,
		.dma_callback = sunxi_dbi_dma_callback,
	};
	uint32_t reg;
	int ret;

	if (!device_is_ready(cfg->dma_dev) || len == 0U || len > SUNXI_DBI_MAX_BURST) {
		return -ENOTSUP;
	}

	ret = dbi_reset_fifo(cfg);
	if (ret != 0) {
		return ret;
	}
	dbi_write(cfg, SPI_BURST_CNT, len);
	dbi_write(cfg, SPI_TX_CNT, len);
	dbi_write(cfg, SPI_BCC, len & GENMASK(23, 0));
	dbi_write(cfg, SPI_INT_STA, 0xffffffffU);
	dbi_write(cfg, DBI_INT, DBI_INT_STATUS_MASK);
	reg = dbi_read(cfg, SPI_TC) & ~SPI_TC_SS_LEVEL;
	dbi_write(cfg, SPI_TC, reg);
	ret = dbi_set_cs(cfg, true);
	if (ret != 0) {
		goto out_cs;
	}

	k_sem_reset(&data->dma_sem);
	data->dma_status = -EINPROGRESS;
	ret = dma_config(cfg->dma_dev, cfg->dma_channel, &dma_cfg);
	if (ret != 0) {
		goto out_cs;
	}

	reg = dbi_read(cfg, SPI_FIFO_CTL) | SPI_FIFO_TX_DRQEN;
	dbi_write(cfg, SPI_FIFO_CTL, reg);
	reg = dbi_read(cfg, DBI_CTRL2) | DBI_CTRL2_DMA_ENABLE;
	dbi_write(cfg, DBI_CTRL2, reg);

	ret = dma_start(cfg->dma_dev, cfg->dma_channel);
	if (ret != 0) {
		goto out_dma;
	}
	reg = dbi_read(cfg, SPI_TC);
	dbi_write(cfg, SPI_TC, reg | SPI_TC_XCH);

	ret = k_sem_take(&data->dma_sem,
			K_USEC(CONFIG_MIPI_DBI_SUNXI_TIMEOUT_US));
	if (ret != 0) {
		ret = -ETIMEDOUT;
		(void)dma_stop(cfg->dma_dev, cfg->dma_channel);
		goto out_dma;
	}
	if (data->dma_status < 0) {
		ret = data->dma_status;
		goto out_dma;
	}
	ret = dbi_wait_complete(cfg);

out_dma:
	reg = dbi_read(cfg, SPI_FIFO_CTL) & ~SPI_FIFO_TX_DRQEN;
	dbi_write(cfg, SPI_FIFO_CTL, reg);
	reg = dbi_read(cfg, DBI_CTRL2) & ~DBI_CTRL2_DMA_ENABLE;
	dbi_write(cfg, DBI_CTRL2, reg);
out_cs:
	reg = dbi_read(cfg, SPI_TC);
	dbi_write(cfg, SPI_TC, reg | SPI_TC_SS_LEVEL);
	(void)dbi_set_cs(cfg, false);
	ARG_UNUSED(final);
	return ret;
}

static int dbi_set_dcx(const struct sunxi_dbi_config *cfg, bool data)
{
	uint32_t reg = dbi_read(cfg, DBI_CTRL1);

	if (data) {
		reg |= DBI_CTRL1_DCX_DATA;
	} else {
		reg &= ~DBI_CTRL1_DCX_DATA;
	}
	dbi_write(cfg, DBI_CTRL1, reg);
	return 0;
}

static int sunxi_dbi_command_write(const struct device *dev,
					   const struct mipi_dbi_config *dbi_config,
					   uint8_t cmd, const uint8_t *data, size_t len)
{
	const struct sunxi_dbi_config *cfg = dev->config;
	struct sunxi_dbi_data *drv_data = dev->data;
	int ret;

	if (dbi_config == NULL ||
		dbi_config->mode != MIPI_DBI_MODE_SPI_4WIRE) {
		return -ENOTSUP;
	}

	k_mutex_lock(&drv_data->lock, K_FOREVER);
	ret = dbi_set_dcx(cfg, false);
	if (ret == 0) {
		ret = dbi_transfer(cfg, drv_data, &cmd, 1U);
	}
	if (ret == 0 && len != 0U) {
		ret = dbi_set_dcx(cfg, true);
	}
	if (ret == 0 && len != 0U) {
		ret = dbi_transfer(cfg, drv_data, data, len);
	}
	k_mutex_unlock(&drv_data->lock);

	return ret;
}

static int sunxi_dbi_write_display(const struct device *dev,
					   const struct mipi_dbi_config *dbi_config,
					   const uint8_t *framebuf,
					   struct display_buffer_descriptor *desc,
					   enum display_pixel_format pixfmt)
{
	const struct sunxi_dbi_config *cfg = dev->config;
	struct sunxi_dbi_data *drv_data = dev->data;
	int ret;

	ARG_UNUSED(pixfmt);
	if (dbi_config == NULL || desc == NULL || framebuf == NULL ||
		dbi_config->mode != MIPI_DBI_MODE_SPI_4WIRE ||
		desc->buf_size == 0U) {
		return -EINVAL;
	}

	k_mutex_lock(&drv_data->lock, K_FOREVER);
	ret = dbi_set_dcx(cfg, true);
	if (ret == 0) {
		ret = dbi_transfer(cfg, drv_data, framebuf, desc->buf_size);
	}
	k_mutex_unlock(&drv_data->lock);

	return ret;
}

static int sunxi_dbi_reset(const struct device *dev, k_timeout_t delay)
{
	const struct sunxi_dbi_config *cfg = dev->config;
	int ret;

	if (cfg->reset_gpio.port == NULL) {
		return -ENOTSUP;
	}
	ret = gpio_pin_set_dt(&cfg->reset_gpio, 1);
	if (ret != 0) {
		return ret;
	}
	k_sleep(delay);
	return gpio_pin_set_dt(&cfg->reset_gpio, 0);
}

static int sunxi_dbi_init(const struct device *dev)
{
	const struct sunxi_dbi_config *cfg = dev->config;
	struct sunxi_dbi_data *data = dev->data;
	clock_control_subsys_t module =
		(clock_control_subsys_t)(uintptr_t)cfg->module_clock_id;
	clock_control_subsys_t bus =
		(clock_control_subsys_t)(uintptr_t)cfg->bus_clock_id;
	uint32_t reg;
	int ret;

	k_mutex_init(&data->lock);
	k_sem_init(&data->dma_sem, 0, 1);
	data->dma_status = -EINPROGRESS;
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
	if (cfg->reset_gpio.port != NULL) {
		ret = gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_INACTIVE);
		if (ret != 0) {
			return ret;
		}
	}
	if (cfg->cs_gpio.port != NULL) {
		ret = gpio_pin_configure_dt(&cfg->cs_gpio, GPIO_OUTPUT_INACTIVE);
		if (ret != 0) {
			return ret;
		}
	}
	ret = dbi_set_clock(cfg);
	if (ret != 0) {
		return ret;
	}

	reg = dbi_read(cfg, SPI_GCR);
	reg |= SPI_GCR_ENABLE | SPI_GCR_MASTER | SPI_GCR_DBI_MODE |
		SPI_GCR_DBI_ENABLE;
	reg &= ~SPI_GCR_SOFT_RESET;
	dbi_write(cfg, SPI_GCR, reg);

	/* Active-low CS handling. */
	reg = SPI_TC_SPOL | SPI_TC_SDM | SPI_TC_DHB | SPI_TC_SS_OWNER;
	dbi_write(cfg, SPI_TC, reg);

	reg = dbi_read(cfg, DBI_CTRL0);
	reg &= ~(DBI_CTRL0_INTERFACE | DBI_CTRL0_FORMAT);
	reg |= FIELD_PREP(DBI_CTRL0_INTERFACE, SUNXI_DBI_INTERFACE_D2LI);
	dbi_write(cfg, DBI_CTRL0, reg);

	reg = DBI_CTRL2_HRDY_BYPASS | DBI_CTRL2_DCX_PIN;
	dbi_write(cfg, DBI_CTRL2, reg);
	dbi_write(cfg, DBI_CTRL1, 0U);
	dbi_write(cfg, DBI_SIZE, 0U);
	dbi_write(cfg, DBI_INT, 0U);
	dbi_write(cfg, SPI_INT_STA, 0xffffffffU);

	return dbi_reset_fifo(cfg);
}

static DEVICE_API(mipi_dbi, sunxi_dbi_api) = {
	.command_write = sunxi_dbi_command_write,
	.write_display = sunxi_dbi_write_display,
	.reset = sunxi_dbi_reset,
};

#define SUNXI_DBI_INIT(inst) \
	PINCTRL_DT_INST_DEFINE(inst); \
	static const struct sunxi_dbi_config sunxi_dbi_cfg_##inst = { \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst), \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(inst, mod)), \
		.module_clock_id = DT_INST_CLOCKS_CELL_BY_NAME(inst, mod, clkid), \
		.bus_clock_id = DT_INST_CLOCKS_CELL_BY_NAME(inst, bus, clkid), \
		.reset = RESET_DT_SPEC_INST_GET(inst), \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}), \
		.cs_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, cs_gpios, {0}), \
		.base = DT_INST_REG_ADDR(inst), \
		.clock_reg = DT_INST_PROP(inst, clock_reg), \
		.clock_frequency = DT_INST_PROP(inst, clock_frequency), \
		.dma_dev = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, dmas), \
			(DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(inst, tx))), (NULL)), \
		.dma_channel = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, dmas), \
			(DT_INST_DMAS_CELL_BY_NAME(inst, tx, channel)), (0)), \
	}; \
	static struct sunxi_dbi_data sunxi_dbi_data_##inst; \
	DEVICE_DT_INST_DEFINE(inst, sunxi_dbi_init, NULL, \
		&sunxi_dbi_data_##inst, &sunxi_dbi_cfg_##inst, POST_KERNEL, \
		CONFIG_MIPI_DBI_SUNXI_INIT_PRIORITY, &sunxi_dbi_api);

DT_INST_FOREACH_STATUS_OKAY(SUNXI_DBI_INIT)
