/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Vector versions of the loops the CPU runs in this driver. Each function does what the plain C
 * loop in its comment does, byte for byte; without the vector extension (or with
 * CONFIG_VDEC_SUNXI_RVV off) the plain C loop is what runs.
 */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_RVV_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_RVV_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_VDEC_SUNXI_RVV) && defined(__riscv_v)
#define VE_HAVE_RVV 1
#include <riscv_vector.h>
#else
#define VE_HAVE_RVV 0
#endif

#if VE_HAVE_RVV

/* for (i = 0; i < n; i++) cur[i] += up[i]; */
static inline void ve_rvv_add_bytes(uint8_t *cur, const uint8_t *up, size_t n)
{
	while (n > 0) {
		size_t vl = __riscv_vsetvl_e8m8(n);
		vuint8m8_t a = __riscv_vle8_v_u8m8(cur, vl);
		vuint8m8_t b = __riscv_vle8_v_u8m8(up, vl);

		__riscv_vse8_v_u8m8(cur, __riscv_vadd_vv_u8m8(a, b, vl), vl);
		cur += vl;
		up += vl;
		n -= vl;
	}
}

/*
 * Adler-32 of n bytes, n <= 5552 so that the sums do not overflow before the modulo:
 *   while (n--) { a += *p++; b += a; }
 * A piece of vl bytes adds vl * a + sum((vl - j) * p[j]) to b and sum(p[j]) to a.
 */
static inline void ve_rvv_adler_block(uint32_t *pa, uint32_t *pb, const uint8_t *p, uint32_t n)
{
	uint32_t a = *pa, b = *pb;

	while (n > 0) {
		size_t vl = __riscv_vsetvl_e8m2(n);
		vuint16m4_t w = __riscv_vzext_vf2_u16m4(__riscv_vle8_v_u8m2(p, vl), vl);
		vuint16m4_t weight = __riscv_vrsub_vx_u16m4(__riscv_vid_v_u16m4(vl), (uint16_t)vl, vl);
		vuint32m8_t prod = __riscv_vwmulu_vv_u32m8(w, weight, vl);
		vuint32m1_t zero = __riscv_vmv_s_x_u32m1(0, 1);
		uint32_t weighted = __riscv_vmv_x_s_u32m1_u32(
			__riscv_vredsum_vs_u32m8_u32m1(prod, zero, vl));
		uint32_t sum = __riscv_vmv_x_s_u32m1_u32(
			__riscv_vwredsumu_vs_u16m4_u32m1(w, zero, vl));

		b += a * (uint32_t)vl + weighted;
		a += sum;
		p += vl;
		n -= vl;
	}
	*pa = a;
	*pb = b;
}

/*
 * RGBA pixels (r, g, b, a bytes) to words with the bytes the other way round (a, b, g, r in memory):
 *   for each pixel: out[0] = in[3]; out[1] = in[2]; out[2] = in[1]; out[3] = in[0];
 * The buffers have any alignment: the data is loaded and stored as bytes and only looked at as
 * 32 bit words inside the registers.
 */
static inline void ve_rvv_reverse_pixels(uint8_t *out, const uint8_t *in, size_t pixels)
{
	while (pixels > 0) {
		size_t vl = __riscv_vsetvl_e32m4(pixels);
		vuint32m4_t x = __riscv_vreinterpret_v_u8m4_u32m4(__riscv_vle8_v_u8m4(in, vl * 4));
		vuint32m4_t r = __riscv_vsll_vx_u32m4(x, 24, vl);

		r = __riscv_vor_vv_u32m4(r, __riscv_vsll_vx_u32m4(__riscv_vand_vx_u32m4(x, 0xff00, vl), 8, vl), vl);
		r = __riscv_vor_vv_u32m4(r, __riscv_vand_vx_u32m4(__riscv_vsrl_vx_u32m4(x, 8, vl), 0xff00, vl), vl);
		r = __riscv_vor_vv_u32m4(r, __riscv_vsrl_vx_u32m4(x, 24, vl), vl);
		__riscv_vse8_v_u8m4(out, __riscv_vreinterpret_v_u32m4_u8m4(r), vl * 4);
		in += vl * 4;
		out += vl * 4;
		pixels -= vl;
	}
}

/*
 * Swap the two bytes of every pair: for (i = 0; i < pairs; i++) swap(p[2 * i], p[2 * i + 1]);
 * p is 2 byte aligned.
 */
static inline void ve_rvv_swap_pairs(uint8_t *p, size_t pairs)
{
	uint16_t *q = (uint16_t *)p;

	while (pairs > 0) {
		size_t vl = __riscv_vsetvl_e16m8(pairs);
		vuint16m8_t x = __riscv_vle16_v_u16m8(q, vl);

		x = __riscv_vor_vv_u16m8(__riscv_vsll_vx_u16m8(x, 8, vl),
					 __riscv_vsrl_vx_u16m8(x, 8, vl), vl);
		__riscv_vse16_v_u16m8(q, x, vl);
		q += vl;
		pairs -= vl;
	}
}

/*
 * First position i with p[i] == 0, p[i + 1] == 0 and p[i + 2] == 1 and i + 3 <= n, or n when there is none.
 */
static inline size_t ve_rvv_find_start_code(const uint8_t *p, size_t n)
{
	size_t i = 0;

	while (i + 3 <= n) {
		size_t vl = __riscv_vsetvl_e8m4(n - i - 2);
		vuint8m4_t b0 = __riscv_vle8_v_u8m4(p + i, vl);
		vuint8m4_t b1 = __riscv_vle8_v_u8m4(p + i + 1, vl);
		vuint8m4_t b2 = __riscv_vle8_v_u8m4(p + i + 2, vl);
		vbool2_t m = __riscv_vmand_mm_b2(__riscv_vmseq_vx_u8m4_b2(b0, 0, vl),
						 __riscv_vmseq_vx_u8m4_b2(b1, 0, vl), vl);
		long first;

		m = __riscv_vmand_mm_b2(m, __riscv_vmseq_vx_u8m4_b2(b2, 1, vl), vl);
		first = __riscv_vfirst_m_b2(m, vl);
		if (first >= 0) {
			return i + first;
		}
		i += vl;
	}

	return n;
}

#endif /* VE_HAVE_RVV */

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_RVV_H_ */
