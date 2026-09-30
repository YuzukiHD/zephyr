/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_GLUE_GLUE_MEM_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_GLUE_GLUE_MEM_H_

#include <stddef.h>

/* Layout fixed by the archive: the order of the members is part of its interface */
struct ve_mem_ops {
	int (*open)(void);
	void (*close)(void);
	int (*total_size)(void);
	void *(*palloc)(int size, void *veops, void *self);
	void *(*palloc_no_cache)(int size, void *veops, void *self);
	void (*pfree)(void *mem, void *veops, void *self);
	void (*flush_cache)(void *mem, int size);
	void *(*ve_get_phyaddr)(void *virt);
	void *(*ve_get_viraddr)(void *phys);
	void *(*cpu_get_phyaddr)(void *virt);
	void *(*cpu_get_viraddr)(void *phys);
	int (*mem_set)(void *s, int c, size_t n);
	int (*mem_cpy)(void *dst, void *src, size_t n);
	int (*mem_read)(void *dst, void *src, size_t n);
	int (*mem_write)(void *dst, void *src, size_t n);
	int (*setup)(void);
	int (*shutdown)(void);
	unsigned int (*get_ve_addr_offset)(void);
};

struct ve_mem_ops *ve_mem_get_ops(void);

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_GLUE_GLUE_MEM_H_ */
