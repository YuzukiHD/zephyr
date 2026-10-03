/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>

extern size_t rvv_vlenb(void);
extern void rvv_add_u32(uint32_t *c, const uint32_t *a, const uint32_t *b, size_t n);
extern void rvv_set_v8(uint32_t seed);
extern uint32_t rvv_check_v8(uint32_t seed);
extern uint32_t rvv_hold_v8(uint32_t seed, uint32_t iters);

#define N 100
#define ROUNDS 200
#define HOLD_ITERS 300000

static uint32_t fail;
static struct k_timer tick;
static volatile uint32_t ticks;

static void tick_fn(struct k_timer *t)
{
	/* the interrupt handler trashes v8 and the vector CSRs too */
	rvv_set_v8(0xdeadbeef);
	ticks++;
}

static void worker(void *p1, void *p2, void *p3)
{
	uint32_t seed = (uint32_t)(uintptr_t)p1;
	volatile double f = seed;

	for (int i = 0; i < ROUNDS; i++) {
		uint32_t s = seed + i;

		f = f * 1.0001 + 1.0;	/* FPU sharing next to it */
		uint32_t got = rvv_hold_v8(s, HOLD_ITERS);

		if (got != s) {
			printk("RVV: worker %x round %d: v8 %08x != %08x\n",
			       seed, i, got, s);
			fail++;
			break;
		}
	}
}

K_THREAD_STACK_DEFINE(stack_a, 2048);
K_THREAD_STACK_DEFINE(stack_b, 2048);
static struct k_thread thr_a, thr_b;

int main(void)
{
	static uint32_t a[N], b[N], c[N];
	/* vlenb in bytes = number of e8m1 elements */
	size_t vlenb = rvv_vlenb();
	printk("RVV: VLENB = %u ((CONFIG_SUN252I_F101_VLEN / 8) = %d)\n", (unsigned)vlenb,
	       (CONFIG_SUN252I_F101_VLEN / 8));
	if (vlenb != (CONFIG_SUN252I_F101_VLEN / 8)) {
		fail++;
	}

	for (int i = 0; i < N; i++) {
		a[i] = i * 3;
		b[i] = 1000 - i;
	}
	rvv_add_u32(c, a, b, N);
	for (int i = 0; i < N; i++) {
		if (c[i] != a[i] + b[i]) {
			printk("RVV: add[%d] = %u, expected %u\n", i, c[i], a[i] + b[i]);
			fail++;
			break;
		}
	}
	printk("RVV: vector add %s\n", fail ? "FAILED" : "ok");

	k_timer_init(&tick, tick_fn, NULL);
	k_timer_start(&tick, K_USEC(300), K_USEC(300));

	k_thread_create(&thr_a, stack_a, 2048, worker, (void *)0x11110000, NULL, NULL,
			5, 0, K_NO_WAIT);
	k_thread_create(&thr_b, stack_b, 2048, worker, (void *)0x22220000, NULL, NULL,
			5, 0, K_NO_WAIT);
	/* the main thread takes part in the time slicing */
	k_thread_priority_set(k_current_get(), 5);
	for (int i = 0; i < ROUNDS; i++) {
		uint32_t s = 0x33330000 + i;

		if (rvv_hold_v8(s, HOLD_ITERS) != s) {
			printk("RVV: main round %d lost v8\n", i);
			fail++;
			break;
		}
	}
	k_thread_join(&thr_a, K_FOREVER);
	k_thread_join(&thr_b, K_FOREVER);
	k_timer_stop(&tick);

	printk("RVV: %u timer ticks during the test\n", ticks);
	if (fail) {
		printk("RVV: %u check(s) FAILED\n", fail);
	} else {
		printk("RVV: all checks passed\n");
	}
	return 0;
}
