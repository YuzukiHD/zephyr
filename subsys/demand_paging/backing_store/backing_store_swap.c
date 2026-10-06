/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A backing store for one region of anonymous memory on a swap file of the file system. The
 * location of a page is its offset in the region, which is also its offset in the file: no
 * space has to be managed and a page that has been written out keeps its place, so after it is
 * read back the page frame is marked as backed and the kernel drops it for free as long as it
 * is not written to again.
 *
 * Pages go through a buffer of the image (mapped 1:1 and aligned, the scratch page is not).
 * The paging operations are serialized by the kernel, so one buffer is enough. The file
 * system sleeps while the card works: CONFIG_DEMAND_PAGING_BACKING_STORE_SLEEPS.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/mm/backing_store_swap.h>
#include <zephyr/kernel/mm/demand_paging.h>
#include <zephyr/logging/log.h>
#include <kernel_arch_interface.h>
#include <mmu.h>

LOG_MODULE_REGISTER(backing_store_swap, CONFIG_KERNEL_LOG_LEVEL);

#define PAGE CONFIG_MMU_PAGE_SIZE

static struct fs_file_t file;
static char swap_path[96];
static bool opened;
static uint8_t *base;
static size_t region_size;
/* where the next transfer of the file starts, to skip a seek in sequential access */
static off_t file_pos = -1;
/* bytes of the file: the file system does not seek beyond its end */
static off_t file_size;
static uint8_t buffer[PAGE] __aligned(64);

static bool in_region(const void *addr)
{
	return opened && (const uint8_t *)addr >= base && (const uint8_t *)addr < base + region_size;
}

int k_mem_paging_backing_store_location_get(struct k_mem_page_frame *pf, uintptr_t *location,
					    bool page_fault)
{
	const void *virt = k_mem_page_frame_to_virt(pf);

	if (!in_region(virt)) {
		/* nothing else can be paged out */
		return -ENOMEM;
	}
	*location = (uintptr_t)((const uint8_t *)virt - base);

	return 0;
}

void k_mem_paging_backing_store_location_free(uintptr_t location)
{
}

static void seek_to(off_t pos)
{
	if (pos != file_pos && fs_seek(&file, pos, FS_SEEK_SET) != 0) {
		LOG_ERR("seek to %#lx failed", (unsigned long)pos);
		k_panic();
	}
}

void k_mem_paging_backing_store_page_out(uintptr_t location)
{
	ssize_t n;

	/* pages are mostly written out in the order they were first used; a page that comes before
	 * the end of the file is written in place, one behind it leaves a gap that is filled with
	 * zeros
	 */
	if ((off_t)location > file_size) {
		memset(buffer, 0, PAGE);
		seek_to(file_size);
		while (file_size < (off_t)location) {
			n = fs_write(&file, buffer, PAGE);
			if (n != PAGE) {
				LOG_ERR("write at %#lx failed: %d", (unsigned long)file_size, (int)n);
				k_panic();
			}
			file_size += PAGE;
		}
		file_pos = file_size;
	}
	memcpy(buffer, K_MEM_SCRATCH_PAGE, PAGE);
	seek_to((off_t)location);
	n = fs_write(&file, buffer, PAGE);
	if (n != PAGE) {
		LOG_ERR("write at %#lx failed: %d", (unsigned long)location, (int)n);
		k_panic();
	}
	file_pos = (off_t)location + PAGE;
	if (file_pos > file_size) {
		file_size = file_pos;
	}
}

void k_mem_paging_backing_store_page_in(uintptr_t location)
{
	ssize_t n;

	seek_to((off_t)location);
	n = fs_read(&file, buffer, PAGE);
	if (n != PAGE) {
		LOG_ERR("read at %#lx failed: %d", (unsigned long)location, (int)n);
		k_panic();
	}
	file_pos = (off_t)location + PAGE;
	memcpy(K_MEM_SCRATCH_PAGE, buffer, PAGE);
}

void k_mem_paging_backing_store_page_finalize(struct k_mem_page_frame *pf, uintptr_t location)
{
	/* a page that was just created as zero is not in the file */
	if (location == ARCH_UNPAGED_ANON_ZERO || location == ARCH_UNPAGED_ANON_UNINIT) {
		return;
	}
	k_mem_page_frame_set(pf, K_MEM_PAGE_FRAME_BACKED);
}

int k_mem_paging_backing_store_location_query(void *addr, uintptr_t *location)
{
	if (!in_region(addr)) {
		return -EFAULT;
	}
	*location = (uintptr_t)((uint8_t *)addr - base);

	return 0;
}

void k_mem_paging_backing_store_init(void)
{
	fs_file_t_init(&file);
}

void *k_mem_paging_swap_map(const char *path, size_t size)
{
	void *addr;

	if (opened || size == 0U || (size % PAGE) != 0U) {
		return NULL;
	}
	/* start from an empty file */
	(void)fs_unlink(path);
	if (fs_open(&file, path, FS_O_CREATE | FS_O_RDWR) != 0) {
		return NULL;
	}
	strncpy(swap_path, path, sizeof(swap_path) - 1);
	/* the file grows with the pages that are written out */
	file_size = 0;
	file_pos = -1;

	/* the pages are zero until they are written out; opened is set after the mapping: the
	 * guard pages and the pages themselves are not touched by it
	 */
	addr = k_mem_map(size, K_MEM_PERM_RW);
	if (addr == NULL) {
		fs_close(&file);
		fs_unlink(path);
		return NULL;
	}
	base = addr;
	region_size = size;
	opened = true;

	return addr;
}

void k_mem_paging_swap_unmap(void *addr)
{
	if (!opened || addr != base) {
		return;
	}
	opened = false;
	k_mem_unmap(base, region_size);
	fs_close(&file);
	fs_unlink(swap_path);
	base = NULL;
}
