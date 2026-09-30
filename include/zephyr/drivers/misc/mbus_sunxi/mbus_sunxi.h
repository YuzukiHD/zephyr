/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

/**
 * @file
 * @brief Allwinner sunxi memory bus (MBUS): arbitration and bandwidth monitor
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_MBUS_SUNXI_MBUS_SUNXI_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_MBUS_SUNXI_MBUS_SUNXI_H_

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bandwidth counters of the MBUS performance monitor (PMU) */
enum sunxi_mbus_pmu {
	SUNXI_MBUS_PMU_CPU,
	SUNXI_MBUS_PMU_RV_SYS,
	SUNXI_MBUS_PMU_MAHB,
	SUNXI_MBUS_PMU_DMA,
	SUNXI_MBUS_PMU_VE,
	SUNXI_MBUS_PMU_CE,
	SUNXI_MBUS_PMU_TVD,
	SUNXI_MBUS_PMU_CSI,
	SUNXI_MBUS_PMU_DSP_SYS,
	SUNXI_MBUS_PMU_G2D,
	SUNXI_MBUS_PMU_DI,
	SUNXI_MBUS_PMU_DE,
	SUNXI_MBUS_PMU_IOMMU,
	SUNXI_MBUS_PMU_RESERVED,
	SUNXI_MBUS_PMU_OTHER,
	SUNXI_MBUS_PMU_TOTAL,
	SUNXI_MBUS_PMU_COUNT,
};

/** Name of a PMU counter, for logging */
const char *sunxi_mbus_pmu_name(enum sunxi_mbus_pmu counter);

/** Number of arbitrated masters (valid master numbers are 0..count-1) */
#define SUNXI_MBUS_MASTER_COUNT 40

/**
 * @brief Set the priority of a master (0..3, 3 is the highest).
 */
int sunxi_mbus_set_priority(const struct device *dev, uint32_t master, uint32_t priority);

/**
 * @brief Get the priority of a master.
 */
int sunxi_mbus_get_priority(const struct device *dev, uint32_t master, uint32_t *priority);

/**
 * @brief Limit the bandwidth of a master to @p mbps MB/s (0 removes the limit).
 */
int sunxi_mbus_set_limit(const struct device *dev, uint32_t master, uint32_t mbps);

/**
 * @brief Get the bandwidth limit of a master in MB/s (0 when unlimited).
 */
int sunxi_mbus_get_limit(const struct device *dev, uint32_t master, uint32_t *mbps);

/**
 * @brief Bytes a master moved since the monitor was started.
 *
 * The counters are free running 32 bit byte counts that wrap around; take
 * two readings and subtract them (unsigned) to get the traffic in between.
 */
int sunxi_mbus_get_traffic(const struct device *dev, enum sunxi_mbus_pmu counter,
			   uint32_t *bytes);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_MBUS_SUNXI_MBUS_SUNXI_H_ */
