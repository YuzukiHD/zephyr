/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_KERNEL_MM_BACKING_STORE_SWAP_H_
#define ZEPHYR_INCLUDE_KERNEL_MM_BACKING_STORE_SWAP_H_

#include <stddef.h>

/**
 * @file
 * @brief Anonymous memory that is paged out to a file
 *
 * With CONFIG_BACKING_STORE_SWAP a region of memory bigger than the page frames that are
 * left can be mapped. A page is zero when it is first touched, goes to the swap file when its
 * frame is needed and was changed since it was read, and is dropped when it was not (the file
 * still has it). The page at offset X of the region is at offset X of the file, so the file is
 * as large as the highest page that was ever written out (a page behind the end of the file
 * leaves a gap that is filled with zeros).
 *
 * One region at a time. The file system has to be mounted. The region must not be handed to a
 * driver as a DMA buffer and no code that holds the lock of the file system may touch it
 * (copy through a buffer outside of the region instead).
 */

/**
 * @brief Create the swap file and map the region
 *
 * @param path File to use, an existing one is truncated
 * @param size Size of the region in bytes (a multiple of the page size)
 *
 * @return The region, NULL on failure
 */
void *k_mem_paging_swap_map(const char *path, size_t size);

/** @brief Unmap the region, close and delete the swap file */
void k_mem_paging_swap_unmap(void *addr);

#endif /* ZEPHYR_INCLUDE_KERNEL_MM_BACKING_STORE_SWAP_H_ */
