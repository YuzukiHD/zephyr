/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <riscv_vector.h>
#include <stdint.h>
#include <stddef.h>
#include <zephyr/sys/util.h>

size_t rvv_vlenb(void)
{
	return __riscv_vsetvlmax_e8m1();
}

/* c[i] = a[i] + b[i] */
void rvv_add_u32(uint32_t *c32, const uint32_t *a32, const uint32_t *b32, size_t n)
{
	/* the intrinsics of this toolchain take unsigned long (32 bit here) */
	unsigned long *c = (unsigned long *)c32;
	const unsigned long *a = (const unsigned long *)a32;
	const unsigned long *b = (const unsigned long *)b32;

	while (n > 0) {
		size_t vl = __riscv_vsetvl_e32m8(n);
		vuint32m8_t va = __riscv_vle32_v_u32m8(a, vl);
		vuint32m8_t vb = __riscv_vle32_v_u32m8(b, vl);

		__riscv_vse32_v_u32m8(c, __riscv_vadd_vv_u32m8(va, vb, vl), vl);
		a += vl;
		b += vl;
		c += vl;
		n -= vl;
	}
}

/*
 * Fill v8 with a pattern and read it back later. vsetvli and the vector
 * instruction are in the same asm statement so the compiler cannot move or
 * drop the vtype setup (vill is set in a thread that never used vectors).
 */
void rvv_set_v8(uint32_t seed)
{
	__asm__ volatile("vsetvli t0, zero, e32, m1, ta, ma\n\t"
			 "vmv.v.x v8, %0"
			 :: "r"(seed) : "t0", "memory");
}

/* returns seed if all elements match, else the first wrong element */
uint32_t rvv_check_v8(uint32_t seed)
{
	uint32_t out[(CONFIG_SUN252I_F101_VLEN / 8) / 4];

	__asm__ volatile("vsetvli t0, zero, e32, m1, ta, ma\n\t"
			 "vse32.v v8, (%0)"
			 :: "r"(out) : "t0", "memory");
	for (size_t i = 0; i < ARRAY_SIZE(out); i++) {
		if (out[i] != seed) {
			return out[i];
		}
	}
	return seed;
}

/*
 * Keep a pattern in v8 over a loop without any function call, so only a
 * preemption (interrupt, time slice) can disturb it. Vector registers are
 * caller saved, a call or a voluntary switch (k_sleep, k_yield) may clobber
 * them. Returns seed if v8 survived, else the first wrong element.
 */
__attribute__((optimize("no-tree-vectorize")))
uint32_t rvv_hold_v8(uint32_t seed, uint32_t iters)
{
	uint32_t out[CONFIG_SUN252I_F101_VLEN / 32];

	__asm__ volatile("vsetvli t0, zero, e32, m1, ta, ma\n\t"
			 "vmv.v.x v8, %0"
			 :: "r"(seed) : "t0", "memory");
	for (volatile uint32_t i = 0; i < iters; i++) {
	}
	__asm__ volatile("vsetvli t0, zero, e32, m1, ta, ma\n\t"
			 "vse32.v v8, (%0)"
			 :: "r"(out) : "t0", "memory");
	for (size_t i = 0; i < ARRAY_SIZE(out); i++) {
		if (out[i] != seed) {
			return out[i];
		}
	}
	return seed;
}
