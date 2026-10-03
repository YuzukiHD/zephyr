/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT allwinner_sunxi_cctl

#include <zephyr/drivers/clock_control.h>
#include <zephyr/dt-bindings/clock/allwinner-ccu.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <soc.h>

/*
 * Generic Allwinner sunxi CCU bus clock gate driver.
 *
 * Everything chip-specific is described by the DT node:
 *   - the CCU base addresses come from the 'ccu-bases' property of the
 *     parent CCU node, indexed by the base index field of the clock id
 *   - the 'apb-uart-reg' / 'pll-peri-reg' / 'uart-bgr-reg' properties
 *     point at the registers used to compute the APB_UART clock rate
 *
 * A clock id encodes the CCU base index, the register offset within that
 * CCU and the gate bit position (see allwinner-ccu.h).
 */

struct ccu_cctl_config {
	const uint32_t *bases;
	uint32_t n_bases;
	uint32_t apb_uart_reg;
	uint32_t pll_peri_reg;
	uint32_t uart_bgr_reg;
};

struct ccu_cctl_data {
	struct k_spinlock lock;
};

static bool ccu_cctl_is_cpu(const struct ccu_cctl_config *config, uint32_t id)
{
	ARG_UNUSED(config);

	return ALLWINNER_CCU_ID_BIT(id) == ALLWINNER_CCU_BIT_RATE &&
	       ALLWINNER_CCU_ID_REG(id) == ALLWINNER_CCU_PLL_CPU_REG;
}

static int ccu_cctl_on(const struct device *dev, clock_control_subsys_t sys)
{
	const struct ccu_cctl_config *config = dev->config;
	struct ccu_cctl_data *data = dev->data;
	uint32_t id = (uint32_t)(uintptr_t)sys;
	uint32_t base_idx = ALLWINNER_CCU_ID_BASE(id);

	if (base_idx >= config->n_bases) {
		return -EINVAL;
	}
	if (ccu_cctl_is_cpu(config, id)) {
		return 0;
	}

	K_SPINLOCK(&data->lock) {
		sys_set_bit(config->bases[base_idx] + ALLWINNER_CCU_ID_REG(id),
			    ALLWINNER_CCU_ID_BIT(id));
	}

	return 0;
}

static int ccu_cctl_off(const struct device *dev, clock_control_subsys_t sys)
{
	const struct ccu_cctl_config *config = dev->config;
	struct ccu_cctl_data *data = dev->data;
	uint32_t id = (uint32_t)(uintptr_t)sys;
	uint32_t base_idx = ALLWINNER_CCU_ID_BASE(id);

	if (base_idx >= config->n_bases) {
		return -EINVAL;
	}
	if (ccu_cctl_is_cpu(config, id)) {
		return 0;
	}

	K_SPINLOCK(&data->lock) {
		sys_clear_bit(config->bases[base_idx] + ALLWINNER_CCU_ID_REG(id),
			      ALLWINNER_CCU_ID_BIT(id));
	}

	return 0;
}

static enum clock_control_status ccu_cctl_get_status(const struct device *dev,
						     clock_control_subsys_t sys)
{
	const struct ccu_cctl_config *config = dev->config;
	uint32_t id = (uint32_t)(uintptr_t)sys;
	uint32_t base_idx = ALLWINNER_CCU_ID_BASE(id);

	if (base_idx >= config->n_bases) {
		return CLOCK_CONTROL_STATUS_UNKNOWN;
	}
	if (ccu_cctl_is_cpu(config, id)) {
		return CLOCK_CONTROL_STATUS_ON;
	}

	if (sys_test_bit(config->bases[base_idx] + ALLWINNER_CCU_ID_REG(id),
			 ALLWINNER_CCU_ID_BIT(id))) {
		return CLOCK_CONTROL_STATUS_ON;
	}

	return CLOCK_CONTROL_STATUS_OFF;
}

/*
 * APB_UART clock rate.
 *
 * The APB_UART clock feeds all UART modules. Its parent is selected by
 * the standard sunxi mux field and divided by the M and N factors.
 * PLL rates are derived from the PLL_PERI control register.
 */
