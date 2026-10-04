/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Maps a file from the SD card, reads it through the mapping and with the file
 * system and compares the two, then touches pages at random.
 */

#include <ff.h>
#include <stdlib.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/mm.h>
#include <zephyr/kernel/mm/backing_store_fs.h>
#include <zephyr/kernel/mm/demand_paging.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#define CHUNK		(64 * 1024)

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/SD:",
};

static uint32_t us_since(uint32_t c0)
{
	return (uint32_t)k_cyc_to_us_floor64(k_cycle_get_32() - c0);
}

static void stats(const char *what)
{
	struct k_mem_paging_stats_t s;

	k_mem_paging_stats_get(&s);
	printk("%s: %lu page faults, %lu clean pages dropped\n", what, s.pagefaults.cnt,
	       s.eviction.clean);
}

int main(void)
{
	struct fs_file_t f;
	uint8_t *copy;
	const uint8_t *map;
	size_t size = 0;
	uint32_t crc_map = 0, crc_fs = 0, t0;
	uint32_t seed = 1, total = 0;
	int ret;

	ret = fs_mount(&mp);
	if (ret != 0) {
		printk("cannot mount the card: %d\n", ret);
		return 0;
	}
	printk("free page frames: %zu KiB\n", k_mem_free_get() / 1024);

	map = k_mem_paging_map_file(CONFIG_SAMPLE_PAGING_FILE, &size, 0);
	if (map == NULL) {
		printk("cannot map %s\n", CONFIG_SAMPLE_PAGING_FILE);
		return 0;
	}
	printk("%s: %zu bytes mapped at %p\n", CONFIG_SAMPLE_PAGING_FILE, size, map);

	/* the whole file through the mapping */
	t0 = k_cycle_get_32();
	for (size_t off = 0; off < size; off += CHUNK) {
		crc_map = crc32_ieee_update(crc_map, map + off, MIN(CHUNK, size - off));
	}
	printk("through the mapping: crc %08x, %u ms\n", crc_map, us_since(t0) / 1000);
	stats("after the sequential read");

	/* the same with the file system */
	copy = aligned_alloc(64, CHUNK);
	fs_file_t_init(&f);
	if (copy == NULL || fs_open(&f, CONFIG_SAMPLE_PAGING_FILE, FS_O_READ) != 0) {
		printk("cannot read the file\n");
		return 0;
	}
	t0 = k_cycle_get_32();
	for (size_t off = 0; off < size; off += CHUNK) {
		ssize_t n = fs_read(&f, copy, CHUNK);

		if (n <= 0) {
			break;
		}
		crc_fs = crc32_ieee_update(crc_fs, copy, n);
	}
	fs_close(&f);
	printk("with the file system: crc %08x, %u ms\n", crc_fs, us_since(t0) / 1000);

	/* pages at random: the cost of one that is not there */
	t0 = k_cycle_get_32();
	for (int i = 0; i < 512; i++) {
		seed = seed * 1103515245U + 12345U;
		total += map[(((seed >> 8) % (size / 4096)) * 4096) + 17];
	}
	printk("512 random pages: %u us each (sum %u)\n", us_since(t0) / 512, total);
	stats("at the end");

	printk("%s\n", crc_map == crc_fs ? "PASS" : "FAIL");

	return 0;
}
