/*
 * Copyright (c) 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Fuse access of the Allwinner secure ID block
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_HWINFO_HWINFO_SUNXI_SID_H_
#define ZEPHYR_INCLUDE_DRIVERS_HWINFO_HWINFO_SUNXI_SID_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of fuse bits of the block */
#define SUNXI_SID_BITS 512

/**
 * @brief Read fuse bytes
 *
 * @param offset byte offset in the fuse array, a multiple of 4
 * @param buf    destination
 * @param len    bytes to read, a multiple of 4
 * @retval number of bytes read, negative errno otherwise
 */
ssize_t sunxi_sid_read(uint32_t offset, void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_HWINFO_HWINFO_SUNXI_SID_H_ */
