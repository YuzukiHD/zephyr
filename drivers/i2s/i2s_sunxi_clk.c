/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "i2s_sunxi_clk.h"

LOG_MODULE_DECLARE(i2s_sunxi, CONFIG_I2S_LOG_LEVEL);

#define CCU_BASE		0x02001000U
#define HOSC_HZ			24000000U

/* PLL_AUDIO1: N[15:8] (multiplier is N + 1), /2 output [18:16], /5 output [22:20] */
#define PLL_AUDIO1_REG		0x0080U
#define PLL_AUDIO1_SDM_EN	BIT(24)
#define PLL_AUDIO1_OUT		BIT(27)
#define PLL_AUDIO1_LOCKED	BIT(28)
#define PLL_AUDIO1_LOCK_EN	BIT(29)
#define PLL_AUDIO1_LDO		BIT(30)
#define PLL_AUDIO1_EN		BIT(31)
#define PLL_AUDIO1_N_SHIFT	8
#define PLL_AUDIO1_DIV2_SHIFT	16
#define PLL_AUDIO1_DIV5_SHIFT	20
#define PLL_AUDIO1_DIV_MASK	0x7U
/* Fractional pattern registers of the 44.1 kHz family */
#define PLL_AUDIO1_PAT0_REG	0x0180U
#define PLL_AUDIO1_PAT1_REG	0x0184U
#define PLL_AUDIO1_PAT0_44K1	0xc000a234U

/* Module clock registers: M[4:0], P[9:8], source[26:24], gate 31 */
#define MOD_M_MASK		0x1fU
#define MOD_P_SHIFT		8
#define MOD_P_MASK		0x3U
#define MOD_MUX_SHIFT		24
#define MOD_MUX_DIV2		0U
#define MOD_MUX_DIV5		1U
#define MOD_GATE		BIT(31)

#define PLL_LOCK_TIMEOUT_US	2000

/* PLL N + 1 and the divided output used by each family */
#define PLL_48K_N		128U	/* 24 MHz * 128 = 3.072 GHz */
#define PLL_48K_DIV		5U	/* 614.4 MHz */
#define PLL_44K1_N		90U	/* 2.160 GHz plus the fractional part */
#define PLL_44K1_RATE		2167603200U
#define PLL_44K1_DIV		2U	/* 1083.8016 MHz */

static K_MUTEX_DEFINE(pll_lock);
static int pll_users;
static enum sunxi_audio_family pll_family;

static inline uint32_t ccu_rd(uint32_t off)
{
	return sys_read32(CCU_BASE + off);
}

static inline void ccu_wr(uint32_t off, uint32_t val)
{
	sys_write32(val, CCU_BASE + off);
}

int sunxi_audio_family(uint32_t frame_rate)
{
	if (frame_rate != 0U && frame_rate % 8000U == 0U) {
		return SUNXI_AUDIO_FAMILY_48K;
	}
	if (frame_rate % 11025U == 0U && frame_rate != 0U) {
		return SUNXI_AUDIO_FAMILY_44K1;
	}
	if (frame_rate == 44100U || frame_rate == 22050U) {
		return SUNXI_AUDIO_FAMILY_44K1;
	}

	return -EINVAL;
}

uint32_t sunxi_audio_base_mclk(enum sunxi_audio_family family)
{
	return family == SUNXI_AUDIO_FAMILY_48K ? 24576000U : 22579200U;
}

static uint32_t pll_out_rate(enum sunxi_audio_family family)
{
	return family == SUNXI_AUDIO_FAMILY_48K ? HOSC_HZ * PLL_48K_N / PLL_48K_DIV :
						  PLL_44K1_RATE / PLL_44K1_DIV;
}

