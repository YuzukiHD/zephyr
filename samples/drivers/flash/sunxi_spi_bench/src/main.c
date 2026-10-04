/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Reads the EVB flash through the plain SPI controller (spi0) to compare with
 * the SPIF driver (sample sunxi_spif): one wire at 24 MHz, then four wires at
 * 100 MHz after a search of the sample point. The pattern at 0x600000 was
 * written with xfel (256 KiB, crc32 990a2aef).
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi/spi_sunxi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#define OFFSET	0x600000U
#define TOTAL	(256U * 1024U)
#define EXPECT_CRC	0x990a2aefU

static const struct device *const spi = DEVICE_DT_GET(DT_NODELABEL(spi0));
static uint8_t buf[TOTAL] __aligned(64);
static uint8_t ref[4096];

/* fast read (0x0b) on one wire, or quad output read (0x6b, QE set in the flash) */
static int read_flash(uint32_t hz, bool quad, uint32_t off, uint8_t *dst, uint32_t len,
		      uint32_t chunk)
{
	struct spi_config cfg = {
		.frequency = hz,
		.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB |
			     (quad ? SPI_LINES_QUAD : SPI_LINES_SINGLE),
	};

	for (uint32_t o = 0; o < len; o += chunk) {
		uint32_t a = off + o;
		uint8_t cmd[5] = { quad ? 0x6b : 0x0b, a >> 16, a >> 8, a, 0 };
		struct spi_buf tx = { .buf = cmd, .len = sizeof(cmd) };
		struct spi_buf_set txs = { .buffers = &tx, .count = 1 };
		struct spi_buf rx = { .buf = dst + o, .len = MIN(chunk, len - o) };
		struct spi_buf_set rxs = { .buffers = &rx, .count = 1 };
		int ret = spi_transceive(spi, &cfg, &txs, &rxs);

		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static void bench(const char *name, uint32_t hz, bool quad, uint32_t chunk)
{
	uint32_t t0 = k_cycle_get_32(), us;
	int ret = read_flash(hz, quad, OFFSET, buf, TOTAL, chunk);
	uint32_t crc;

	us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
	crc = crc32_ieee(buf, TOTAL);
	printk("%s %u Hz, %u byte reads: %d, %u us, %u KiB/s, crc %08x %s\n", name, hz, chunk, ret,
	       us, (uint32_t)((uint64_t)TOTAL / 1024U * 1000000U / us), crc,
	       crc == EXPECT_CRC ? "ok" : "WRONG");
}

int main(void)
{
	uint32_t best_len = 0, best_mode = 0, best_start = 0;

	if (!device_is_ready(spi)) {
		printk("spi0 not ready\n");
		return 0;
	}
	bench("one wire", 24000000, false, 4096);
	bench("one wire", 24000000, false, 65536);

	/* the reference for the search */
	read_flash(24000000, false, OFFSET, ref, sizeof(ref), sizeof(ref));
	printk("reference crc %08x\n", crc32_ieee(ref, sizeof(ref)));

	for (uint32_t mode = 0; mode < SUNXI_SPI_SAMPLE_MODES; mode++) {
		char line[SUNXI_SPI_SAMPLE_DELAYS + 1];
		uint32_t run = 0;

		for (uint32_t d = 0; d < SUNXI_SPI_SAMPLE_DELAYS; d++) {
			uint8_t tmp[sizeof(ref)];
			bool ok;

			sunxi_spi_set_sample(spi, mode, d);
			memset(tmp, 0, sizeof(tmp));
			ok = read_flash(100000000, true, OFFSET, tmp, sizeof(tmp), sizeof(tmp)) == 0 &&
			     memcmp(tmp, ref, sizeof(ref)) == 0;
			line[d] = ok ? '#' : '.';
			run = ok ? run + 1 : 0;
			if (run > best_len) {
				best_len = run;
				best_mode = mode;
				best_start = d + 1 - run;
			}
		}
		line[SUNXI_SPI_SAMPLE_DELAYS] = '\0';
		printk("  mode %u %s\n", mode, line);
	}
	printk("best: mode %u delay %u window %u\n", best_mode, best_start + best_len / 2, best_len);
	if (best_len == 0U) {
		return 0;
	}
	sunxi_spi_set_sample(spi, best_mode, best_start + best_len / 2);
	bench("four wires", 100000000, true, 4096);
	bench("four wires", 100000000, true, 65536);

	return 0;
}
