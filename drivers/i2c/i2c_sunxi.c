/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT allwinner_sunxi_twi

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define TWI_DATA	0x08
#define TWI_CTL		0x0c
#define TWI_STATUS	0x10
#define TWI_CLK		0x14
#define TWI_SRST	0x18
#define TWI_EFT		0x1c

#define CTL_ACK		BIT(2)
#define CTL_INTFLG	BIT(3)
#define CTL_STP		BIT(4)
#define CTL_STA		BIT(5)
#define CTL_BUSEN	BIT(6)

#define STATUS_START	0x08
#define STATUS_RESTART	0x10
#define STATUS_ADDR_W	0x18
#define STATUS_ADDR_W_NACK 0x20
#define STATUS_DATA_W	0x28
#define STATUS_DATA_W_NACK 0x30
#define STATUS_ARB_LOST	0x38
#define STATUS_ADDR_R	0x40
#define STATUS_ADDR_R_NACK 0x48
#define STATUS_DATA_R_ACK 0x50
#define STATUS_DATA_R_NACK 0x58
#define TWI_TIMEOUT_US	100000U

struct sunxi_twi_config {
	const struct pinctrl_dev_config *pcfg;
	const struct device *clock_dev;
	uint32_t base;
	uint32_t clock_id;
	struct reset_dt_spec reset;
	uint32_t bitrate;
};

struct sunxi_twi_data {
	struct k_mutex lock;
};

static inline uint32_t twi_read(const struct sunxi_twi_config *cfg,
				uint32_t offset)
{
	return sys_read32(cfg->base + offset);
}

static inline void twi_write(const struct sunxi_twi_config *cfg,
				 uint32_t offset, uint32_t value)
{
	sys_write32(value, cfg->base + offset);
}

static int twi_status_error(uint32_t status)
{
	switch (status) {
	case STATUS_ADDR_W_NACK:
	case STATUS_DATA_W_NACK:
	case STATUS_ADDR_R_NACK:
	case STATUS_DATA_R_NACK:
		return -EIO;
	case STATUS_ARB_LOST:
		return -EAGAIN;
	default:
		return -EIO;
	}
}

static int twi_wait_flag(const struct sunxi_twi_config *cfg,
			 uint32_t expected_status)
{
	for (uint32_t elapsed = 0; elapsed < TWI_TIMEOUT_US; elapsed++) {
		if ((twi_read(cfg, TWI_CTL) & CTL_INTFLG) != 0U) {
			uint32_t status = twi_read(cfg, TWI_STATUS) & 0xffU;

			return (status == expected_status) ? 0 : twi_status_error(status);
		}
		k_busy_wait(1);
	}

	return -ETIMEDOUT;
}

static int twi_start(const struct sunxi_twi_config *cfg, bool repeated)
{
	uint32_t ctl = twi_read(cfg, TWI_CTL) | CTL_INTFLG | CTL_STA;

	/* INTFLG is write-one-to-clear; STA starts START or RESTART. */
	twi_write(cfg, TWI_CTL, ctl);

	return twi_wait_flag(cfg, repeated ? STATUS_RESTART : STATUS_START);
}

static int twi_send_address(const struct sunxi_twi_config *cfg,
				uint16_t addr, bool read)
{
	uint32_t ctl;

	twi_write(cfg, TWI_DATA, ((uint32_t)addr << 1) | (read ? 1U : 0U));
	ctl = twi_read(cfg, TWI_CTL) | CTL_INTFLG;
	twi_write(cfg, TWI_CTL, ctl);

	return twi_wait_flag(cfg, read ? STATUS_ADDR_R : STATUS_ADDR_W);
}

