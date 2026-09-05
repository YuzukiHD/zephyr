/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT allwinner_sunxi_pwm

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(pwm_sunxi, CONFIG_PWM_LOG_LEVEL);

#define PWM_PCGR		0x40U
#define PWM_PER			0x80U
#define PWM_PCR			0x100U
#define PWM_PPR			0x104U
#define PWM_CH_STRIDE		0x20U

#define PWM_PCCR_BASE		0x20U
#define PWM_PCCR_STRIDE		0x04U
#define PWM_PCCR_DIV_MASK	GENMASK(3, 0)
#define PWM_PCCR_SRC_MASK	GENMASK(8, 7)
#define PWM_PCR_PRESCALE_MASK	GENMASK(7, 0)
#define PWM_PCR_POLARITY		BIT(8)
#define PWM_PCGR_GATE(channel)	BIT(channel)
#define PWM_PCGR_BYPASS(channel)	BIT((channel) + 16U)
#define PWM_PER_ENABLE(channel)	BIT(channel)

#define SUNXI_PWM_CHANNELS	4U
#define SUNXI_PWM_REF_CLK	24000000ULL
#define SUNXI_PWM_FAST_CLK	100000000ULL

struct sunxi_pwm_config {
	const struct pinctrl_dev_config *pcfg;
	const struct device *clock_dev;
	uint32_t clock_id;
	struct reset_dt_spec reset;
	uintptr_t base;
	uint32_t channels;
};

struct sunxi_pwm_data {
	struct k_mutex lock;
};

static inline uint32_t pwm_read(const struct sunxi_pwm_config *cfg,
				uint32_t offset)
{
	return sys_read32(cfg->base + offset);
}

static inline void pwm_write(const struct sunxi_pwm_config *cfg,
				 uint32_t offset, uint32_t value)
{
	sys_write32(value, cfg->base + offset);
}

static inline void pwm_update(const struct sunxi_pwm_config *cfg,
				      uint32_t offset, uint32_t mask,
				      uint32_t value)
{
	uint32_t reg = pwm_read(cfg, offset);

	reg = (reg & ~mask) | (value & mask);
	pwm_write(cfg, offset, reg);
}

static uint32_t pwm_pccr_offset(uint32_t channel)
{
	return PWM_PCCR_BASE + (channel / 2U) * PWM_PCCR_STRIDE;
}