static int ccu_cctl_get_apb_uart_rate(uint32_t base, uint32_t apb_uart_reg,
				      uint32_t pll_peri_reg, uint32_t *rate)
{
	uint32_t reg = sys_read32(base + apb_uart_reg);
	uint32_t mux = FIELD_GET(ALLWINNER_CCU_APB_UART_MUX_MASK, reg);
	uint32_t factor_m =
		FIELD_GET(ALLWINNER_CCU_APB_UART_FACTOR_M_MASK, reg) + 1;
	uint32_t factor_n =
		FIELD_GET(ALLWINNER_CCU_APB_UART_FACTOR_N_MASK, reg) + 1;
	uint32_t parent;

	switch (mux) {
	case ALLWINNER_CCU_APB_UART_MUX_HOSC:
		parent = ALLWINNER_CCU_HOSC_RATE;
		break;
	case ALLWINNER_CCU_APB_UART_MUX_CLK32K:
		parent = ALLWINNER_CCU_CLK32K_RATE;
		break;
	case ALLWINNER_CCU_APB_UART_MUX_PLL480M:
	case ALLWINNER_CCU_APB_UART_MUX_PLL1X: {
		uint32_t pll = sys_read32(base + pll_peri_reg);
		uint32_t n = FIELD_GET(ALLWINNER_CCU_PLL_PERI_N_MASK, pll) + 1;
		uint32_t p0 =
			FIELD_GET(ALLWINNER_CCU_PLL_PERI_P0_MASK, pll) + 1;
		uint32_t m =
			(pll & ALLWINNER_CCU_PLL_PERI_INPUT_DIV2) ? 2 : 1;
		uint32_t pll_peri = ALLWINNER_CCU_HOSC_RATE / m / p0 * n;

		if (mux == ALLWINNER_CCU_APB_UART_MUX_PLL480M) {
			uint32_t div = FIELD_GET(ALLWINNER_CCU_PLL_PERI_DIV_MASK,
						 pll);

			parent = pll_peri / (div + 1);
		} else {
			parent = pll_peri / 2;
		}
		break;
	}
	default:
		return -ENOTSUP;
	}

	*rate = parent / factor_m / factor_n;

	return 0;
}

/* The PLL output divided by the post dividers: the clock of the CPU core */
static uint32_t ccu_cctl_cpu_div(uint32_t reg)
{
	return (FIELD_GET(ALLWINNER_CCU_PLL_CPU_P_MASK, reg) + 1) *
	       (FIELD_GET(ALLWINNER_CCU_PLL_CPU_M0_MASK, reg) + 1) *
	       (FIELD_GET(ALLWINNER_CCU_PLL_CPU_M1_MASK, reg) + 1);
}

static uint32_t ccu_cctl_cpu_rate(uint32_t base)
{
	uint32_t reg = sys_read32(base + ALLWINNER_CCU_PLL_CPU_REG);

	return ALLWINNER_CCU_HOSC_RATE / ccu_cctl_cpu_div(reg) *
	       FIELD_GET(ALLWINNER_CCU_PLL_CPU_N_MASK, reg);
}

/*
 * The factor N is latched by the update bit while the spread spectrum mode is
 * on; the mode is then switched off and latched again. The CPU keeps running
 * from the PLL throughout, the output settles within the delay at the end.
 */
