/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Extensions of the Allwinner sunxi SPI controller driver
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SPI_SPI_SUNXI_H_
#define ZEPHYR_INCLUDE_DRIVERS_SPI_SPI_SUNXI_H_

#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Sample modes: the sampling of the data is delayed by n half cycles */
#define SUNXI_SPI_SAMPLE_MODES	7
/** Steps of the sample delay line */
#define SUNXI_SPI_SAMPLE_DELAYS	64

/**
 * @brief Choose the sample point
 *
 * Above 60 MHz the returning data needs a delayed sampling. Without this call
 * the driver samples one cycle late there. @p mode is the number of half
 * cycles (0..6), @p delay the step of the delay line (0..63) added on top.
 * Applied from the next transfer on.
 */
int sunxi_spi_set_sample(const struct device *dev, uint8_t mode, uint8_t delay);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_SPI_SPI_SUNXI_H_ */