static int pll_start(enum sunxi_audio_family family)
{
	uint32_t reg = ccu_rd(PLL_AUDIO1_REG);
	uint32_t n = family == SUNXI_AUDIO_FAMILY_48K ? PLL_48K_N : PLL_44K1_N;
	int tries = PLL_LOCK_TIMEOUT_US;

	/* reprogram with the output gated and the PLL off */
	reg &= ~(PLL_AUDIO1_OUT | PLL_AUDIO1_EN | PLL_AUDIO1_LDO | PLL_AUDIO1_LOCK_EN |
		 PLL_AUDIO1_SDM_EN | (0xffU << PLL_AUDIO1_N_SHIFT) |
		 (PLL_AUDIO1_DIV_MASK << PLL_AUDIO1_DIV2_SHIFT) |
		 (PLL_AUDIO1_DIV_MASK << PLL_AUDIO1_DIV5_SHIFT));
	ccu_wr(PLL_AUDIO1_REG, reg);

	reg |= ((n - 1U) << PLL_AUDIO1_N_SHIFT) |
	       ((PLL_44K1_DIV - 1U) << PLL_AUDIO1_DIV2_SHIFT) |
	       ((PLL_48K_DIV - 1U) << PLL_AUDIO1_DIV5_SHIFT);
	if (family == SUNXI_AUDIO_FAMILY_44K1) {
		ccu_wr(PLL_AUDIO1_PAT0_REG, PLL_AUDIO1_PAT0_44K1);
		ccu_wr(PLL_AUDIO1_PAT1_REG, 0U);
		reg |= PLL_AUDIO1_SDM_EN;
	} else {
		ccu_wr(PLL_AUDIO1_PAT0_REG, 0U);
		ccu_wr(PLL_AUDIO1_PAT1_REG, 0U);
	}
	ccu_wr(PLL_AUDIO1_REG, reg);

	reg |= PLL_AUDIO1_LDO;
	ccu_wr(PLL_AUDIO1_REG, reg);
	reg |= PLL_AUDIO1_EN;
	ccu_wr(PLL_AUDIO1_REG, reg);
	reg |= PLL_AUDIO1_LOCK_EN;
	ccu_wr(PLL_AUDIO1_REG, reg);
	while (tries-- > 0 && (ccu_rd(PLL_AUDIO1_REG) & PLL_AUDIO1_LOCKED) == 0U) {
		k_busy_wait(1);
	}
	if ((ccu_rd(PLL_AUDIO1_REG) & PLL_AUDIO1_LOCKED) == 0U) {
		LOG_ERR("PLL_AUDIO1 does not lock (%08x)", ccu_rd(PLL_AUDIO1_REG));
		reg &= ~(PLL_AUDIO1_EN | PLL_AUDIO1_LDO | PLL_AUDIO1_LOCK_EN);
		ccu_wr(PLL_AUDIO1_REG, reg);
		return -ETIMEDOUT;
	}
	ccu_wr(PLL_AUDIO1_REG, reg | PLL_AUDIO1_OUT);

	return 0;
}

static void pll_stop(void)
{
	uint32_t reg = ccu_rd(PLL_AUDIO1_REG);

	reg &= ~PLL_AUDIO1_OUT;
	ccu_wr(PLL_AUDIO1_REG, reg);
	reg &= ~(PLL_AUDIO1_EN | PLL_AUDIO1_LDO | PLL_AUDIO1_LOCK_EN | PLL_AUDIO1_SDM_EN);
	ccu_wr(PLL_AUDIO1_REG, reg);
}

int sunxi_audio_pll_get(enum sunxi_audio_family family)
{
	int ret = 0;

	k_mutex_lock(&pll_lock, K_FOREVER);
	if (pll_users == 0) {
		ret = pll_start(family);
		if (ret == 0) {
			pll_family = family;
			pll_users = 1;
		}
	} else if (pll_family != family) {
		ret = -EBUSY;
	} else {
		pll_users++;
	}
	k_mutex_unlock(&pll_lock);

	return ret;
}

void sunxi_audio_pll_put(void)
{
	k_mutex_lock(&pll_lock, K_FOREVER);
	if (pll_users > 0 && --pll_users == 0) {
		pll_stop();
	}
	k_mutex_unlock(&pll_lock);
}

int sunxi_audio_module_clk_set(uint32_t reg, uint32_t rate, uint32_t *actual)
{
	uint32_t src, div, best_p = 0, best_m = 1, best_err = UINT32_MAX;

	if (pll_users == 0 || rate == 0U) {
		return -EINVAL;
	}
	src = pll_out_rate(pll_family);
	div = DIV_ROUND_CLOSEST(src, rate);
	for (uint32_t p = 0; p < 4U; p++) {
		uint32_t m = DIV_ROUND_CLOSEST(div, 1U << p);
		uint32_t got, err;

		if (m < 1U || m > 32U) {
			continue;
		}
		got = src / (m << p);
		err = got > rate ? got - rate : rate - got;
		if (err < best_err) {
			best_err = err;
			best_p = p;
			best_m = m;
		}
	}
	if (best_err > rate / 100U) {
		LOG_ERR("clock %u Hz out of %u Hz is not reachable", rate, src);
		return -ERANGE;
	}

	/* the gate stays closed while the divider changes */
	ccu_wr(reg, 0U);
	ccu_wr(reg, ((pll_family == SUNXI_AUDIO_FAMILY_48K ? MOD_MUX_DIV5 : MOD_MUX_DIV2)
		     << MOD_MUX_SHIFT) | (best_p << MOD_P_SHIFT) | (best_m - 1U));
	ccu_wr(reg, ccu_rd(reg) | MOD_GATE);
	if (actual != NULL) {
		*actual = src / (best_m << best_p);
	}

	return 0;
}

void sunxi_audio_module_clk_off(uint32_t reg)
{
	ccu_wr(reg, ccu_rd(reg) & ~MOD_GATE);
}
