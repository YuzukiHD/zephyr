/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Extensions of the Allwinner SPIF flash controller driver
 *
 * Besides the flash API the driver can tune the sample point of the read data
 * (needed above a few ten MHz where the return path of the clock and data
 * lines is longer than a clock period), keep the tuned sample point in a flash
 * partition, and map a part of the flash into the address space (XIP) so that
 * data can be read and code executed in place.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_FLASH_FLASH_SUNXI_SPIF_H_
#define ZEPHYR_INCLUDE_DRIVERS_FLASH_FLASH_SUNXI_SPIF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name Placement in the XIP window
 *
 * With CONFIG_FLASH_SUNXI_SPIF_XIP_SECTIONS a function marked @c __xip_text
 * and a constant marked @c __xip_rodata are linked into the memory window of
 * the flash and run and are read from there.
 * @{
 */
#define __xip_text	__attribute__((section(".xip_text")))
#define __xip_rodata	__attribute__((section(".xip_rodata")))
/** @} */

/** Number of sample modes tried by the tuning */
#define SUNXI_SPIF_TUNE_MODES	3
/** Number of sample delay steps tried per mode */
#define SUNXI_SPIF_TUNE_DELAYS	64

/** What the driver knows about the controller and the flash */
struct sunxi_spif_info {
	uint32_t version;	/**< controller version register */
	uint8_t jedec_id[3];
	uint32_t size;		/**< flash capacity in bytes */
	uint32_t frequency;	/**< SCK in Hz in use */
	bool quad;		/**< four wire read/program in use */
	bool dtr;		/**< reads use double data rate (1-4-4 DTR) */
	bool addr_4byte;
	bool sample_tuned;	/**< a sample point is applied (tuned or loaded) */
	uint8_t sample_mode;
	uint8_t sample_delay;
	bool xip_active;
	uint32_t xip_flash_offset;
	uint32_t xip_length;
};

/** Result of a tuning run */
struct sunxi_spif_tune_result {
	/** the frequency the result is for (the last one tried when tuning failed) */
	uint32_t frequency;
	uint8_t mode;		/**< chosen sample mode */
	uint8_t delay;		/**< chosen sample delay (middle of the window) */
	uint8_t window_start;	/**< first delay of the passing window */
	uint8_t window_len;	/**< width of the passing window in delay steps */
	bool dtr;		/**< the point is for DTR reads */
	/** bit n of ok[mode]: delay n reads the ID and the reference data correctly */
	uint64_t ok[SUNXI_SPIF_TUNE_MODES];
	/** the same for the ID alone (one wire command), at the last frequency tried */
	uint64_t ok_one_wire[SUNXI_SPIF_TUNE_MODES];
};

/** @brief Get the identification and the current operating point */
int sunxi_spif_get_info(const struct device *dev, struct sunxi_spif_info *info);

/**
 * @brief Search the sample point
 *
 * Reads the first 4 KiB of the tune partition (default: start of the flash)
 * at 24 MHz as reference, then at the configured frequency with every
 * combination of sample mode and delay. A point passes when the JEDEC ID (a
 * one wire command, like the status polling) and the reference data (read
 * with the wires in use) both come back right. The middle of the widest
 * passing window is taken. When no window of at least
 * CONFIG_FLASH_SUNXI_SPIF_TUNE_MIN_WINDOW steps exists the next lower
 * frequency is tried. The reference data must not be constant.
 *
 * On success the sample point is applied and the frequency it is good for
 * (possibly lower than the configured one) is in use. On failure the
 * controller falls back to 24 MHz without a sample delay.
 *
 * With DTR reads enabled, the DTR points are searched first; when none is
 * found at any frequency, DTR is dropped and the search is repeated for SDR.
 *
 * @retval 0 a window was found
 * @retval -ENODATA the reference data is blank or constant
 * @retval -EIO no sample point reads the data correctly
 */
int sunxi_spif_tune(const struct device *dev, struct sunxi_spif_tune_result *res);

/**
 * @brief Apply a sample point by hand at the operating frequency
 *
 * Like @ref sunxi_spif_tune and @ref sunxi_spif_params_load this changes the
 * clock and the sampling and returns -EBUSY while the XIP window is mapped.
 */
int sunxi_spif_set_sample(const struct device *dev, uint8_t mode, uint8_t delay);

/**
 * @brief Store the applied sample point in the params partition
 *
 * Records are appended to the partition; it is erased when full.
 *
 * @retval -ENOENT the node has no params partition
 * @retval -EAGAIN no sample point has been applied yet
 */
int sunxi_spif_params_save(const struct device *dev);

/**
 * @brief Apply the sample point stored in the params partition
 *
 * @retval -ENOENT no params partition, or no valid record for this flash
 *                 and frequency
 */
int sunxi_spif_params_load(const struct device *dev);

/** @brief Erase the stored sample point */
int sunxi_spif_params_erase(const struct device *dev);

/**
 * @brief Map flash into the address space
 *
 * After this, the bytes of the flash starting at @p flash_offset can be read
 * (and code run) at the window address, see @ref sunxi_spif_xip_window. Flash
 * writes and erases keep working: the mapping is suspended around each
 * command, with interrupts masked, and restored afterwards. Code that runs
 * from the window must not be running on another context at that time, and
 * the driver code itself must not live in the window.
 *
 * @param flash_offset offset in the flash, aligned to 4 KiB
 * @param length bytes to map (at most the window size, 32 MiB)
 */
int sunxi_spif_xip_enable(const struct device *dev, uint32_t flash_offset, size_t length);

/** @brief Remove the mapping */
int sunxi_spif_xip_disable(const struct device *dev);

/** @brief Address in the window of the first mapped byte */
const void *sunxi_spif_xip_window(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_FLASH_FLASH_SUNXI_SPIF_H_ */
