/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Maps more anonymous memory than there are page frames for it, writes a
 * pattern in every page and reads it back: the pages are evicted to the
 * backing store and paged in again. Prints the paging statistics.
 */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/mm.h>
#include <zephyr/kernel/mm/demand_paging.h>
#include <zephyr/sys/printk.h>

#define PAGE		CONFIG_MMU_PAGE_SIZE
#define WORDS		(PAGE / sizeof(uint32_t))
/* page frames left for the memory that is paged */
#define ROOM_PAGES	24
#define TEST_PAGES	64

static uint32_t pattern(unsigned int page, unsigned int word)
{
	return (page * 0x9e3779b1U) ^ (word * 0x85ebca6bU) ^ 0xa5a5a5a5U;
}

static void stats(const char *what)
{
	struct k_mem_paging_stats_t s;

	k_mem_paging_stats_get(&s);
	printk("%s: %lu page faults, %lu clean and %lu dirty pages evicted\n", what,
	       s.pagefaults.cnt, s.eviction.clean, s.eviction.dirty);
}

static int check(const uint32_t *mem, unsigned int page)
{
	unsigned int bad = 0;

	/* a few words of every page: the first, the last and a scattering */
	for (unsigned int w = 0; w < WORDS; w += 97) {
		if (mem[page * WORDS + w] != pattern(page, w)) {
			bad++;
		}
	}
	if (mem[page * WORDS + WORDS - 1] != pattern(page, WORDS - 1)) {
		bad++;
	}

	return bad;
}

int main(void)
{
	size_t room = (size_t)ROOM_PAGES * PAGE;
	size_t free_bytes = k_mem_free_get();
	uint32_t *mem;
	unsigned int errors = 0;
	uint32_t t0;

	printk("free: %zu KiB, paged memory: %u pages, room for %u\n", free_bytes / 1024,
	       TEST_PAGES, ROOM_PAGES);

	/* use up the page frames, but for a few, with memory that stays */
	if (k_mem_map(free_bytes - room, K_MEM_PERM_RW | K_MEM_MAP_LOCK) == NULL) {
		printk("cannot pin the page frames\n");
		return 0;
	}
	printk("free now: %zu KiB\n", k_mem_free_get() / 1024);

	mem = k_mem_map((size_t)TEST_PAGES * PAGE, K_MEM_PERM_RW);
	if (mem == NULL) {
		printk("cannot map the paged memory\n");
		return 0;
	}
	printk("mapped at %p\n", mem);

	t0 = k_cycle_get_32();
	for (unsigned int p = 0; p < TEST_PAGES; p++) {
		for (unsigned int w = 0; w < WORDS; w += 97) {
			mem[p * WORDS + w] = pattern(p, w);
		}
		mem[p * WORDS + WORDS - 1] = pattern(p, WORDS - 1);
	}
	printk("written in %u us\n", (uint32_t)k_cyc_to_us_floor64(k_cycle_get_32() - t0));
	stats("after the writes");

	for (unsigned int p = 0; p < TEST_PAGES; p++) {
		errors += check(mem, p);
	}
	stats("after the first read");
	for (unsigned int p = TEST_PAGES; p-- > 0;) {
		errors += check(mem, p);
	}
	stats("after the second read");

	printk("%s: %u wrong words\n", errors == 0U ? "PASS" : "FAIL", errors);

	return 0;
}
