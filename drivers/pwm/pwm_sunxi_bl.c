/*
 * Allwinner PWM_BL: dedicated backlight block (digital dimming stage feeding
 * an analog current sink)
 *
 * SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

#define DT_DRV_COMPAT allwinner_sunxi_pwm_bl

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/pinctrl/pinctrl_sunxi.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(pwm_sunxi_bl, CONFIG_PWM_LOG_LEVEL);

#define PWMBL_PORT_STRIDE	0x200U
#define PWMBL_PORTS		2U

#define PWMBL_CTRL		0x0000U
#define PWMBL_CLK_CFG		0x0004U
#define PWMBL_PRD		0x0008U
#define PWMBL_ACT_CYCLE		0x000cU
#define PWMBL_ACT_STEP		0x0010U
#define PWMBL_ACT_UP_CYCLE	0x0014U
#define PWMBL_OCP_FLT_V		0x0018U
#define PWMBL_INT_EN		0x0040U
#define PWMBL_ANA0		0x0050U
#define PWMBL_ANA1		0x0054U
#define PWMBL_ANA2		0x0058U

#define CTRL_ENABLE		BIT(0)
#define CTRL_CLK_GATING		BIT(1)
#define CTRL_MODE_DIG_ANA	BIT(2)

#define ANA1_MAX_BRIGHT_MASK	GENMASK(2, 0)
#define ANA1_LOAD_CUR_MASK	GENMASK(23, 16)
#define ANA1_OCP_EN		BIT(31)
#define OCP_FLT_V_MASK		GENMASK(7, 0)
#define OCP_FLT_EN		BIT(31)

#define PWMBL_VF_400MV		2U
#define PWMBL_VF_500MV		3U
#define PWMBL_ANA_750K		1U
#define PWMBL_SRC_PERI		2U

#define PWMBL_MAX_LOAD_CUR	200U
#define PWMBL_DIM_STEPS		256U

/* CCU helpers: pwm-bl module clock (mux 26:24, div 4:0, gate 31) and bus */
#define CCU_PLL_PERI_REG	0x0020U
#define HOSC_RATE		24000000U
#define PWMBL_MOD_TARGET	400000000U

struct pwm_sunxi_bl_config {
	uintptr_t base;
	uintptr_t ccu;
	uint16_t mod_reg;
	uint16_t bus_reg;
	const struct pinctrl_dev_config *pcfg;
};

struct pwm_sunxi_bl_data {
	struct k_mutex lock;
};

static inline uint32_t pbl_rd(const struct pwm_sunxi_bl_config *c,
			      unsigned int port, uint32_t off)
{
	return sys_read32(c->base + port * PWMBL_PORT_STRIDE + off);
}

static inline void pbl_wr(const struct pwm_sunxi_bl_config *c, unsigned int port,
			  uint32_t off, uint32_t val)
{
	sys_write32(val, c->base + port * PWMBL_PORT_STRIDE + off);
}

static void pbl_upd(const struct pwm_sunxi_bl_config *c, unsigned int port,
		    uint32_t off, uint32_t mask, uint32_t val)
{
	uint32_t v = pbl_rd(c, port, off);

	pbl_wr(c, port, off, (v & ~mask) | (val & mask));
}

static uint32_t pll_peri_rate(const struct pwm_sunxi_bl_config *c)
{
	uint32_t pll = sys_read32(c->ccu + CCU_PLL_PERI_REG);
	uint32_t n = ((pll >> 8) & 0xff) + 1;
	uint32_t p0 = ((pll >> 16) & 0x7) + 1;
	uint32_t m = (pll & BIT(1)) ? 2 : 1;

	return HOSC_RATE / m / p0 * n;
}

/* bus clock + reset + module clock (PLL_PERI_2X / div, close to 400 MHz) */
static void pbl_clock_init(const struct pwm_sunxi_bl_config *c)
{
	uint32_t src = pll_peri_rate(c);
	uint32_t div = CLAMP(DIV_ROUND_CLOSEST(src, PWMBL_MOD_TARGET), 1U, 32U);
	uint32_t reg;

	reg = sys_read32(c->ccu + c->bus_reg);
	reg |= BIT(16) | BIT(0);		/* deassert reset, ungate bus */
	sys_write32(reg, c->ccu + c->bus_reg);

	reg = sys_read32(c->ccu + c->mod_reg);
	reg &= ~(BIT(31) | GENMASK(26, 24) | GENMASK(4, 0));	/* PLL_PERI_2X */
	reg |= div - 1;
	sys_write32(reg, c->ccu + c->mod_reg);
	sys_write32(reg | BIT(31), c->ccu + c->mod_reg);
}

