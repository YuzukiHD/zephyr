/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT allwinner_sunxi_gpio

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/drivers/pinctrl/pinctrl_sunxi.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

/*
 * Generic Allwinner sunxi PIO GPIO driver.
 *
 * The register layout is described by struct sunxi_pio_hw_info, selected
 * through the 'allwinner,pio-hw-type' property, so no chip-specific code
 * is required to support a new PIO. Each DT gpio node covers one PIO bank
 * identified by the 'allwinner,bank' property.
 */

#define GPIO_PINS_PER_BANK	SUNXI_PIO_PINS_PER_BANK

/* Allwinner PIO muxsel values for plain GPIO */
#define PIO_MUXSEL_INPUT	0
#define PIO_MUXSEL_OUTPUT	1

/* IRQ configuration values */
#define PIO_IRQ_EDGE_RISING	0x00
#define PIO_IRQ_EDGE_FALLING	0x01
#define PIO_IRQ_LEVEL_HIGH	0x02
#define PIO_IRQ_LEVEL_LOW	0x03
#define PIO_IRQ_EDGE_BOTH	0x04

struct gpio_sunxi_config {
	struct gpio_driver_config common;
	uint32_t hw_type;
	uint32_t base;
	uint32_t bank;
};

struct gpio_sunxi_data {
	struct gpio_driver_data common;
	const struct sunxi_pio_hw_info *hw;
	sys_slist_t cb;
	struct k_spinlock lock;
	uint8_t prev_mux[GPIO_PINS_PER_BANK];
};

static uint32_t gpio_sunxi_bank_base(const struct gpio_sunxi_config *config,
				     const struct sunxi_pio_hw_info *hw)
{
	ARG_UNUSED(hw);

	/* Each GPIO DT instance already points at its bank's register block. */
	return config->base;
}

static uint32_t gpio_sunxi_irq_base(const struct gpio_sunxi_config *config,
				    const struct sunxi_pio_hw_info *hw)
{
	/* The DT instance base is bank-relative, while the interrupt block is
	 * laid out from the common PIO base with a different bank stride.
	 */
	return config->base - config->bank * hw->bank_mem_size +
	       hw->irq_regs_offset + config->bank * hw->irq_bank_stride;
}

static int gpio_sunxi_pin_configure(const struct device *dev,
					gpio_pin_t pin, gpio_flags_t flags)
{
	const struct gpio_sunxi_config *config = dev->config;
	struct gpio_sunxi_data *data = dev->data;
	const struct sunxi_pio_hw_info *hw = data->hw;
	uint32_t bank_base = gpio_sunxi_bank_base(config, hw);
	uint32_t muxsel;
	uint32_t pull = 0;
	uint32_t reg;

	if ((flags & GPIO_SINGLE_ENDED) != 0) {
		return -ENOTSUP;
	}

	if ((flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)) != 0) {
		pull = (flags & GPIO_PULL_UP) ? 1 : 2;
	}

	if ((flags & GPIO_OUTPUT) != 0) {
		muxsel = PIO_MUXSEL_OUTPUT;
		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0) {
			sys_set_bit(bank_base + hw->data_regs_offset, pin);
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0) {
			sys_clear_bit(bank_base + hw->data_regs_offset, pin);
		}
	} else {
		muxsel = PIO_MUXSEL_INPUT;
	}

	reg = bank_base + hw->mux_regs_offset + (pin / hw->mux_pins_per_reg) * 4;
	reg = (sys_read32(reg) &
	       ~(0xf << (pin % hw->mux_pins_per_reg) * hw->mux_pins_bits)) |
	      (muxsel << (pin % hw->mux_pins_per_reg) * hw->mux_pins_bits);
	sys_write32(reg, bank_base + hw->mux_regs_offset +
		    (pin / hw->mux_pins_per_reg) * 4);

	reg = bank_base + hw->pull_regs_offset + (pin / hw->pull_pins_per_reg) * 4;
	reg = (sys_read32(reg) &
	       ~(0x3 << (pin % hw->pull_pins_per_reg) * hw->pull_pins_bits)) |
	      (pull << (pin % hw->pull_pins_per_reg) * hw->pull_pins_bits);
	sys_write32(reg, bank_base + hw->pull_regs_offset +
		    (pin / hw->pull_pins_per_reg) * 4);

	return 0;
}

