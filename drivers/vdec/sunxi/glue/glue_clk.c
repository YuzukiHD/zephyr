/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Clock handles for the archive. It asks for the module clock, the two bus
 * gates and a few PLL outputs by number. The module clock is the only one
 * with a selectable parent and divider; the PLL outputs are read only.
 */

#include <errno.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/dt-bindings/clock/allwinner-ccu.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "glue.h"

LOG_MODULE_DECLARE(vdec_sunxi_glue, CONFIG_VDEC_SUNXI_LOG_LEVEL);

#define PLL_PERI_REG	(VE_CCU_BASE + 0x20)
#define HOSC_RATE	24000000U

/* module clock register */
#define MOD_GATE	BIT(31)
#define MOD_MUX_MASK	GENMASK(26, 24)
#define MOD_DIV_MASK	GENMASK(4, 0)

/* parent order of the mux field */
static const uint16_t mod_parents[] = {
	GLUE_CLK_PERI_480M, GLUE_CLK_PERI_2X, GLUE_CLK_AUDIO1_DIV2, GLUE_CLK_CPU_PLL, GLUE_CLK_DDR_PLL,
};

struct glue_clk {
	uint16_t id;
};

static struct glue_clk clks[] = {
	{ GLUE_CLK_CPU_PLL },    { GLUE_CLK_DDR_PLL },       { GLUE_CLK_PERI_2X },
	{ GLUE_CLK_PERI_1X },    { GLUE_CLK_PERI_480M },     { GLUE_CLK_AUDIO1_DIV2 },
	{ GLUE_CLK_VE },         { GLUE_CLK_BUS_VE },        { GLUE_CLK_BUS_VE_M },
};

static const struct device *const ccu = DEVICE_DT_GET(DT_CLOCKS_CTLR_BY_NAME(VE_NODE, bus));

static uint32_t pll_peri_rate(uint32_t *rate_480m)
{
	uint32_t pll = sys_read32(PLL_PERI_REG);
	uint32_t n = FIELD_GET(ALLWINNER_CCU_PLL_PERI_N_MASK, pll) + 1;
	uint32_t p0 = FIELD_GET(ALLWINNER_CCU_PLL_PERI_P0_MASK, pll) + 1;
	uint32_t m = (pll & ALLWINNER_CCU_PLL_PERI_INPUT_DIV2) ? 2 : 1;
	uint32_t raw = HOSC_RATE / m / p0 * n;

	*rate_480m = raw / (FIELD_GET(ALLWINNER_CCU_PLL_PERI_DIV_MASK, pll) + 1);

	return raw;
}

static uint32_t source_rate(uint16_t id)
{
	uint32_t r480;
	uint32_t raw = pll_peri_rate(&r480);

	switch (id) {
	case GLUE_CLK_PERI_480M:
		return r480;
	case GLUE_CLK_PERI_2X:
		return raw;
	case GLUE_CLK_PERI_1X:
		return raw / 2;
	default:
		return 0;
	}
}

static uint32_t mod_parent_rate(void)
{
	uint32_t mux = FIELD_GET(MOD_MUX_MASK, sys_read32(VE_CLK_REG));

	return mux < ARRAY_SIZE(mod_parents) ? source_rate(mod_parents[mux]) : 0;
}

static clock_control_subsys_t gate_id(uint16_t id)
{
	switch (id) {
	case GLUE_CLK_BUS_VE:
		return (clock_control_subsys_t)DT_CLOCKS_CELL_BY_NAME(VE_NODE, bus, clkid);
	case GLUE_CLK_BUS_VE_M:
		return (clock_control_subsys_t)DT_CLOCKS_CELL_BY_NAME(VE_NODE, mbus, clkid);
	default:
		return NULL;
	}
}

int hal_n_clk_get(uint8_t cc_id, uint16_t clk_id, void **clk)
{
	if (cc_id != GLUE_CCU_SYS) {
		return -EINVAL;
	}
	for (size_t i = 0; i < ARRAY_SIZE(clks); i++) {
		if (clks[i].id == clk_id) {
			*clk = &clks[i];
			return 0;
		}
	}
	LOG_WRN("unknown clock %u", clk_id);

	return -ENOENT;
}

int hal_n_clk_enable(void *clk)
{
	struct glue_clk *c = clk;

	if (c->id == GLUE_CLK_VE) {
		sys_set_bits(VE_CLK_REG, MOD_GATE);
		return 0;
	}

	return clock_control_on(ccu, gate_id(c->id));
}

int hal_n_clk_disable(void *clk)
{
	struct glue_clk *c = clk;

	if (c->id == GLUE_CLK_VE) {
		sys_clear_bits(VE_CLK_REG, MOD_GATE);
		return 0;
	}

	return clock_control_off(ccu, gate_id(c->id));
}

int hal_n_clk_set_parent(void *clk, void *parent)
{
	struct glue_clk *c = clk;
	struct glue_clk *p = parent;

	if (c->id != GLUE_CLK_VE) {
		return -ENOTSUP;
	}
	for (uint32_t i = 0; i < ARRAY_SIZE(mod_parents); i++) {
		if (mod_parents[i] == p->id) {
			uint32_t reg = sys_read32(VE_CLK_REG) & ~MOD_MUX_MASK;

			sys_write32(reg | FIELD_PREP(MOD_MUX_MASK, i), VE_CLK_REG);
			return 0;
		}
	}

	return -EINVAL;
}

static uint32_t div_for(uint32_t parent, uint32_t freq)
{
	return CLAMP((parent + freq / 2) / freq, 1U, MOD_DIV_MASK + 1U);
}

int hal_n_clk_set_freq(void *clk, uint32_t freq)
{
	struct glue_clk *c = clk;
	uint32_t parent = mod_parent_rate();
	uint32_t reg;

	if (c->id != GLUE_CLK_VE || parent == 0U || freq == 0U) {
		return -EINVAL;
	}
	reg = sys_read32(VE_CLK_REG) & ~MOD_DIV_MASK;
	sys_write32(reg | (div_for(parent, freq) - 1U), VE_CLK_REG);

	return 0;
}

int hal_n_clk_get_freq(void *clk, uint32_t *freq)
{
	struct glue_clk *c = clk;

	if (c->id == GLUE_CLK_VE) {
		*freq = mod_parent_rate() / (FIELD_GET(MOD_DIV_MASK, sys_read32(VE_CLK_REG)) + 1U);
	} else {
		*freq = source_rate(c->id);
	}

	return 0;
}

int hal_n_clk_round_freq(void *clk, uint32_t *freq)
{
	struct glue_clk *c = clk;
	uint32_t parent = mod_parent_rate();

	if (c->id != GLUE_CLK_VE || parent == 0U || *freq == 0U) {
		return -EINVAL;
	}
	*freq = parent / div_for(parent, *freq);

	return 0;
}
