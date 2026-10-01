/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_I2S_SUNXI_H_
#define ZEPHYR_INCLUDE_DRIVERS_I2S_SUNXI_H_

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Playback progress of the on-chip codec
 *
 * @param dev the codec device
 * @param blocks number of I2S blocks played since the stream started
 * @param cycle k_cycle_get_32() at the moment the last of them finished
 *
 * A player derives its audio clock from it: blocks times the frames per block
 * plus the time since @p cycle gives the position in the stream.
 */
int i2s_sunxi_codec_tx_position(const struct device *dev, uint32_t *blocks, uint32_t *cycle);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_I2S_SUNXI_H_ */