static int gpio_sunxi_port_get_raw(const struct device *dev,
				       gpio_port_value_t *value)
{
	const struct gpio_sunxi_config *config = dev->config;
	struct gpio_sunxi_data *data = dev->data;

	*value = sys_read32(gpio_sunxi_bank_base(config, data->hw) +
			    data->hw->data_regs_offset);

	return 0;
}

static int gpio_sunxi_port_set_masked_raw(const struct device *dev,
					      gpio_port_pins_t mask,
					      gpio_port_value_t value)
{
	const struct gpio_sunxi_config *config = dev->config;
	struct gpio_sunxi_data *data = dev->data;
	uint32_t reg;

	reg = sys_read32(gpio_sunxi_bank_base(config, data->hw) +
			 data->hw->data_regs_offset);
	reg = (reg & ~mask) | (mask & value);
	sys_write32(reg, gpio_sunxi_bank_base(config, data->hw) +
		    data->hw->data_regs_offset);

	return 0;
}

static int gpio_sunxi_port_set_bits_raw(const struct device *dev,
					    gpio_port_pins_t mask)
{
	const struct gpio_sunxi_config *config = dev->config;
	struct gpio_sunxi_data *data = dev->data;

	sys_set_bits(gpio_sunxi_bank_base(config, data->hw) +
		     data->hw->data_regs_offset, mask);

	return 0;
}

static int gpio_sunxi_port_clear_bits_raw(const struct device *dev,
					      gpio_port_pins_t mask)
{
	const struct gpio_sunxi_config *config = dev->config;
	struct gpio_sunxi_data *data = dev->data;

	sys_clear_bits(gpio_sunxi_bank_base(config, data->hw) +
		       data->hw->data_regs_offset, mask);

	return 0;
}

static int gpio_sunxi_port_toggle_bits(const struct device *dev,
					   gpio_port_pins_t mask)
{
	const struct gpio_sunxi_config *config = dev->config;
	struct gpio_sunxi_data *data = dev->data;
	uint32_t reg;

	reg = sys_read32(gpio_sunxi_bank_base(config, data->hw) +
			 data->hw->data_regs_offset);
	reg ^= mask;
	sys_write32(reg, gpio_sunxi_bank_base(config, data->hw) +
		    data->hw->data_regs_offset);

	return 0;
}

static int gpio_sunxi_pin_interrupt_configure(const struct device *dev,
						  gpio_pin_t pin,
						  enum gpio_int_mode mode,
						  enum gpio_int_trig trig)
{
	const struct gpio_sunxi_config *config = dev->config;
	struct gpio_sunxi_data *data = dev->data;
	const struct sunxi_pio_hw_info *hw = data->hw;
	uint32_t irq_base = gpio_sunxi_irq_base(config, hw);
	uint32_t bank_base = gpio_sunxi_bank_base(config, hw);
	uint32_t irq_cfg;
	uint32_t reg;

	if (mode == GPIO_INT_MODE_DISABLED) {
		sys_clear_bit(irq_base + hw->irq_ctl_offset, pin);

		reg = bank_base + hw->mux_regs_offset +
		      (pin / hw->mux_pins_per_reg) * 4;
		reg = (sys_read32(reg) &
		       ~(0xf << (pin % hw->mux_pins_per_reg) * hw->mux_pins_bits)) |
		      (data->prev_mux[pin] << (pin % hw->mux_pins_per_reg) *
		       hw->mux_pins_bits);
		sys_write32(reg, bank_base + hw->mux_regs_offset +
			    (pin / hw->mux_pins_per_reg) * 4);

		return 0;
	}

	if (mode == GPIO_INT_MODE_LEVEL) {
		irq_cfg = (trig == GPIO_INT_TRIG_HIGH) ? PIO_IRQ_LEVEL_HIGH
						      : PIO_IRQ_LEVEL_LOW;
	} else {
		switch (trig) {
		case GPIO_INT_TRIG_LOW:
			irq_cfg = PIO_IRQ_EDGE_FALLING;
			break;
		case GPIO_INT_TRIG_HIGH:
			irq_cfg = PIO_IRQ_EDGE_RISING;
			break;
		default:
			irq_cfg = PIO_IRQ_EDGE_BOTH;
			break;
		}
	}