/* pin drive level 3 on PB0 (port 1) and PB2 (port 0) */
static void pbl_pin_drive(void)
{
	const struct sunxi_pio_hw_info *hw = &sunxi_pio_hw_type0_info;
	uintptr_t bank = DT_REG_ADDR(DT_NODELABEL(pio)) + 1 * hw->bank_mem_size;
	static const uint8_t pins[] = { 0, 2 };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		uintptr_t reg = bank + hw->drv_regs_offset +
				(pins[i] / hw->drv_pins_per_reg) * 4;
		uint32_t shift = (pins[i] % hw->drv_pins_per_reg) * hw->drv_pins_bits;

		sys_write32((sys_read32(reg) & ~(0xfU << shift)) | (3U << shift), reg);
	}
}

/* port 1: pure analog, 750 kHz (hal_pwm_bl_analog_init) */
static void pbl_port1_init(const struct pwm_sunxi_bl_config *c)
{
	pbl_upd(c, 1, PWMBL_CTRL, CTRL_MODE_DIG_ANA, 0);
	pbl_upd(c, 1, PWMBL_ANA2, 0x3, PWMBL_ANA_750K);
	pbl_upd(c, 1, PWMBL_INT_EN, BIT(0) | BIT(2) | BIT(3), BIT(0) | BIT(2) | BIT(3));
	pbl_upd(c, 1, PWMBL_ANA1, ANA1_OCP_EN, 0);
	pbl_upd(c, 1, PWMBL_OCP_FLT_V, OCP_FLT_V_MASK, 0xff);
	pbl_upd(c, 1, PWMBL_OCP_FLT_V, OCP_FLT_EN, OCP_FLT_EN);
	pbl_upd(c, 1, PWMBL_ANA1, ANA1_MAX_BRIGHT_MASK, PWMBL_VF_400MV);
	pbl_upd(c, 1, PWMBL_ANA1, ANA1_LOAD_CUR_MASK, 0);
	pbl_upd(c, 1, PWMBL_ANA0, BIT(0), BIT(0));
	pbl_upd(c, 1, PWMBL_ANA1, ANA1_LOAD_CUR_MASK, 0xffU << 16);
}

/* port 0: digital + analog compare (hal_pwm_bl_dig_compare_init) */
static void pbl_port0_init(const struct pwm_sunxi_bl_config *c)
{
	pbl_upd(c, 0, PWMBL_CTRL, CTRL_MODE_DIG_ANA, CTRL_MODE_DIG_ANA);
	/* reference clock: PERI source, K = 0, M = 0 */
	pbl_upd(c, 0, PWMBL_CLK_CFG, GENMASK(31, 30), PWMBL_SRC_PERI << 30);
	pbl_upd(c, 0, PWMBL_CLK_CFG, GENMASK(14, 8), 0);
	pbl_upd(c, 0, PWMBL_CLK_CFG, GENMASK(3, 0), 0);
	/* period 0x215, initial active 0x1a, max active 0x140 */
	pbl_upd(c, 0, PWMBL_PRD, GENMASK(15, 0), 0x215);
	pbl_upd(c, 0, PWMBL_ACT_CYCLE, GENMASK(15, 0), 0x140);
	pbl_upd(c, 0, PWMBL_ACT_CYCLE, GENMASK(31, 16), 0x1aU << 16);
	pbl_upd(c, 0, PWMBL_ACT_STEP, GENMASK(15, 0), 1);
	pbl_upd(c, 0, PWMBL_ACT_UP_CYCLE, GENMASK(7, 0), 1);
	pbl_upd(c, 0, PWMBL_ANA1, ANA1_OCP_EN, 0);
	pbl_upd(c, 0, PWMBL_OCP_FLT_V, OCP_FLT_V_MASK, 0xff);
	pbl_upd(c, 0, PWMBL_OCP_FLT_V, OCP_FLT_EN, OCP_FLT_EN);
	pbl_upd(c, 0, PWMBL_INT_EN, BIT(0) | BIT(2) | BIT(3), BIT(0) | BIT(2) | BIT(3));
	pbl_upd(c, 0, PWMBL_ANA1, ANA1_MAX_BRIGHT_MASK, PWMBL_VF_500MV);
	pbl_upd(c, 0, PWMBL_ANA1, ANA1_LOAD_CUR_MASK, 0);
	pbl_upd(c, 0, PWMBL_CTRL, CTRL_CLK_GATING, CTRL_CLK_GATING);
	pbl_upd(c, 0, PWMBL_ANA0, BIT(0), BIT(0));
}

