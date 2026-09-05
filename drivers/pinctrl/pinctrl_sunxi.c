/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT allwinner_sunxi_pinctrl

#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/pinctrl/pinctrl_sunxi.h>
#include <zephyr/dt-bindings/pinctrl/allwinner-pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

/*
 * Generic Allwinner sunxi PIO pinmux driver.
 *
 * The register layout is described by struct sunxi_pio_hw_info, selected
 * through the 'allwinner,pio-hw-type' property of the pinctrl node, so no
 * chip-specific code is required to support a new PIO.
 */

#define PINCTRL_BASE_ADDR	DT_INST_REG_ADDR(0)
#define PINCTRL_HW_INFO		sunxi_pio_hw_info_get(DT_INST_PROP(0, allwinner_pio_hw_type))

static int pinctrl_sunxi_set_pin(const struct sunxi_pio_hw_info *hw,
				 uint32_t base, uint32_t pinmux, uint8_t pull)
{
	uint32_t pin = ALLWINNER_PINMUX_PIN(pinmux);
	uint32_t muxsel = ALLWINNER_PINMUX_MUXSEL(pinmux);
	uint32_t bank = pin / SUNXI_PIO_PINS_PER_BANK;
	uint32_t num = pin % SUNXI_PIO_PINS_PER_BANK;
	uint32_t bank_base = base + bank * hw->bank_mem_size;
	uint32_t reg;

	if (muxsel > 0xf) {
		return -EINVAL;
	}

	/* muxsel */
	reg = bank_base + hw->mux_regs_offset +
	      (num / hw->mux_pins_per_reg) * 4;
	reg = (sys_read32(reg) &
	       ~(0xf << (num % hw->mux_pins_per_reg) * hw->mux_pins_bits)) |
	      (muxsel << (num % hw->mux_pins_per_reg) * hw->mux_pins_bits);
	sys_write32(reg, bank_base + hw->mux_regs_offset +
		    (num / hw->mux_pins_per_reg) * 4);

	/* pull */
	reg = bank_base + hw->pull_regs_offset +
	      (num / hw->pull_pins_per_reg) * 4;
	reg = (sys_read32(reg) &
	       ~(0x3 << (num % hw->pull_pins_per_reg) * hw->pull_pins_bits)) |
	      (pull << (num % hw->pull_pins_per_reg) * hw->pull_pins_bits);
	sys_write32(reg, bank_base + hw->pull_regs_offset +
		    (num / hw->pull_pins_per_reg) * 4);

	return 0;
}

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt,
			   uintptr_t reg)
{
	const struct sunxi_pio_hw_info *hw = PINCTRL_HW_INFO;
	int ret;
	int i;

	ARG_UNUSED(reg);

	if (hw == NULL) {
		return -EINVAL;
	}

	for (i = 0; i < pin_cnt; i++) {
		ret = pinctrl_sunxi_set_pin(hw, PINCTRL_BASE_ADDR,
					    pins[i].pinmux, pins[i].pull);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}