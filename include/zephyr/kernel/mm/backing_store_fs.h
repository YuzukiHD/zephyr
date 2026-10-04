/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_KERNEL_MM_BACKING_STORE_FS_H_
#define ZEPHYR_INCLUDE_KERNEL_MM_BACKING_STORE_FS_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @file
 * @brief Read-only memory mappings of files, paged in on demand
 *
 * With CONFIG_BACKING_STORE_FS the pages of a file are brought in from the
 * file system when the mapped memory is first touched, and dropped again when
 * the page frame is needed for something else: the file is never written
 * back. A mapping is read-only unless K_MEM_PERM_RW is given; the changes of a
 * writable mapping stay only while the page is resident (pin it with k_mem_pin()
 * to keep them).
 *
 * One file at a time. Memory from k_mem_map() that is not pinned (without
 * K_MEM_MAP_LOCK) cannot be paged out by this store, so it has to be pinned.
 * The mapped memory must not be handed to a driver as a DMA buffer and no
 * code that holds the lock of the file system may touch it.
 */

/**
 * @brief Map a file
 *
 * @param path File to map
 * @param[out] size Size of the file in bytes, the mapping is the same rounded up to a page
 * @param flags 0 or K_MEM_PERM_RW
 *
 * @return The address of the first byte of the file, NULL if the file cannot be opened or
 *         mapped
 */
void *k_mem_paging_map_file(const char *path, size_t *size, uint32_t flags);

/** @brief Unmap the file and close it */
void k_mem_paging_unmap_file(void *addr);

#endif /* ZEPHYR_INCLUDE_KERNEL_MM_BACKING_STORE_FS_H_ */
