/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Vector state kept in every exception frame (CONFIG_RISCV_SOC_CONTEXT_SAVE).
 */

#ifndef ZEPHYR_SOC_ALLWINNER_SUN252I_F101_SOC_CONTEXT_H_
#define ZEPHYR_SOC_ALLWINNER_SUN252I_F101_SOC_CONTEXT_H_

#ifdef CONFIG_SUN252I_F101_RVV

#define CTX_VECTOR_BYTES (32 * (CONFIG_SUN252I_F101_VLEN / 8))

/* 8 words of CSRs keep the frame a multiple of 16 bytes */
#define SOC_ESF_MEMBERS							\
	unsigned long vstart;						\
	unsigned long vl;						\
	unsigned long vtype;						\
	unsigned long vcsr;						\
	unsigned long vpad[4];						\
	unsigned char v[CTX_VECTOR_BYTES] __aligned(16)

/* vtype 0 is a legal e8/m1 setting, so the first return into a new thread works */
#define SOC_ESF_INIT 0

#endif /* CONFIG_SUN252I_F101_RVV */

#endif /* ZEPHYR_SOC_ALLWINNER_SUN252I_F101_SOC_CONTEXT_H_ */
