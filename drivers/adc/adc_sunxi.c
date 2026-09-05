/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT allwinner_sunxi_gpadc

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(adc_sunxi, CONFIG_ADC_LOG_LEVEL);

#define GP_SR_REG		0x00U
#define GP_CTRL_REG		0x04U
#define GP_CS_EN_REG		0x08U
#define GP_DATA_INTC_REG	0x28U
#define GP_DATA_INTS_REG	0x38U
#define GP_CH0_DATA_REG		0x80U

#define GP_CTRL_VCM_BUF_EN	BIT(0)
#define GP_CTRL_CALIBRATION	BIT(17)
#define GP_CTRL_MODE_MASK	GENMASK(19, 18)
#define GP_CTRL_CONTINUOUS	(2U << 18)
#define GP_CTRL_ENABLE		BIT(16)
#define GP_DATA_MASK		GENMASK(11, 0)

#define SUNXI_ADC_CHANNELS	12U
#define SUNXI_ADC_CLOCK_HZ	24000000U
#define SUNXI_ADC_MIN_RATE	400U
#define SUNXI_ADC_MAX_RATE	1000000U
#define SUNXI_ADC_DEFAULT_TIMEOUT_US	10000U

struct sunxi_adc_config {
	uintptr_t base;
	const struct device *clock_dev;
	uint32_t clock_id;
	struct reset_dt_spec reset;
	uint32_t channels;
	uint32_t vref_mv;
	uint32_t sample_rate;
	uint32_t timeout_us;
};

struct sunxi_adc_data {
	struct k_mutex lock;
	uint32_t configured;
};

static inline uint32_t adc_read_reg(const struct sunxi_adc_config *cfg,
					uint32_t offset)
{
	return sys_read32(cfg->base + offset);
}

static inline void adc_write_reg(const struct sunxi_adc_config *cfg,
					 uint32_t offset, uint32_t value)
{
	sys_write32(value, cfg->base + offset);
}

static int sunxi_adc_channel_setup(const struct device *dev,
					   const struct adc_channel_cfg *channel_cfg)
{
	const struct sunxi_adc_config *cfg = dev->config;
	struct sunxi_adc_data *data = dev->data;

	if (channel_cfg == NULL || channel_cfg->channel_id >= cfg->channels ||
		channel_cfg->channel_id >= SUNXI_ADC_CHANNELS) {
		return -EINVAL;
	}
	if (channel_cfg->gain != ADC_GAIN_1 ||
		channel_cfg->reference != ADC_REF_INTERNAL ||
		channel_cfg->acquisition_time != ADC_ACQ_TIME_DEFAULT ||
		channel_cfg->differential) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	data->configured |= BIT(channel_cfg->channel_id);
	k_mutex_unlock(&data->lock);

	return 0;
}

static int sunxi_adc_read_channel(const struct sunxi_adc_config *cfg,
					  uint32_t channel, uint16_t *sample)
{
	const uint32_t bit = BIT(channel);
	uint32_t status;

	/* Select one input at a time so the result order is deterministic. */
	adc_write_reg(cfg, GP_CS_EN_REG, bit);
	adc_write_reg(cfg, GP_DATA_INTS_REG, bit);

	/* The hardware needs about 1.5 ms after changing the selected input. */
	k_busy_wait(1500U);
	for (uint32_t elapsed = 0U; elapsed < cfg->timeout_us; elapsed++) {
		status = adc_read_reg(cfg, GP_DATA_INTS_REG);
		if ((status & bit) != 0U) {
			*sample = (uint16_t)(adc_read_reg(cfg,
					GP_CH0_DATA_REG + channel * sizeof(uint32_t)) &
					GP_DATA_MASK);
			/* DATA_INTS is write-one-to-clear. */
			adc_write_reg(cfg, GP_DATA_INTS_REG, bit);
			return 0;
		}
		k_busy_wait(1U);
	}

	return -ETIMEDOUT;
}

