/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_LIB_OPENCORE_AAC_OSCL_MEM_H_
#define ZEPHYR_LIB_OPENCORE_AAC_OSCL_MEM_H_

#include <string.h>

#define oscl_memset  memset
#define oscl_memcpy  memcpy
#define oscl_memmove memmove
#define oscl_memcmp  memcmp

#endif /* ZEPHYR_LIB_OPENCORE_AAC_OSCL_MEM_H_ */