static int twi_write_msg(const struct sunxi_twi_config *cfg,
				const struct i2c_msg *msg)
{
	for (uint32_t i = 0; i < msg->len; i++) {
		uint32_t ctl;
		int ret;

		twi_write(cfg, TWI_DATA, msg->buf[i]);
		ctl = twi_read(cfg, TWI_CTL) | CTL_INTFLG;
		twi_write(cfg, TWI_CTL, ctl);
		ret = twi_wait_flag(cfg, STATUS_DATA_W);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static int twi_read_msg(const struct sunxi_twi_config *cfg,
				struct i2c_msg *msg)
{
	for (uint32_t i = 0; i < msg->len; i++) {
		uint32_t ctl = twi_read(cfg, TWI_CTL);
		int ret;

		if (i + 1U < msg->len) {
			ctl |= CTL_ACK;
		} else {
			ctl &= ~CTL_ACK;
		}
		ctl |= CTL_INTFLG;
		twi_write(cfg, TWI_CTL, ctl);
		ret = twi_wait_flag(cfg, (i + 1U < msg->len) ?
					STATUS_DATA_R_ACK : STATUS_DATA_R_NACK);
		if (ret != 0) {
			return ret;
		}
		msg->buf[i] = (uint8_t)twi_read(cfg, TWI_DATA);
	}

	return 0;
}

static int twi_stop(const struct sunxi_twi_config *cfg)
{
	uint32_t ctl = twi_read(cfg, TWI_CTL) | CTL_STP | CTL_INTFLG;

	twi_write(cfg, TWI_CTL, ctl);
	for (uint32_t elapsed = 0; elapsed < TWI_TIMEOUT_US; elapsed++) {
		if ((twi_read(cfg, TWI_CTL) & CTL_STP) == 0U) {
			return 0;
		}
		k_busy_wait(1);
	}

	return -ETIMEDOUT;
}

static int sunxi_twi_configure(const struct device *dev, uint32_t dev_config)
{
	const struct sunxi_twi_config *cfg = dev->config;
	uint32_t clk;

	if ((dev_config & I2C_MODE_CONTROLLER) == 0U ||
	    (dev_config & I2C_ADDR_10_BITS) != 0U) {
		return -ENOTSUP;
	}

	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		clk = 0x59U; /* 100 kHz at the 24 MHz TWI source clock */
		break;
	case I2C_SPEED_FAST:
		clk = 0x28U; /* 400 kHz at the 24 MHz TWI source clock */
		break;
	default:
		return -ENOTSUP;
	}

	twi_write(cfg, TWI_EFT, 0U);
	twi_write(cfg, TWI_SRST, 1U);
	twi_write(cfg, TWI_CLK, clk);
	twi_write(cfg, TWI_CTL, CTL_BUSEN);

	return 0;
}

static int sunxi_twi_transfer(const struct device *dev, struct i2c_msg *msgs,
				      uint8_t num_msgs, uint16_t addr)
{
	const struct sunxi_twi_config *cfg = dev->config;
	struct sunxi_twi_data *data = dev->data;
	int ret = 0;

	if (addr > 0x7fU || msgs == NULL || num_msgs == 0U) {
		return -EINVAL;
	}
	for (uint8_t i = 0; i < num_msgs; i++) {
		if ((msgs[i].flags & I2C_MSG_ADDR_10_BITS) != 0U) {
			return -ENOTSUP;
		}
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	for (uint8_t i = 0; i < num_msgs; i++) {
		bool start = i == 0U;

		/* Consecutive write fragments form one I2C transaction. */
		if (i != 0U &&
		    ((msgs[i].flags & I2C_MSG_RESTART) != 0U ||
		     (msgs[i - 1U].flags & I2C_MSG_READ) != 0U ||
		     (msgs[i].flags & I2C_MSG_READ) != 0U)) {
			start = true;
		}

		if (start) {
			ret = twi_start(cfg, i != 0U);
			if (ret != 0) {
				break;
			}
			ret = twi_send_address(cfg, addr,
					(msgs[i].flags & I2C_MSG_READ) != 0U);
		}
		if (ret == 0) {
			ret = (msgs[i].flags & I2C_MSG_READ) != 0U ?
				twi_read_msg(cfg, &msgs[i]) : twi_write_msg(cfg, &msgs[i]);
		}
		if (ret != 0) {
			break;
		}
		if ((msgs[i].flags & I2C_MSG_STOP) != 0U || i + 1U == num_msgs) {
			ret = twi_stop(cfg);
			if (ret != 0) {
				break;
			}
		}
	}
	if (ret != 0) {
		(void)twi_stop(cfg);
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

static int sunxi_twi_init(const struct device *dev)
{
	const struct sunxi_twi_config *cfg = dev->config;
	struct sunxi_twi_data *data = dev->data;
	clock_control_subsys_t clock_id =
		(clock_control_subsys_t)(uintptr_t)cfg->clock_id;
	uint32_t speed;
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

	switch (cfg->bitrate) {
	case 100000U:
	case 200000U:
	case 400000U:
		speed = (cfg->bitrate == 400000U) ? I2C_SPEED_FAST :
			I2C_SPEED_STANDARD;
		break;
	default:
		return -ENOTSUP;
	}

	ret = sunxi_twi_configure(dev, I2C_MODE_CONTROLLER |
					 I2C_SPEED_SET(speed));
	if (ret == 0 && cfg->bitrate == 200000U) {
		/* F101 has no Zephyr speed enum for 200 kHz, but DT may request it. */
		twi_write(cfg, TWI_CLK, 0x58U);
	}

	return ret;
}

static DEVICE_API(i2c, sunxi_twi_api) = {
	.configure = sunxi_twi_configure,
	.transfer = sunxi_twi_transfer,
#ifdef CONFIG_I2C_RTIO
	.iodev_submit = i2c_iodev_submit_fallback,
#endif
};

#define SUNXI_TWI_INIT(inst) \
	PINCTRL_DT_INST_DEFINE(inst); \
	static const struct sunxi_twi_config sunxi_twi_cfg_##inst = { \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst), \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(inst)), \
		.base = DT_INST_REG_ADDR(inst), \
		.clock_id = DT_INST_CLOCKS_CELL(inst, clkid), \
		.reset = RESET_DT_SPEC_INST_GET(inst), \
		.bitrate = DT_INST_PROP(inst, clock_frequency), \
	}; \
	static struct sunxi_twi_data sunxi_twi_data_##inst; \
	DEVICE_DT_INST_DEFINE(inst, sunxi_twi_init, NULL, \
		&sunxi_twi_data_##inst, &sunxi_twi_cfg_##inst, \
		POST_KERNEL, CONFIG_I2C_SUNXI_INIT_PRIORITY, &sunxi_twi_api);

DT_INST_FOREACH_STATUS_OKAY(SUNXI_TWI_INIT)