static int sunxi_adc_read(const struct device *dev,
				  const struct adc_sequence *sequence)
{
	const struct sunxi_adc_config *cfg = dev->config;
	struct sunxi_adc_data *data = dev->data;
	uint32_t channel_count = 0U;
	uint32_t channel_mask;
	uint16_t *buffer;
	int ret = 0;

	if (sequence == NULL || sequence->buffer == NULL ||
		sequence->resolution != 12U || sequence->oversampling != 0U ||
		sequence->channels == 0U) {
		return -EINVAL;
	}
	if (sequence->options != NULL &&
		(sequence->options->interval_us != 0U ||
		 sequence->options->callback != NULL ||
		 sequence->options->extra_samplings != 0U)) {
		return -ENOTSUP;
	}

	channel_mask = BIT_MASK(cfg->channels);
	if ((sequence->channels & ~channel_mask) != 0U) {
		return -EINVAL;
	}
	for (uint32_t channel = 0U; channel < cfg->channels; channel++) {
		if ((sequence->channels & BIT(channel)) != 0U) {
			channel_count++;
		}
	}
	if (sequence->buffer_size < channel_count * sizeof(uint16_t)) {
		return -ENOMEM;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	if ((sequence->channels & ~data->configured) != 0U) {
		ret = -EINVAL;
		goto out;
	}

	buffer = sequence->buffer;
	for (uint32_t channel = 0U; channel < cfg->channels; channel++) {
		if ((sequence->channels & BIT(channel)) == 0U) {
			continue;
		}

		ret = sunxi_adc_read_channel(cfg, channel, buffer++);
		if (ret != 0) {
			break;
		}
	}
	adc_write_reg(cfg, GP_CS_EN_REG, 0U);

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int sunxi_adc_init(const struct device *dev)
{
	const struct sunxi_adc_config *cfg = dev->config;
	struct sunxi_adc_data *data = dev->data;
	clock_control_subsys_t clock_id =
		(clock_control_subsys_t)(uintptr_t)cfg->clock_id;
	uint32_t divider;
	uint32_t sample_rate_reg;
	uint32_t control;
	int ret;

	k_mutex_init(&data->lock);
	ret = clock_control_on(cfg->clock_dev, clock_id);
	if (ret != 0) {
		return ret;
	}
	ret = reset_line_toggle_dt(&cfg->reset);
	if (ret != 0) {
		return ret;
	}
	if (cfg->sample_rate < SUNXI_ADC_MIN_RATE ||
		cfg->sample_rate > SUNXI_ADC_MAX_RATE) {
		return -EINVAL;
	}

	divider = SUNXI_ADC_CLOCK_HZ / cfg->sample_rate - 1U;
	if (divider > UINT16_MAX) {
		return -EINVAL;
	}
	sample_rate_reg = adc_read_reg(cfg, GP_SR_REG);
	sample_rate_reg = (sample_rate_reg & GENMASK(15, 0)) |
		(divider << 16);
	adc_write_reg(cfg, GP_SR_REG, sample_rate_reg);

	/* Continuous conversion, calibration and the F101 VCM buffer. */
	control = adc_read_reg(cfg, GP_CTRL_REG);
	control &= ~GP_CTRL_MODE_MASK;
	control |= GP_CTRL_CONTINUOUS | GP_CTRL_CALIBRATION |
		GP_CTRL_VCM_BUF_EN | GP_CTRL_ENABLE;
	adc_write_reg(cfg, GP_CTRL_REG, control);
	adc_write_reg(cfg, GP_CS_EN_REG, 0U);
	adc_write_reg(cfg, GP_DATA_INTC_REG, 0U);
	adc_write_reg(cfg, GP_DATA_INTS_REG, BIT_MASK(cfg->channels));

	return 0;
}

static DEVICE_API(adc, sunxi_adc_api) = {
	.channel_setup = sunxi_adc_channel_setup,
	.read = sunxi_adc_read,
	.ref_internal = 1800U,
};

#define SUNXI_ADC_INIT(inst) \
	static const struct sunxi_adc_config sunxi_adc_cfg_##inst = { \
		.base = DT_INST_REG_ADDR(inst), \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(inst)), \
		.clock_id = DT_INST_CLOCKS_CELL(inst, clkid), \
		.reset = RESET_DT_SPEC_INST_GET(inst), \
		.channels = DT_INST_PROP(inst, channels), \
		.vref_mv = DT_INST_PROP(inst, vref_mv), \
		.sample_rate = DT_INST_PROP(inst, sample_rate), \
		.timeout_us = DT_INST_PROP_OR(inst, read_timeout_us, \
			SUNXI_ADC_DEFAULT_TIMEOUT_US), \
	}; \
	static struct sunxi_adc_data sunxi_adc_data_##inst; \
	DEVICE_DT_INST_DEFINE(inst, sunxi_adc_init, NULL, \
		&sunxi_adc_data_##inst, &sunxi_adc_cfg_##inst, POST_KERNEL, \
		CONFIG_ADC_SUNXI_INIT_PRIORITY, &sunxi_adc_api);

DT_INST_FOREACH_STATUS_OKAY(SUNXI_ADC_INIT)
