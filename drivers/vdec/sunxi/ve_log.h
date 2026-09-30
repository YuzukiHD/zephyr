/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_LOG_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_LOG_H_

#include <zephyr/logging/log.h>

/* The module itself is registered once, in ve_core.c */
#define VE_LOG_DECLARE() LOG_MODULE_DECLARE(vdec_sunxi, CONFIG_VDEC_SUNXI_LOG_LEVEL)

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_LOG_H_ */