static int ccu_cctl_set_cpu_rate(const struct device *dev, uint32_t base, uint32_t rate)
{
	struct ccu_cctl_data *data = dev->data;
	uintptr_t ctrl = base + ALLWINNER_CCU_PLL_CPU_REG;
	uintptr_t ssc = base + ALLWINNER_CCU_PLL_CPU_SSC_REG;
	uint32_t div = ccu_cctl_cpu_div(sys_read32(ctrl));
	uint32_t n = DIV_ROUND_CLOSEST(rate, ALLWINNER_CCU_HOSC_RATE / div);

	if (rate > ALLWINNER_CCU_PLL_CPU_MAX_RATE || n < ALLWINNER_CCU_PLL_CPU_N_MIN ||
	    n > ALLWINNER_CCU_PLL_CPU_N_MAX) {
		return -EINVAL;
	}

	K_SPINLOCK(&data->lock) {
		sys_write32(sys_read32(ssc) | ALLWINNER_CCU_PLL_CPU_SSC_MODE, ssc);
		sys_write32((sys_read32(ctrl) & ~ALLWINNER_CCU_PLL_CPU_N_MASK) |
			    FIELD_PREP(ALLWINNER_CCU_PLL_CPU_N_MASK, n) |
			    ALLWINNER_CCU_PLL_CPU_UPDATE, ctrl);
		while (sys_read32(ctrl) & ALLWINNER_CCU_PLL_CPU_UPDATE) {
		}
		sys_write32(sys_read32(ssc) & ~ALLWINNER_CCU_PLL_CPU_SSC_MODE, ssc);
		sys_write32(sys_read32(ctrl) | ALLWINNER_CCU_PLL_CPU_UPDATE, ctrl);
		while (sys_read32(ctrl) & ALLWINNER_CCU_PLL_CPU_UPDATE) {
		}
		k_busy_wait(200);
	}

	return 0;
}

static int ccu_cctl_set_rate(const struct device *dev, clock_control_subsys_t sys,
			     clock_control_subsys_rate_t rate)
{
	const struct ccu_cctl_config *config = dev->config;
	uint32_t id = (uint32_t)(uintptr_t)sys;
	uint32_t base_idx = ALLWINNER_CCU_ID_BASE(id);

	if (base_idx >= config->n_bases) {
		return -EINVAL;
	}
	if (ccu_cctl_is_cpu(config, id)) {
		return ccu_cctl_set_cpu_rate(dev, config->bases[base_idx],
					     (uint32_t)(uintptr_t)rate);
	}

	return -ENOTSUP;
}

static int ccu_cctl_get_rate(const struct device *dev,
			     clock_control_subsys_t sys, uint32_t *rate)
{
	const struct ccu_cctl_config *config = dev->config;
	uint32_t id = (uint32_t)(uintptr_t)sys;
	uint32_t base_idx = ALLWINNER_CCU_ID_BASE(id);

	if (base_idx >= config->n_bases) {
		return -EINVAL;
	}

	if (ccu_cctl_is_cpu(config, id)) {
		*rate = ccu_cctl_cpu_rate(config->bases[base_idx]);
		return 0;
	}
	if (config->uart_bgr_reg != 0 &&
	    ALLWINNER_CCU_ID_REG(id) == config->uart_bgr_reg) {
		return ccu_cctl_get_apb_uart_rate(config->bases[base_idx],
						  config->apb_uart_reg,
						  config->pll_peri_reg, rate);
	}

	return -ENOTSUP;
}

static DEVICE_API(clock_control, ccu_cctl_driver_api) = {
	.on = ccu_cctl_on,
	.off = ccu_cctl_off,
	.get_status = ccu_cctl_get_status,
	.get_rate = ccu_cctl_get_rate,
	.set_rate = ccu_cctl_set_rate,
};

#define CCU_CCTL_INIT(n)							\
	static const uint32_t ccu_bases_##n[] =				\
		DT_PROP(DT_INST_PARENT(n), ccu_bases);			\
	static const struct ccu_cctl_config ccu_cctl_config_##n = {		\
		.bases = ccu_bases_##n,					\
		.n_bases = DT_PROP_LEN(DT_INST_PARENT(n), ccu_bases),	\
		.apb_uart_reg = DT_INST_PROP_OR(n, apb_uart_reg, 0),	\
		.pll_peri_reg = DT_INST_PROP_OR(n, pll_peri_reg, 0),	\
		.uart_bgr_reg = DT_INST_PROP_OR(n, uart_bgr_reg, 0),	\
	};									\
	static struct ccu_cctl_data ccu_cctl_data_##n;				\
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, &ccu_cctl_data_##n,		\
			      &ccu_cctl_config_##n, PRE_KERNEL_1,		\
			      CONFIG_CLOCK_CONTROL_INIT_PRIORITY,		\
			      &ccu_cctl_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CCU_CCTL_INIT)