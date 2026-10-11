/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <zephyr/cache.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "ve_log.h"
#include "ve_mem.h"

VE_LOG_DECLARE();

void *ve_mem_alloc(size_t size)
{
	void *mem = aligned_alloc(CONFIG_VDEC_SUNXI_MEM_ALIGN,
				  ROUND_UP(size, CONFIG_DCACHE_LINE_SIZE));

	if (mem == NULL) {
		LOG_ERR("out of memory for %zu bytes", size);
	}
	if (IS_ENABLED(CONFIG_VDEC_SUNXI_MEM_TRACE)) {
		static size_t total;

		total += size;
		printk("ALLOC %zu -> %p (total %zu)\n", size, mem, total);
	}

	return mem;
}

void ve_mem_free(void *mem)
{
	if (IS_ENABLED(CONFIG_VDEC_SUNXI_MEM_TRACE)) {
		printk("FREE %p\n", mem);
	}
	free(mem);
}

void ve_mem_flush(const void *mem, size_t size)
{
	uintptr_t start = ROUND_DOWN((uintptr_t)mem, CONFIG_DCACHE_LINE_SIZE);
	uintptr_t end = ROUND_UP((uintptr_t)mem + size, CONFIG_DCACHE_LINE_SIZE);

	sys_cache_data_flush_and_invd_range((void *)start, end - start);
}