	/* Save the current pin function and switch to IRQ mux */
	reg = bank_base + hw->mux_regs_offset +
	      (pin / hw->mux_pins_per_reg) * 4;
	data->prev_mux[pin] = (sys_read32(reg) >>
			       (pin % hw->mux_pins_per_reg) * hw->mux_pins_bits) & 0xf;
	reg = (sys_read32(reg) &
	       ~(0xf << (pin % hw->mux_pins_per_reg) * hw->mux_pins_bits)) |
	      (hw->irq_muxsel << (pin % hw->mux_pins_per_reg) * hw->mux_pins_bits);
	sys_write32(reg, bank_base + hw->mux_regs_offset +
		    (pin / hw->mux_pins_per_reg) * 4);

	/* Configure the trigger mode and enable the interrupt */
	reg = irq_base + (pin / hw->mux_pins_per_reg) * 4;
	reg = (sys_read32(reg) &
	       ~(0xf << (pin % hw->mux_pins_per_reg) * hw->mux_pins_bits)) |
	      (irq_cfg << (pin % hw->mux_pins_per_reg) * hw->mux_pins_bits);
	sys_write32(reg, irq_base + (pin / hw->mux_pins_per_reg) * 4);

	sys_set_bit(irq_base + hw->irq_ctl_offset, pin);

	return 0;
}

static int gpio_sunxi_manage_callback(const struct device *dev,
					  struct gpio_callback *callback,
					  bool set)
{
	struct gpio_sunxi_data *data = dev->data;

	return gpio_manage_callback(&data->cb, callback, set);
}

static uint32_t gpio_sunxi_get_pending_int(const struct device *dev)
{
	const struct gpio_sunxi_config *config = dev->config;
	struct gpio_sunxi_data *data = dev->data;

	return sys_read32(gpio_sunxi_irq_base(config, data->hw) +
			  data->hw->irq_status_offset);
}

static void gpio_sunxi_isr(const void *arg)
{
	const struct device *dev = arg;
	const struct gpio_sunxi_config *config = dev->config;
	struct gpio_sunxi_data *data = dev->data;
	uint32_t status;

	status = sys_read32(gpio_sunxi_irq_base(config, data->hw) +
			    data->hw->irq_status_offset);
	if (status != 0) {
		sys_write32(status, gpio_sunxi_irq_base(config, data->hw) +
			    data->hw->irq_status_offset);
		gpio_fire_callbacks(&data->cb, dev, status);
	}
}

static int gpio_sunxi_init(const struct device *dev, uint32_t irqn)
{
	struct gpio_sunxi_data *data = dev->data;
	const struct gpio_sunxi_config *config = dev->config;

	data->hw = sunxi_pio_hw_info_get(config->hw_type);
	if (data->hw == NULL) {
		return -EINVAL;
	}

	/* The F101 PLIC reserves priority 0 as disabled. */
	irq_connect_dynamic(irqn, 1, gpio_sunxi_isr, dev, 0);
	irq_enable(irqn);

	return 0;
}

static DEVICE_API(gpio, gpio_sunxi_driver_api) = {
	.pin_configure = gpio_sunxi_pin_configure,
	.port_get_raw = gpio_sunxi_port_get_raw,
	.port_set_masked_raw = gpio_sunxi_port_set_masked_raw,
	.port_set_bits_raw = gpio_sunxi_port_set_bits_raw,
	.port_clear_bits_raw = gpio_sunxi_port_clear_bits_raw,
	.port_toggle_bits = gpio_sunxi_port_toggle_bits,
	.pin_interrupt_configure = gpio_sunxi_pin_interrupt_configure,
	.manage_callback = gpio_sunxi_manage_callback,
	.get_pending_int = gpio_sunxi_get_pending_int,
};

#define GPIO_SUNXI_INIT(n)							\
	static int gpio_sunxi_init_##n(const struct device *dev)			\
	{									\
		return gpio_sunxi_init(dev, DT_INST_IRQN(n));			\
	}									\
	static const struct gpio_sunxi_config gpio_sunxi_config_##n = {	\
		.common = { .port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(n) },			\
		.hw_type = DT_INST_PROP(n, allwinner_pio_hw_type),			\
		.base = DT_INST_REG_ADDR(n),					\
		.bank = DT_INST_PROP(n, allwinner_bank),					\
	};									\
	static struct gpio_sunxi_data gpio_sunxi_data_##n;		\
	DEVICE_DT_INST_DEFINE(n, gpio_sunxi_init_##n, NULL,			\
			      &gpio_sunxi_data_##n,				\
			      &gpio_sunxi_config_##n, POST_KERNEL,		\
			      CONFIG_GPIO_INIT_PRIORITY,				\
			      &gpio_sunxi_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GPIO_SUNXI_INIT)