/* output on both ports, load current on port 0 */
static void pbl_set_level(const struct pwm_sunxi_bl_config *c, uint32_t level)
{
	unsigned int port;

	level = MIN(level, PWMBL_MAX_LOAD_CUR);
	for (port = 0; port < PWMBL_PORTS; port++) {
		pbl_upd(c, port, PWMBL_CTRL, CTRL_ENABLE, level ? CTRL_ENABLE : 0);
	}
	pbl_upd(c, 0, PWMBL_ANA1, ANA1_LOAD_CUR_MASK, level << 16);
}

static int pwm_sunxi_bl_set_cycles(const struct device *dev, uint32_t channel,
				   uint32_t period_cycles, uint32_t pulse_cycles,
				   pwm_flags_t flags)
{
	const struct pwm_sunxi_bl_config *c = dev->config;
	struct pwm_sunxi_bl_data *d = dev->data;
	uint32_t level;

	ARG_UNUSED(flags);	/* the analog stage has no polarity */

	if (period_cycles == 0 || pulse_cycles > period_cycles) {
		return -EINVAL;
	}

	level = (uint32_t)(((uint64_t)pulse_cycles * PWMBL_DIM_STEPS) / period_cycles);

	k_mutex_lock(&d->lock, K_FOREVER);
	pbl_set_level(c, level);
	k_mutex_unlock(&d->lock);
	return 0;
}

static int pwm_sunxi_bl_get_cycles_per_sec(const struct device *dev,
					   uint32_t channel, uint64_t *cycles)
{
	ARG_UNUSED(dev);
	if (channel != 0) {
		return -EINVAL;
	}
	*cycles = NSEC_PER_SEC;	/* one cycle per nanosecond keeps ratios exact */
	return 0;
}

static int pwm_sunxi_bl_init(const struct device *dev)
{
	const struct pwm_sunxi_bl_config *c = dev->config;
	struct pwm_sunxi_bl_data *d = dev->data;
	int ret;

	k_mutex_init(&d->lock);

	pbl_clock_init(c);
	ret = pinctrl_apply_state(c->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}
	pbl_pin_drive();
	pbl_port1_init(c);
	pbl_port0_init(c);
	LOG_DBG("PWM_BL ready");
	return 0;
}

static DEVICE_API(pwm, pwm_sunxi_bl_api) = {
	.set_cycles = pwm_sunxi_bl_set_cycles,
	.get_cycles_per_sec = pwm_sunxi_bl_get_cycles_per_sec,
};

#define PWM_SUNXI_BL_INIT(n)							\
	PINCTRL_DT_INST_DEFINE(n);						\
	static const struct pwm_sunxi_bl_config pwm_sunxi_bl_cfg_##n = {	\
		.base = DT_INST_REG_ADDR(n),					\
		.ccu = DT_INST_PROP(n, ccu_base),				\
		.mod_reg = DT_INST_PROP(n, mod_clk_reg),			\
		.bus_reg = DT_INST_PROP(n, bus_clk_reg),			\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),			\
	};									\
	static struct pwm_sunxi_bl_data pwm_sunxi_bl_data_##n;			\
	DEVICE_DT_INST_DEFINE(n, pwm_sunxi_bl_init, NULL, &pwm_sunxi_bl_data_##n, \
			      &pwm_sunxi_bl_cfg_##n, POST_KERNEL,		\
			      CONFIG_PWM_SUNXI_BL_INIT_PRIORITY, &pwm_sunxi_bl_api);

DT_INST_FOREACH_STATUS_OKAY(PWM_SUNXI_BL_INIT)
