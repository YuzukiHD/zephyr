/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Buffer allocator the decoders use for frames and bitstreams.
 *
 * The video engine addresses memory physically, and virtual equals physical on
 * this SoC, so the address translations are identity. The cache is not
 * coherent with the engine: whoever hands a buffer to or takes one from the
 * engine flushes it first.
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/sys/util.h>

#include "glue.h"

#include "glue_mem.h"

LOG_MODULE_DECLARE(vdec_sunxi_glue, CONFIG_VDEC_SUNXI_LOG_LEVEL);

static int mem_open(void)
{
	return 0;
}

static void mem_close(void)
{
}

static int mem_total_size(void)
{
	return 16; /* MiB */
}

static void *mem_palloc(int size, void *veops, void *self)
{
	size_t sz = ROUND_UP((size_t)size, CONFIG_DCACHE_LINE_SIZE);
	void *p = aligned_alloc(CONFIG_VDEC_SUNXI_MEM_ALIGN, sz);

	ARG_UNUSED(veops);
	ARG_UNUSED(self);

	if (p == NULL) {
		LOG_ERR("out of memory for %d bytes", size);
	}

	return p;
}

static void mem_pfree(void *p, void *veops, void *self)
{
	ARG_UNUSED(veops);
	ARG_UNUSED(self);

	free(p);
}

static void mem_flush_cache(void *p, int size)
{
	uintptr_t start = ROUND_DOWN((uintptr_t)p, CONFIG_DCACHE_LINE_SIZE);
	uintptr_t end = ROUND_UP((uintptr_t)p + size, CONFIG_DCACHE_LINE_SIZE);

	sys_cache_data_flush_and_invd_range((void *)start, end - start);
}

static void *mem_identity(void *p)
{
	return p;
}

static int mem_set(void *s, int c, size_t n)
{
	memset(s, c, n);
	return 0;
}

static int mem_copy(void *dst, void *src, size_t n)
{
	memcpy(dst, src, n);
	return 0;
}

static int mem_noop(void)
{
	return 0;
}

static unsigned int mem_ve_offset(void)
{
	return 0;
}

static struct ve_mem_ops mem_ops = {
	.open = mem_open,
	.close = mem_close,
	.total_size = mem_total_size,
	.palloc = mem_palloc,
	.palloc_no_cache = mem_palloc,
	.pfree = mem_pfree,
	.flush_cache = mem_flush_cache,
	.ve_get_phyaddr = mem_identity,
	.ve_get_viraddr = mem_identity,
	.cpu_get_phyaddr = mem_identity,
	.cpu_get_viraddr = mem_identity,
	.mem_set = mem_set,
	.mem_cpy = mem_copy,
	.mem_read = mem_copy,
	.mem_write = mem_copy,
	.setup = mem_noop,
	.shutdown = mem_noop,
	.get_ve_addr_offset = mem_ve_offset,
};

struct ve_mem_ops *ve_mem_get_ops(void)
{
	return &mem_ops;
}
