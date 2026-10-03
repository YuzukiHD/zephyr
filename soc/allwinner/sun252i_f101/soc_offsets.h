/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_ALLWINNER_SUN252I_F101_SOC_OFFSETS_H_
#define ZEPHYR_SOC_ALLWINNER_SUN252I_F101_SOC_OFFSETS_H_

#ifdef CONFIG_SUN252I_F101_RVV

#define GEN_SOC_OFFSET_SYMS()				\
	GEN_OFFSET_SYM(soc_esf_t, vstart);		\
	GEN_OFFSET_SYM(soc_esf_t, vl);			\
	GEN_OFFSET_SYM(soc_esf_t, vtype);		\
	GEN_OFFSET_SYM(soc_esf_t, vcsr);		\
	GEN_OFFSET_SYM(soc_esf_t, v)

#endif /* CONFIG_SUN252I_F101_RVV */

#endif /* ZEPHYR_SOC_ALLWINNER_SUN252I_F101_SOC_OFFSETS_H_ */