static int sunxi_pwm_set_cycles(const struct device *dev, uint32_t channel,
				uint32_t period_cycles, uint32_t pulse_cycles,
				pwm_flags_t flags)
{
	const struct sunxi_pwm_config *cfg = dev->config;
	struct sunxi_pwm_data *data = dev->data;
	uint64_t period_ns;
	uint64_t source_hz;
	uint64_t source_cycles;
	uint32_t div_m = 0U;
	uint32_t prescale = 0U;
	uint32_t period_count = 0U;
	uint32_t active_count;
	uint32_t pccr;
	uint32_t pcr;
	uint32_t pcgr;
	uint32_t ppr;
	int ret = 0;

	if (channel >= cfg->channels || channel >= SUNXI_PWM_CHANNELS) {
		return -EINVAL;
	}
	if (period_cycles == 0U || pulse_cycles > period_cycles) {
		return -EINVAL;
	}
	if ((flags & ~PWM_POLARITY_MASK) != 0U) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	/* A zero pulse is the Zephyr way to stop a PWM output. */
	if (pulse_cycles == 0U) {
		pwm_update(cfg, PWM_PER, PWM_PER_ENABLE(channel), 0U);
		pwm_update(cfg, PWM_PCGR,
			   PWM_PCGR_GATE(channel) | PWM_PCGR_BYPASS(channel), 0U);
		goto out;
	}

	/* Convert the API's fixed 24 MHz reference cycles to nanoseconds. */
	period_ns = ((uint64_t)period_cycles * NSEC_PER_SEC +
			     (SUNXI_PWM_REF_CLK / 2U)) / SUNXI_PWM_REF_CLK;
	if (period_ns == 0U) {
		ret = -ERANGE;
		goto out;
	}

	if (period_ns <= 10U) {
		/* pwm-ng bypasses the divider and emits the selected source clock. */
		source_hz = SUNXI_PWM_FAST_CLK;
		pccr = pwm_read(cfg, pwm_pccr_offset(channel));
		pccr = (pccr & ~(PWM_PCCR_SRC_MASK | PWM_PCCR_DIV_MASK)) |
			(FIELD_PREP(PWM_PCCR_SRC_MASK, 1U));
		pwm_write(cfg, pwm_pccr_offset(channel), pccr);
		pwm_update(cfg, PWM_PCGR,
			   PWM_PCGR_GATE(channel) | PWM_PCGR_BYPASS(channel),
			   PWM_PCGR_GATE(channel) | PWM_PCGR_BYPASS(channel));
		pwm_update(cfg, PWM_PER, PWM_PER_ENABLE(channel),
			   PWM_PER_ENABLE(channel));
		goto out;
	}

	source_hz = (period_ns <= 334U) ? SUNXI_PWM_FAST_CLK :
		SUNXI_PWM_REF_CLK;
	source_cycles = (source_hz * period_ns + (NSEC_PER_SEC / 2U)) /
		NSEC_PER_SEC;
	if (source_cycles == 0U) {
		ret = -ERANGE;
		goto out;
	}

	/* Match pwm-ng's M divider (1..256) and 8-bit prescaler. */
	for (div_m = 0U; div_m <= 8U && period_count == 0U; div_m++) {
		uint32_t m = BIT(div_m);

		for (prescale = 0U; prescale <= UINT8_MAX; prescale++) {
			uint64_t count = source_cycles / m / (prescale + 1U);

			if (count >= 1U && count <= 65536U) {
				period_count = (uint32_t)count;
				break;
			}
		}
	}
	if (period_count == 0U) {
		ret = -ERANGE;
		goto out;
	}

	active_count = (uint32_t)(((uint64_t)period_count * pulse_cycles) /
				  period_cycles);
	if (active_count == 0U) {
		active_count = 1U;
	}
	if (active_count > period_count) {
		active_count = period_count;
	}

	pccr = pwm_read(cfg, pwm_pccr_offset(channel));
	pccr &= ~(PWM_PCCR_SRC_MASK | PWM_PCCR_DIV_MASK);
	pccr |= FIELD_PREP(PWM_PCCR_SRC_MASK,
			   (source_hz == SUNXI_PWM_FAST_CLK) ? 1U : 0U);
	pccr |= FIELD_PREP(PWM_PCCR_DIV_MASK, div_m);
	pwm_write(cfg, pwm_pccr_offset(channel), pccr);

	pcr = pwm_read(cfg, PWM_PCR + channel * PWM_CH_STRIDE);
	pcr &= ~(PWM_PCR_PRESCALE_MASK | PWM_PCR_POLARITY);
	pcr |= FIELD_PREP(PWM_PCR_PRESCALE_MASK, prescale);
	if ((flags & PWM_POLARITY_MASK) == PWM_POLARITY_INVERTED) {
		pcr |= PWM_PCR_POLARITY;
	}
	pwm_write(cfg, PWM_PCR + channel * PWM_CH_STRIDE, pcr);

	ppr = FIELD_PREP(GENMASK(15, 0), active_count) |
		FIELD_PREP(GENMASK(31, 16), period_count - 1U);
	pwm_write(cfg, PWM_PPR + channel * PWM_CH_STRIDE, ppr);

	/* Program the channel before enabling its output. */
	pcgr = pwm_read(cfg, PWM_PCGR);
	pcgr |= PWM_PCGR_GATE(channel);
	pcgr &= ~PWM_PCGR_BYPASS(channel);
	pwm_write(cfg, PWM_PCGR, pcgr);
	pwm_update(cfg, PWM_PER, PWM_PER_ENABLE(channel),
		   PWM_PER_ENABLE(channel));

out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int sunxi_pwm_get_cycles_per_sec(const struct device *dev,
					uint32_t channel, uint64_t *cycles)
{
	const struct sunxi_pwm_config *cfg = dev->config;

	if (channel >= cfg->channels || channel >= SUNXI_PWM_CHANNELS ||
		cycles == NULL) {
		return -EINVAL;
	}

	*cycles = SUNXI_PWM_REF_CLK;
	return 0;
}

static int sunxi_pwm_init(const struct device *dev)
{
	const struct sunxi_pwm_config *cfg = dev->config;
	struct sunxi_pwm_data *data = dev->data;
	clock_control_subsys_t clock_id =
		(clock_control_subsys_t)(uintptr_t)cfg->clock_id;
	uint32_t mask;
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
	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		return ret;
	}

	mask = 0U;
	for (uint32_t channel = 0U; channel < cfg->channels; channel++) {
		mask |= PWM_PER_ENABLE(channel) | PWM_PCGR_GATE(channel) |
			PWM_PCGR_BYPASS(channel);
	}
	pwm_update(cfg, PWM_PER, mask, 0U);
	pwm_update(cfg, PWM_PCGR, mask, 0U);

	return 0;
}

static DEVICE_API(pwm, sunxi_pwm_api) = {
	.set_cycles = sunxi_pwm_set_cycles,
	.get_cycles_per_sec = sunxi_pwm_get_cycles_per_sec,
};

#define SUNXI_PWM_INIT(inst) \
	PINCTRL_DT_INST_DEFINE(inst); \
	static const struct sunxi_pwm_config sunxi_pwm_cfg_##inst = { \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst), \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(inst)), \
		.clock_id = DT_INST_CLOCKS_CELL(inst, clkid), \
		.reset = RESET_DT_SPEC_INST_GET(inst), \
		.base = DT_INST_REG_ADDR(inst), \
		.channels = DT_INST_PROP(inst, channels), \
	}; \
	static struct sunxi_pwm_data sunxi_pwm_data_##inst; \
	DEVICE_DT_INST_DEFINE(inst, sunxi_pwm_init, NULL, \
		&sunxi_pwm_data_##inst, &sunxi_pwm_cfg_##inst, POST_KERNEL, \
		CONFIG_PWM_SUNXI_INIT_PRIORITY, &sunxi_pwm_api);

DT_INST_FOREACH_STATUS_OKAY(SUNXI_PWM_INIT)
