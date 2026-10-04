/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

extern char __rom_region_start[], __rom_region_end[];

static const char rodata_string[] = "rodata is in the flash window too";
static int counter = 41;	/* .data: copied from the flash to RAM at boot */

int main(void)
{
	printk("XIP: main at %p, image %p..%p, rodata %p, data %p, stack %p\n", (void *)main,
	       __rom_region_start, __rom_region_end, rodata_string, &counter, &(int){ 0 });
	counter++;
	printk("XIP: %s, counter %d (expect 42)\n", rodata_string, counter);
	for (int i = 0; i < 3; i++) {
		printk("XIP: tick %d at %lld ms\n", i, k_uptime_get());
		k_sleep(K_MSEC(200));
	}
	printk("XIP: done\n");

	return 0;
}
