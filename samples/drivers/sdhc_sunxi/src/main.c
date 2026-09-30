/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * SD card test for the sunxi SMHC driver: raw sector reads (aligned and
 * unaligned buffers), then a FAT mount with a write / read back / compare of
 * a pseudo random file. Nothing outside that file is written to the card.
 */

#include <ff.h>
#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/printk.h>

#define DISK_NAME	"SD"
#define MOUNT_PT	"/SD:"
#define TEST_FILE	MOUNT_PT "/F101TEST.BIN"
#define BUF_SIZE	(64 * 1024)
#define FILE_SIZE	(1024 * 1024)

static uint8_t buf[BUF_SIZE + 4] __aligned(64);
static uint8_t chk[BUF_SIZE] __aligned(64);
static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = MOUNT_PT,
};

static uint32_t prng(uint32_t *s)
{
	*s = *s * 1664525U + 1013904223U;
	return *s;
}

static void fill(uint8_t *p, size_t n, uint32_t *seed)
{
	for (size_t i = 0; i < n; i += 4) {
		uint32_t v = prng(seed);

		memcpy(p + i, &v, MIN(4U, n - i));
	}
}

static int raw_test(void)
{
	uint32_t sectors, ssize;
	int64_t t;
	int ret;

	ret = disk_access_init(DISK_NAME);
	if (ret) {
		printk("disk init failed: %d\n", ret);
		return ret;
	}
	disk_access_ioctl(DISK_NAME, DISK_IOCTL_GET_SECTOR_COUNT, &sectors);
	disk_access_ioctl(DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE, &ssize);
	printk("card: %u sectors x %u bytes = %u MiB\n", sectors, ssize,
	       (uint32_t)(((uint64_t)sectors * ssize) >> 20));

	ret = disk_access_read(DISK_NAME, buf, 0, 1);
	printk("sector 0 read: %d, signature %02x%02x\n", ret, buf[510], buf[511]);
	if (ret) {
		return ret;
	}

	/* aligned, multi block */
	t = k_uptime_get();
	for (int i = 0; i < 64; i++) {
		ret = disk_access_read(DISK_NAME, buf, i * (BUF_SIZE / ssize), BUF_SIZE / ssize);
		if (ret) {
			printk("aligned read %d failed: %d\n", i, ret);
			return ret;
		}
	}
	t = k_uptime_get() - t;
	printk("read 4 MiB: %lld ms (%u KiB/s)\n", t, t ? (uint32_t)(4096 * 1000 / t) : 0);

	/* unaligned destination, single and multi block */
	ret = disk_access_read(DISK_NAME, buf + 1, 0, 1);
	printk("unaligned 1 sector read: %d\n", ret);
	if (ret) {
		return ret;
	}
	ret = disk_access_read(DISK_NAME, buf + 2, 0, 8);
	printk("unaligned 8 sector read: %d\n", ret);

	return ret;
}

static int fs_test(void)
{
	struct fs_file_t f;
	struct fs_statvfs sv;
	struct fs_dir_t dir;
	struct fs_dirent ent;
	uint32_t seed;
	int64_t t;
	int ret;

	ret = fs_mount(&mp);
	if (ret) {
		printk("mount failed: %d\n", ret);
		return ret;
	}
	if (fs_statvfs(MOUNT_PT, &sv) == 0) {
		printk("fs: %lu x %lu free %lu\n", sv.f_bsize, sv.f_blocks, sv.f_bfree);
	}

	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, MOUNT_PT) == 0) {
		printk("root directory:\n");
		while (fs_readdir(&dir, &ent) == 0 && ent.name[0]) {
			printk("  %s %s %u\n", ent.type == FS_DIR_ENTRY_DIR ? "d" : "-", ent.name,
			       (uint32_t)ent.size);
		}
		fs_closedir(&dir);
	}

	fs_file_t_init(&f);
	ret = fs_open(&f, TEST_FILE, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret) {
		printk("create failed: %d\n", ret);
		goto umount;
	}
	seed = 1;
	t = k_uptime_get();
	for (size_t off = 0; off < FILE_SIZE; off += BUF_SIZE) {
		fill(buf, BUF_SIZE, &seed);
		ret = fs_write(&f, buf, BUF_SIZE);
		if (ret != BUF_SIZE) {
			printk("write failed: %d\n", ret);
			ret = -EIO;
			fs_close(&f);
			goto umount;
		}
	}
	fs_close(&f);
	t = k_uptime_get() - t;
	printk("wrote 1 MiB: %lld ms\n", t);

	ret = fs_open(&f, TEST_FILE, FS_O_READ);
	if (ret) {
		printk("open failed: %d\n", ret);
		goto umount;
	}
	seed = 1;
	t = k_uptime_get();
	for (size_t off = 0; off < FILE_SIZE; off += BUF_SIZE) {
		fill(buf, BUF_SIZE, &seed);
		ret = fs_read(&f, chk, BUF_SIZE);
		if (ret != BUF_SIZE || memcmp(buf, chk, BUF_SIZE) != 0) {
			printk("MISMATCH at %u (read %d)\n", (uint32_t)off, ret);
			fs_close(&f);
			ret = -EIO;
			goto umount;
		}
	}
	fs_close(&f);
	t = k_uptime_get() - t;
	printk("read back 1 MiB: %lld ms, data OK\n", t);
	ret = 0;

umount:
	fs_unlink(TEST_FILE);
	fs_unmount(&mp);
	return ret;
}

int main(void)
{
	int ret = raw_test();

	if (ret == 0) {
		ret = fs_test();
	}
	printk("SD test done: %s (%d)\n", ret ? "FAIL" : "PASS", ret);

	return 0;
}
