/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_PINCTRL_PINCTRL_SUNXI_H_
#define ZEPHYR_INCLUDE_DRIVERS_PINCTRL_PINCTRL_SUNXI_H_

#include <zephyr/dt-bindings/pinctrl/allwinner-pinctrl.h>
#include <zephyr/types.h>

/*
 * Generic description of the Allwinner PIO register layout.
 *
 * The sunxi pinctrl and gpio drivers share this structure to derive every
 * register access from a small set of layout parameters, so that a new
 * chip only needs to describe its PIO block instead of touching driver
 * code. See drivers/pinctrl/pinctrl_sunxi.h in the SPL for the original
 * design.
 *
 * A PIO controller has one block of registers per bank (CFG/DAT/DRV/PULL,
 * bank_mem_size bytes each) followed by a banked interrupt block. The
 * "pins per register" / "bits per pin" pairs describe the packing of pin
 * fields inside a 32-bit register, e.g. the CFG register packs 8 pins
 * with 4 bits each.
 */
struct sunxi_pio_hw_info {
	uint32_t bank_mem_size;

	/* CFG (muxsel) register block */
	uint32_t mux_regs_offset;
	uint32_t mux_pins_per_reg;
	uint32_t mux_pins_bits;

	/* DAT (data) register block */
	uint32_t data_regs_offset;
	uint32_t data_pins_per_reg;
	uint32_t data_pins_bits;

	/* PULL register block */
	uint32_t pull_regs_offset;
	uint32_t pull_pins_per_reg;
	uint32_t pull_pins_bits;

	/* DRV (drive level) register block */
	uint32_t drv_regs_offset;
	uint32_t drv_pins_per_reg;
	uint32_t drv_pins_bits;

	/* Per-bank interrupt block */
	uint32_t irq_regs_offset;
	uint32_t irq_bank_stride;
	uint32_t irq_ctl_offset;
	uint32_t irq_status_offset;
	uint32_t irq_muxsel;
};

/* PIO banks have 32 pins each on every sunxi chip */
#define SUNXI_PIO_PINS_PER_BANK		32

/*
 * Type 0: the compact PIO layout used by the F1xx family.
 *
 * Banks are 0x30 bytes apart, the CFG/DAT/DRV/PULL registers sit at the
 * bank base and the interrupt block follows all banks at 0x200 with a
 * 0x20 stride per bank.
 */
static const struct sunxi_pio_hw_info sunxi_pio_hw_type0_info = {
	.bank_mem_size		= 0x30,
	.mux_regs_offset	= 0x00,
	.mux_pins_per_reg	= 8,
	.mux_pins_bits		= 4,
	.data_regs_offset	= 0x10,
	.data_pins_per_reg	= 32,
	.data_pins_bits		= 1,
	.pull_regs_offset	= 0x24,
	.pull_pins_per_reg	= 16,
	.pull_pins_bits		= 2,
	.drv_regs_offset	= 0x14,
	.drv_pins_per_reg	= 8,
	.drv_pins_bits		= 4,
	.irq_regs_offset	= 0x200,
	.irq_bank_stride	= 0x20,
	.irq_ctl_offset		= 0x10,
	.irq_status_offset	= 0x14,
	.irq_muxsel		= 0x0e,
};

static inline const struct sunxi_pio_hw_info *
sunxi_pio_hw_info_get(uint32_t hw_type)
{
	switch (hw_type) {
	case ALLWINNER_PIO_HW_TYPE0:
		return &sunxi_pio_hw_type0_info;
	default:
		return NULL;
	}
}

#endif /* ZEPHYR_INCLUDE_DRIVERS_PINCTRL_PINCTRL_SUNXI_H_ */