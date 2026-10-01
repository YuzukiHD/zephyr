/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The few types and macros the AAC decoder takes from the OpenCORE base library */

#ifndef ZEPHYR_LIB_OPENCORE_AAC_OSCL_BASE_H_
#define ZEPHYR_LIB_OPENCORE_AAC_OSCL_BASE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OSCL_EXPORT_REF
#define OSCL_IMPORT_REF
#define OSCL_UNUSED_ARG(x) (void)(x)

typedef int8_t int8;
typedef uint8_t uint8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef int32_t int32;
typedef uint32_t uint32;
typedef int64_t int64;
typedef uint64_t uint64;
typedef unsigned int uint;

/* the code casts to the enum by its tag, a C++ habit */
#define eMP4AudioObjectType tMP4AudioObjectType

/*
 * The bit reader functions are plain "__inline" in the headers: in C that leaves
 * no out-of-line copy for the calls the compiler does not inline.
 */
#define __inline static __inline__

#endif /* ZEPHYR_LIB_OPENCORE_AAC_OSCL_BASE_H_ */
