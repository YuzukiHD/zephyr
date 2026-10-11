/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Memory the video engine reads and writes: frames, bitstreams and working
 * tables. The engine addresses memory physically and virtual equals physical on
 * this SoC, so a pointer is also the bus address. The cache is not coherent with
 * the engine: whoever hands a buffer to the engine, or takes one it wrote,
 * flushes it first.
 */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_MEM_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_MEM_H_

#include <stddef.h>

/* Cache line aligned memory of at least @p size bytes, NULL when there is none */
void *ve_mem_alloc(size_t size);
void ve_mem_free(void *mem);

/* Write the cache back to memory and drop it, for a range handed to or taken from the engine */
void ve_mem_flush(const void *mem, size_t size);

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_MEM_H_ */
