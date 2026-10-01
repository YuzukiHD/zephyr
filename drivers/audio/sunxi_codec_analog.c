/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Analog front end of the on-chip audio codec: headphone driver, external
 * speaker amplifier enable, microphone / FM / line input stage and the
 * volume registers. The registers live in the address space of the parent
 * node, the sample streams belong to the I2S_SUNXI_CODEC driver.
 */

#define DT_DRV_COMPAT allwinner_sunxi_codec_analog

#include <errno.h>
#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(sunxi_codec_analog, CONFIG_AUDIO_CODEC_LOG_LEVEL);

/* digital */
#define DAC_DPC			0x00
#define DAC_VOL_CTL		0x04
#define ADC_VOL_CTL		0x34
#define ADC_DIG_CTL		0x50
#define DAC_DIG_EN		BIT(31)
#define DAC_DVOL_SHIFT		12
#define DAC_DVOL_MASK		(0x3fU << DAC_DVOL_SHIFT)
#define DAC_VOL_L_SHIFT		8
#define DAC_VOL_R_SHIFT		0
#define DAC_VOL_SEL		BIT(16)

/* analog */
#define ADC1_AN			0x300
#define ADC1_EN			BIT(31)
#define MIC1_PGA_EN		BIT(30)
#define FMINLEN			BIT(27)
#define LINEINLEN		BIT(23)
#define LINEINL_GAIN_SHIFT	18
#define FML_GAIN_SHIFT		16
#define ADC1_PGA_GAIN_SHIFT	8
#define DAC_AN			0x310
#define DACL_EN			BIT(15)
#define DACR_EN			BIT(14)
#define LMUTE			BIT(12)
#define RMUTE			BIT(10)
#define RAMP			0x31c
#define RMC_EN			BIT(1)
#define RK_OPT_EN		BIT(21)
#define HP2			0x340
#define HPFB_BUF_EN		BIT(31)
#define HP_GAIN_SHIFT		28
#define HP_GAIN_MASK		(0x7U << HP_GAIN_SHIFT)
#define HP_DRVEN		BIT(21)
#define RSWITCH			BIT(19)
#define HPFB_IN_EN		BIT(17)
#define RAMP_OUT_EN		BIT(15)

enum adc_source {
	ADC_SRC_NONE,
	ADC_SRC_MIC,
	ADC_SRC_FMIN,
	ADC_SRC_LINEIN,
};

struct analog_config {
	uintptr_t base;
	struct gpio_dt_spec pa;
	uint32_t pa_delay_ms;
	uint8_t hp_gain;
	uint8_t dac_volume;
	uint8_t adc_volume;
	uint8_t adc_gain;
	enum adc_source adc_source;
};

struct analog_data {
	struct k_mutex lock;
	bool output_on;
	bool muted[2];
	uint8_t dac_volume[2];
	uint8_t adc_volume;
	bool adc_muted;
	enum adc_source source;
};

static inline uint32_t rd(const struct device *dev, uint32_t off)
{
	const struct analog_config *cfg = dev->config;

	return sys_read32(cfg->base + off);
}

static inline void upd(const struct device *dev, uint32_t off, uint32_t mask, uint32_t val)
{
	const struct analog_config *cfg = dev->config;

	sys_write32((sys_read32(cfg->base + off) & ~mask) | (val & mask), cfg->base + off);
}

static void apply_mute(const struct device *dev)
{
	struct analog_data *data = dev->data;
	uint32_t on = 0;

	if (data->output_on) {
		on |= data->muted[0] ? 0U : LMUTE;
		on |= data->muted[1] ? 0U : RMUTE;
	}
	upd(dev, DAC_AN, LMUTE | RMUTE, on);
}

static void apply_dac_volume(const struct device *dev)
{
	struct analog_data *data = dev->data;

	upd(dev, DAC_VOL_CTL, (0xffU << DAC_VOL_L_SHIFT) | (0xffU << DAC_VOL_R_SHIFT),
	    ((uint32_t)data->dac_volume[0] << DAC_VOL_L_SHIFT) |
	    ((uint32_t)data->dac_volume[1] << DAC_VOL_R_SHIFT));
}

