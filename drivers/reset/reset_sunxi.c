/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT allwinner_sunxi_rctl

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/dt-bindings/clock/allwinner-ccu.h>

/*
 * Generic Allwinner sunxi CCU reset line driver.
 *
 * The CCU base addresses come from the 'ccu-bases' property of the parent
 * CCU node, indexed by the base index field of the reset id. A reset id
 * encodes the CCU base index, the register offset within that CCU and the
 * reset bit position (see allwinner-ccu.h).
 */

struct ccu_rctl_config {
	const uint32_t *bases;
	uint32_t n_bases;
};

static int ccu_rctl_status(const struct device *dev, uint32_t id,
			   uint8_t *status)
{
	const struct ccu_rctl_config *config = dev->config;
	uint32_t base_idx = ALLWINNER_CCU_ID_BASE(id);

	if (base_idx >= config->n_bases) {
		return -EINVAL;
	}

	/* Allwinner CCU reset lines: bit set = released, bit cleared = held */
	*status = !sys_test_bit(config->bases[base_idx] + ALLWINNER_CCU_ID_REG(id),
				ALLWINNER_CCU_ID_BIT(id));

	return 0;
}

static int ccu_rctl_line_assert(const struct device *dev, uint32_t id)
{
	const struct ccu_rctl_config *config = dev->config;
	uint32_t base_idx = ALLWINNER_CCU_ID_BASE(id);

	if (base_idx >= config->n_bases) {
		return -EINVAL;
	}

	/* Hold in reset: clear the reset bit */
	sys_clear_bit(config->bases[base_idx] + ALLWINNER_CCU_ID_REG(id),
		      ALLWINNER_CCU_ID_BIT(id));

	return 0;
}

static int ccu_rctl_line_deassert(const struct device *dev, uint32_t id)
{
	const struct ccu_rctl_config *config = dev->config;
	uint32_t base_idx = ALLWINNER_CCU_ID_BASE(id);

	if (base_idx >= config->n_bases) {
		return -EINVAL;
	}

	/* Release from reset: set the reset bit */
	sys_set_bit(config->bases[base_idx] + ALLWINNER_CCU_ID_REG(id),
		    ALLWINNER_CCU_ID_BIT(id));

	return 0;
}

static int ccu_rctl_line_toggle(const struct device *dev, uint32_t id)
{
	(void)ccu_rctl_line_assert(dev, id);
	(void)ccu_rctl_line_deassert(dev, id);

	return 0;
}

static DEVICE_API(reset, ccu_rctl_driver_api) = {
	.status = ccu_rctl_status,
	.line_assert = ccu_rctl_line_assert,
	.line_deassert = ccu_rctl_line_deassert,
	.line_toggle = ccu_rctl_line_toggle,
};

#define CCU_RCTL_INIT(n)							\
	static const uint32_t ccu_bases_##n[] =				\
		DT_PROP(DT_INST_PARENT(n), ccu_bases);			\
	static const struct ccu_rctl_config ccu_rctl_config_##n = {		\
		.bases = ccu_bases_##n,					\
		.n_bases = DT_PROP_LEN(DT_INST_PARENT(n), ccu_bases),	\
	};									\
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, &ccu_rctl_config_##n,	\
			      PRE_KERNEL_1, CONFIG_RESET_INIT_PRIORITY,		\
			      &ccu_rctl_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CCU_RCTL_INIT)