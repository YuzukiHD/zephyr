/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT allwinner_sunxi_wdt

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(wdt_sunxi, CONFIG_WDT_LOG_LEVEL);

#define WDT_IRQ_EN	0x00
#define WDT_STATUS	0x04
#define WDT_CTL		0x10
#define WDT_CFG		0x14
#define WDT_MODE	0x18
#define WDT_OUT_CFG	0x1c

#define WDT_MODE_ENABLE	BIT(0)
#define WDT_TIMEOUT_SHIFT	4
#define WDT_TIMEOUT_MASK	GENMASK(7, 4)
#define WDT_CFG_RESET	BIT(0)
#define WDT_KEY		0x16aa0000U
#define WDT_CTL_RESTART	BIT(0)
#define WDT_CTL_KEY	(0x0a57U << 1)
#define WDT_OUT_RESET_PULSE	0x3fU

struct sunxi_wdt_config {
	uintptr_t base;
};

struct sunxi_wdt_data {
	uint8_t timeout_code;
	uint32_t timeout_ms;
	bool timeout_installed;
	bool setup;
};

struct sunxi_wdt_timeout {
	uint32_t ms;
	uint8_t code;
};

static const struct sunxi_wdt_timeout sunxi_wdt_timeouts[] = {
	{ 1000U, 0x1U },
	{ 2000U, 0x2U },
	{ 3000U, 0x3U },
	{ 4000U, 0x4U },
	{ 5000U, 0x5U },
	{ 6000U, 0x6U },
	{ 8000U, 0x7U },
	{ 10000U, 0x8U },
	{ 12000U, 0x9U },
	{ 14000U, 0xaU },
	{ 16000U, 0xbU },
};

static inline uint32_t wdt_read(const struct sunxi_wdt_config *cfg,
				uint32_t offset)
{
	return sys_read32(cfg->base + offset);
}

static inline void wdt_write(const struct sunxi_wdt_config *cfg,
				 uint32_t offset, uint32_t value)
{
	sys_write32(value, cfg->base + offset);
}

static void sunxi_wdt_disable_hw(const struct sunxi_wdt_config *cfg)
{
	uint32_t mode = wdt_read(cfg, WDT_MODE);

	mode &= ~WDT_MODE_ENABLE;
	wdt_write(cfg, WDT_MODE, mode | WDT_KEY);
	wdt_write(cfg, WDT_IRQ_EN, 0U);
	wdt_write(cfg, WDT_STATUS, 1U);
	wdt_write(cfg, WDT_CFG, WDT_KEY);
}

static int sunxi_wdt_setup(const struct device *dev, uint8_t options)
{
	const struct sunxi_wdt_config *cfg = dev->config;
	struct sunxi_wdt_data *data = dev->data;
	uint32_t mode;

	if (!data->timeout_installed) {
		return -EINVAL;
	}
	if (data->setup) {
		return -EBUSY;
	}
	/*
	 * The counter cannot be stopped in sleep or while the CPU is halted by
	 * a debugger, so WDT_OPT_PAUSE_* are accepted and not honoured.
	 */
	ARG_UNUSED(options);

	/* F101 watchdog has a SoC-reset response and no callback stage. */
	sunxi_wdt_disable_hw(cfg);
	wdt_write(cfg, WDT_OUT_CFG, WDT_OUT_RESET_PULSE);
	wdt_write(cfg, WDT_CFG, WDT_KEY | WDT_CFG_RESET);
	mode = WDT_KEY | ((uint32_t)data->timeout_code << WDT_TIMEOUT_SHIFT) |
		WDT_MODE_ENABLE;
	wdt_write(cfg, WDT_MODE, mode);
	wdt_write(cfg, WDT_CTL, WDT_CTL_KEY | WDT_CTL_RESTART);
	data->setup = true;

	return 0;
}

static int sunxi_wdt_disable(const struct device *dev)
{
	const struct sunxi_wdt_config *cfg = dev->config;
	struct sunxi_wdt_data *data = dev->data;

	if ((wdt_read(cfg, WDT_MODE) & WDT_MODE_ENABLE) == 0U) {
		return -EFAULT;
	}

	sunxi_wdt_disable_hw(cfg);
	data->setup = false;
	data->timeout_installed = false;
	return 0;
}

static int sunxi_wdt_install_timeout(const struct device *dev,
					     const struct wdt_timeout_cfg *timeout)
{
	struct sunxi_wdt_data *data = dev->data;
	const struct sunxi_wdt_timeout *selected = NULL;

	if (timeout == NULL) {
		return -EINVAL;
	}
	if (data->setup) {
		return -EBUSY;
	}
	if (data->timeout_installed) {
		return -ENOMEM;
	}
	if (timeout->window.min != 0U || timeout->window.max == 0U) {
		return -EINVAL;
	}
	if (timeout->callback != NULL) {
		return -ENOTSUP;
	}
	if ((timeout->flags & WDT_FLAG_RESET_MASK) != WDT_FLAG_RESET_SOC) {
		return -ENOTSUP;
	}

	/* Select the first hardware period not shorter than the requested one. */
	for (size_t i = 0; i < ARRAY_SIZE(sunxi_wdt_timeouts); i++) {
		if (timeout->window.max <= sunxi_wdt_timeouts[i].ms) {
			selected = &sunxi_wdt_timeouts[i];
			break;
		}
	}
	if (selected == NULL) {
		return -EINVAL;
	}

	data->timeout_code = selected->code;
	data->timeout_ms = selected->ms;
	data->timeout_installed = true;
	return 0;
}

static int sunxi_wdt_feed(const struct device *dev, int channel_id)
{
	const struct sunxi_wdt_config *cfg = dev->config;
	const struct sunxi_wdt_data *data = dev->data;

	if (channel_id != 0) {
		return -EINVAL;
	}
	if (!data->setup ||
	    (wdt_read(cfg, WDT_MODE) & WDT_MODE_ENABLE) == 0U) {
		return -EINVAL;
	}

	wdt_write(cfg, WDT_CTL, WDT_CTL_KEY | WDT_CTL_RESTART);
	return 0;
}

static int sunxi_wdt_init(const struct device *dev)
{
	const struct sunxi_wdt_config *cfg = dev->config;

	if (IS_ENABLED(CONFIG_WDT_DISABLE_AT_BOOT)) {
		sunxi_wdt_disable_hw(cfg);
	}

	return 0;
}

static DEVICE_API(wdt, sunxi_wdt_api) = {
	.setup = sunxi_wdt_setup,
	.disable = sunxi_wdt_disable,
	.install_timeout = sunxi_wdt_install_timeout,
	.feed = sunxi_wdt_feed,
};

#define SUNXI_WDT_INIT(inst) \
	static const struct sunxi_wdt_config sunxi_wdt_cfg_##inst = { \
		.base = DT_INST_REG_ADDR(inst), \
	}; \
	static struct sunxi_wdt_data sunxi_wdt_data_##inst; \
	DEVICE_DT_INST_DEFINE(inst, sunxi_wdt_init, NULL, \
		&sunxi_wdt_data_##inst, &sunxi_wdt_cfg_##inst, POST_KERNEL, \
		CONFIG_WDT_SUNXI_INIT_PRIORITY, &sunxi_wdt_api);

DT_INST_FOREACH_STATUS_OKAY(SUNXI_WDT_INIT)