static void apply_source(const struct device *dev)
{
	struct analog_data *data = dev->data;
	uint32_t v = 0;

	switch (data->source) {
	case ADC_SRC_MIC:
		v = ADC1_EN | MIC1_PGA_EN;
		break;
	case ADC_SRC_FMIN:
		v = ADC1_EN | FMINLEN;
		break;
	case ADC_SRC_LINEIN:
		v = ADC1_EN | LINEINLEN;
		break;
	default:
		break;
	}
	upd(dev, ADC1_AN, ADC1_EN | MIC1_PGA_EN | FMINLEN | LINEINLEN, v);
}

static void analog_start_output(const struct device *dev)
{
	const struct analog_config *cfg = dev->config;
	struct analog_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (!data->output_on) {
		data->output_on = true;
		apply_mute(dev);
		upd(dev, DAC_DPC, DAC_DIG_EN, DAC_DIG_EN);
		upd(dev, DAC_AN, DACL_EN | DACR_EN, DACL_EN | DACR_EN);
		upd(dev, HP2, HPFB_BUF_EN | HPFB_IN_EN, HPFB_BUF_EN | HPFB_IN_EN);
		upd(dev, HP2, RAMP_OUT_EN | RSWITCH, RAMP_OUT_EN | RSWITCH);
		upd(dev, HP2, HP_DRVEN, HP_DRVEN);
		if (cfg->pa.port != NULL) {
			gpio_pin_set_dt(&cfg->pa, 1);
			k_msleep(cfg->pa_delay_ms);
		}
	}
	k_mutex_unlock(&data->lock);
}

static void analog_stop_output(const struct device *dev)
{
	const struct analog_config *cfg = dev->config;
	struct analog_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (data->output_on) {
		if (cfg->pa.port != NULL) {
			gpio_pin_set_dt(&cfg->pa, 0);
		}
		upd(dev, HP2, HP_DRVEN, 0U);
		data->output_on = false;
		apply_mute(dev);
		upd(dev, HP2, RAMP_OUT_EN | RSWITCH, 0U);
		upd(dev, HP2, HPFB_BUF_EN | HPFB_IN_EN, 0U);
		upd(dev, DAC_DPC, DAC_DIG_EN, 0U);
		upd(dev, DAC_AN, DACL_EN | DACR_EN, 0U);
	}
	k_mutex_unlock(&data->lock);
}

static int analog_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	ARG_UNUSED(dev);

	if (cfg == NULL) {
		return -EINVAL;
	}
	/* the stream format and clocks belong to the I2S device */
	return 0;
}

static bool left(audio_channel_t ch)
{
	return ch == AUDIO_CHANNEL_FRONT_LEFT || ch == AUDIO_CHANNEL_HEADPHONE_LEFT ||
	       ch == AUDIO_CHANNEL_ALL;
}

static bool right(audio_channel_t ch)
{
	return ch == AUDIO_CHANNEL_FRONT_RIGHT || ch == AUDIO_CHANNEL_HEADPHONE_RIGHT ||
	       ch == AUDIO_CHANNEL_ALL;
}

