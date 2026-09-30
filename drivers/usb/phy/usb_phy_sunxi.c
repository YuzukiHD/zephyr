/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

#define DT_DRV_COMPAT allwinner_sunxi_usb_phy

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/usb/usb_phy_sunxi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(usb_phy_sunxi, CONFIG_USB_PHY_SUNXI_LOG_LEVEL);

/* OTG glue block */
#define OTG_ISCR		0x00
#define OTG_PHYCTRL		0x10
#define OTG_PHYSEL		0x20

#define ISCR_DPDM_PULLUP_EN	BIT(16)
#define ISCR_ID_PULLUP_EN	BIT(17)
#define ISCR_FORCE_ID_HIGH	(0x3U << 14)
#define ISCR_FORCE_VBUS_HIGH	(0x3U << 12)
/* write-one-to-clear change flags */
#define ISCR_CHANGE_FLAGS	(BIT(6) | BIT(5) | BIT(4))

#define PHYCTRL_SIDDQ		BIT(3)
#define PHYCTRL_VBUSVLDEXT	BIT(5)

#define PHYSEL_OTG		BIT(0)

struct usb_phy_sunxi_config {
	mem_addr_t otg;
	mem_addr_t hci;
	mem_addr_t sram_remap_reg;
	uint32_t sram_remap_clear;
	const struct device *clock_dev;
	clock_control_subsys_t clock_id;
	struct reset_dt_spec reset;
	struct gpio_dt_spec vbus;
};

struct usb_phy_sunxi_data {
	struct k_mutex lock;
	bool in_use;
	enum sunxi_usb_phy_role role;
};

int sunxi_usb_phy_acquire(const struct device *dev, enum sunxi_usb_phy_role role)
{
	const struct usb_phy_sunxi_config *cfg = dev->config;
	struct usb_phy_sunxi_data *data = dev->data;
	int ret = 0;

	if (role != SUNXI_USB_PHY_DEVICE) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->in_use) {
		ret = (data->role == role) ? 0 : -EBUSY;
		goto out;
	}

	/* The controller must be able to use the FIFO RAM before it is enabled */
	if (cfg->sram_remap_reg != 0) {
		sys_write32(sys_read32(cfg->sram_remap_reg) & ~cfg->sram_remap_clear,
			    cfg->sram_remap_reg);
	}

	/* No ID or VBUS comparator is wired: report the B device with VBUS valid */
	sys_write32((sys_read32(cfg->otg + OTG_ISCR) & ~ISCR_CHANGE_FLAGS) |
			    ISCR_DPDM_PULLUP_EN | ISCR_ID_PULLUP_EN | ISCR_FORCE_ID_HIGH |
			    ISCR_FORCE_VBUS_HIGH,
		    cfg->otg + OTG_ISCR);

	sys_write32((sys_read32(cfg->otg + OTG_PHYCTRL) | PHYCTRL_VBUSVLDEXT) & ~PHYCTRL_SIDDQ,
		    cfg->otg + OTG_PHYCTRL);
	sys_write32(sys_read32(cfg->otg + OTG_PHYSEL) | PHYSEL_OTG, cfg->otg + OTG_PHYSEL);

	LOG_DBG("rammap %08x iscr %08x phyctrl %08x physel %08x",
		sys_read32(cfg->sram_remap_reg), sys_read32(cfg->otg + OTG_ISCR),
		sys_read32(cfg->otg + OTG_PHYCTRL), sys_read32(cfg->otg + OTG_PHYSEL));

	data->in_use = true;
	data->role = role;
out:
	k_mutex_unlock(&data->lock);
	return ret;
}

void sunxi_usb_phy_release(const struct device *dev, enum sunxi_usb_phy_role role)
{
	const struct usb_phy_sunxi_config *cfg = dev->config;
	struct usb_phy_sunxi_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (data->in_use && data->role == role) {
		sys_write32(sys_read32(cfg->otg + OTG_PHYCTRL) | PHYCTRL_SIDDQ,
			    cfg->otg + OTG_PHYCTRL);
		data->in_use = false;
	}

	k_mutex_unlock(&data->lock);
}

static int usb_phy_sunxi_init(const struct device *dev)
{
	const struct usb_phy_sunxi_config *cfg = dev->config;
	struct usb_phy_sunxi_data *data = dev->data;
	int ret;

	k_mutex_init(&data->lock);

	if (!device_is_ready(cfg->clock_dev) || !device_is_ready(cfg->reset.dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(cfg->clock_dev, cfg->clock_id);
	if (ret) {
		LOG_ERR("bus clock failed: %d", ret);
		return ret;
	}

	ret = reset_line_deassert_dt(&cfg->reset);
	if (ret) {
		LOG_ERR("reset failed: %d", ret);
		return ret;
	}

	/* Leave the PHY powered down until a controller takes it */
	sys_write32(sys_read32(cfg->otg + OTG_PHYCTRL) | PHYCTRL_SIDDQ, cfg->otg + OTG_PHYCTRL);

	if (cfg->vbus.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->vbus) ||
		    gpio_pin_configure_dt(&cfg->vbus, GPIO_OUTPUT_INACTIVE)) {
			LOG_ERR("VBUS gpio not usable");
			return -ENODEV;
		}
	}

	LOG_INF("ready");
	return 0;
}

#define USB_PHY_SUNXI_INIT(n)                                                                      \
	static const struct usb_phy_sunxi_config usb_phy_sunxi_config_##n = {                      \
		.otg = DT_INST_REG_ADDR_BY_NAME(n, otg),                                           \
		.hci = DT_INST_REG_ADDR_BY_NAME(n, hci),                                           \
		.sram_remap_reg = DT_INST_PROP_OR(n, sram_remap_reg, 0),                           \
		.sram_remap_clear = DT_INST_PROP_OR(n, sram_remap_clear, 0),                       \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_id = (clock_control_subsys_t)(uintptr_t)DT_INST_CLOCKS_CELL(n, clkid),                 \
		.reset = RESET_DT_SPEC_INST_GET(n),                                                \
		.vbus = GPIO_DT_SPEC_INST_GET_OR(n, vbus_gpios, {0}),                              \
	};                                                                                         \
	static struct usb_phy_sunxi_data usb_phy_sunxi_data_##n;                                   \
	DEVICE_DT_INST_DEFINE(n, usb_phy_sunxi_init, NULL, &usb_phy_sunxi_data_##n,                \
			      &usb_phy_sunxi_config_##n, POST_KERNEL,                              \
			      CONFIG_USB_PHY_SUNXI_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(USB_PHY_SUNXI_INIT)