static int analog_set_property(const struct device *dev, audio_property_t property,
			       audio_channel_t channel, audio_property_value_t val)
{
	struct analog_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	switch (property) {
	case AUDIO_PROPERTY_OUTPUT_VOLUME:
		if (val.vol < 0 || val.vol > 255) {
			ret = -EINVAL;
			break;
		}
		if (left(channel)) {
			data->dac_volume[0] = val.vol;
		}
		if (right(channel)) {
			data->dac_volume[1] = val.vol;
		}
		apply_dac_volume(dev);
		break;
	case AUDIO_PROPERTY_OUTPUT_MUTE:
		if (left(channel)) {
			data->muted[0] = val.mute;
		}
		if (right(channel)) {
			data->muted[1] = val.mute;
		}
		apply_mute(dev);
		break;
	case AUDIO_PROPERTY_INPUT_VOLUME:
		if (val.vol < 0 || val.vol > 255) {
			ret = -EINVAL;
			break;
		}
		data->adc_volume = val.vol;
		if (!data->adc_muted) {
			upd(dev, ADC_VOL_CTL, 0xffU, data->adc_volume);
		}
		break;
	case AUDIO_PROPERTY_INPUT_MUTE:
		data->adc_muted = val.mute;
		upd(dev, ADC_VOL_CTL, 0xffU, val.mute ? 0U : data->adc_volume);
		break;
	default:
		ret = -EINVAL;
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

static int analog_apply_properties(const struct device *dev)
{
	ARG_UNUSED(dev);

	/* every property is written as it is set */
	return 0;
}

static int analog_route_input(const struct device *dev, audio_channel_t channel, uint32_t input)
{
	struct analog_data *data = dev->data;

	ARG_UNUSED(channel);
	if (input > ADC_SRC_LINEIN) {
		return -EINVAL;
	}
	k_mutex_lock(&data->lock, K_FOREVER);
	data->source = input;
	apply_source(dev);
	k_mutex_unlock(&data->lock);

	return 0;
}

static int analog_init(const struct device *dev)
{
	const struct analog_config *cfg = dev->config;
	struct analog_data *data = dev->data;

	k_mutex_init(&data->lock);
	if (cfg->pa.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->pa)) {
			return -ENODEV;
		}
		gpio_pin_configure_dt(&cfg->pa, GPIO_OUTPUT_INACTIVE);
	}

	data->dac_volume[0] = cfg->dac_volume;
	data->dac_volume[1] = cfg->dac_volume;
	data->adc_volume = cfg->adc_volume;

	/* manual ramp control and no pop when the headphone driver powers up */
	upd(dev, RAMP, RMC_EN | RK_OPT_EN, RMC_EN | RK_OPT_EN);
	upd(dev, DAC_VOL_CTL, DAC_VOL_SEL, DAC_VOL_SEL);
	upd(dev, DAC_DPC, DAC_DVOL_MASK, 0U);
	upd(dev, HP2, HP_GAIN_MASK, (uint32_t)(7U - cfg->hp_gain) << HP_GAIN_SHIFT);
	apply_dac_volume(dev);
	upd(dev, ADC_VOL_CTL, 0xffU, cfg->adc_volume);
	upd(dev, ADC1_AN, 0xfU << ADC1_PGA_GAIN_SHIFT,
	    (uint32_t)cfg->adc_gain << ADC1_PGA_GAIN_SHIFT);
	upd(dev, ADC1_AN, (3U << LINEINL_GAIN_SHIFT) | (3U << FML_GAIN_SHIFT),
	    (3U << LINEINL_GAIN_SHIFT) | (3U << FML_GAIN_SHIFT));
	data->source = cfg->adc_source;
	apply_source(dev);

	return 0;
}

static const struct audio_codec_api analog_api = {
	.configure = analog_configure,
	.start_output = analog_start_output,
	.stop_output = analog_stop_output,
	.set_property = analog_set_property,
	.apply_properties = analog_apply_properties,
	.route_input = analog_route_input,
};

#define ADC_SRC(n) \
	(DT_INST_ENUM_IDX(n, adc_source) == 0 ? ADC_SRC_MIC : \
	 DT_INST_ENUM_IDX(n, adc_source) == 1 ? ADC_SRC_FMIN : \
	 DT_INST_ENUM_IDX(n, adc_source) == 2 ? ADC_SRC_LINEIN : ADC_SRC_NONE)

#define ANALOG_INIT(n)								\
	static const struct analog_config analog_cfg_##n = {			\
		.base = DT_REG_ADDR(DT_INST_PARENT(n)),				\
		.pa = GPIO_DT_SPEC_INST_GET_OR(n, pa_gpios, {0}),		\
		.pa_delay_ms = DT_INST_PROP(n, pa_delay_ms),			\
		.hp_gain = DT_INST_PROP(n, hpout_gain),				\
		.dac_volume = DT_INST_PROP(n, dac_volume),			\
		.adc_volume = DT_INST_PROP(n, adc_volume),			\
		.adc_gain = DT_INST_PROP(n, adc_gain),				\
		.adc_source = ADC_SRC(n),					\
	};									\
	static struct analog_data analog_data_##n;				\
	DEVICE_DT_INST_DEFINE(n, analog_init, NULL, &analog_data_##n, &analog_cfg_##n,	\
			      POST_KERNEL, CONFIG_AUDIO_CODEC_INIT_PRIORITY, &analog_api);

DT_INST_FOREACH_STATUS_OKAY(ANALOG_INIT)
