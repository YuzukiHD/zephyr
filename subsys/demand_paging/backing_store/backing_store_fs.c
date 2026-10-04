/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * A read-only backing store on a file of the file system. The location of a
 * page is its offset in the file; a page frame that holds a page of the file
 * is clean for good, so the kernel drops it when it needs the frame and never
 * asks to write it.
 *
 * The page is read into a buffer of the image and copied to the scratch page:
 * the buffer is mapped 1:1 and aligned, which a driver doing DMA into it needs,
 * the scratch page is not. The paging operations are serialized by the kernel,
 * so one buffer will do. It reads with the file system, which sleeps while the
 * card works: this needs CONFIG_DEMAND_PAGING_BACKING_STORE_SLEEPS.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/mm/backing_store_fs.h>
#include <zephyr/kernel/mm/demand_paging.h>
#include <zephyr/logging/log.h>
#include <kernel_arch_interface.h>
#include <mmu.h>

LOG_MODULE_REGISTER(backing_store_fs, CONFIG_KERNEL_LOG_LEVEL);

#define PAGE CONFIG_MMU_PAGE_SIZE

static struct fs_file_t file;
static bool opened;
static void *base;
static size_t mapped;
/* where the next read of the file starts, to skip the seek of a sequential read */
static off_t file_pos = -1;
static uint8_t buffer[PAGE] __aligned(64);

int k_mem_paging_backing_store_location_get(struct k_mem_page_frame *pf, uintptr_t *location,
					    bool page_fault)
{
	if (k_mem_page_frame_is_backed(pf)) {
		return k_mem_paging_backing_store_location_query(k_mem_page_frame_to_virt(pf),
								  location);
	}

	/* a read-only store: nothing else can be paged out */
	return -ENOMEM;
}

void k_mem_paging_backing_store_location_free(uintptr_t location)
{
}

void k_mem_paging_backing_store_page_out(uintptr_t location)
{
	/* the file is never written: a page that was changed through a writable mapping and
	 * is not pinned loses its changes
	 */
	LOG_WRN("modified page at %#lx dropped", location);
}

void k_mem_paging_backing_store_page_in(uintptr_t location)
{
	ssize_t n;

	if ((off_t)location != file_pos) {
		if (fs_seek(&file, (off_t)location, FS_SEEK_SET) != 0) {
			LOG_ERR("seek to %#lx failed", location);
			k_panic();
		}
	}
	n = fs_read(&file, buffer, PAGE);
	if (n < 0) {
		LOG_ERR("read at %#lx failed: %d", location, (int)n);
		k_panic();
	}
	file_pos = (off_t)location + n;
	/* the last page of the file */
	if ((size_t)n < PAGE) {
		memset(buffer + n, 0, PAGE - n);
	}
	memcpy(K_MEM_SCRATCH_PAGE, buffer, PAGE);
}

void k_mem_paging_backing_store_page_finalize(struct k_mem_page_frame *pf, uintptr_t location)
{
	k_mem_page_frame_set(pf, K_MEM_PAGE_FRAME_BACKED);
}

int k_mem_paging_backing_store_location_query(void *addr, uintptr_t *location)
{
	if (!opened || (uintptr_t)addr < (uintptr_t)base ||
	    (uintptr_t)addr >= (uintptr_t)base + mapped) {
		return -EFAULT;
	}
	*location = (uintptr_t)addr - (uintptr_t)base;

	return 0;
}

void k_mem_paging_backing_store_init(void)
{
	fs_file_t_init(&file);
}

void *k_mem_paging_map_file(const char *path, size_t *size, uint32_t flags)
{
	struct fs_dirent st;
	void *addr;

	if (opened) {
		return NULL;
	}
	if (fs_stat(path, &st) != 0 || st.size == 0U) {
		return NULL;
	}
	if (fs_open(&file, path, FS_O_READ) != 0) {
		return NULL;
	}
	mapped = ROUND_UP(st.size, PAGE);
	opened = true;
	file_pos = 0;

	/* the location of the first page is 0 and goes up by a page for each page; the
	 * content comes from the file, so the pages must not be cleared
	 */
	addr = k_mem_map_unpaged(0, mapped, K_MEM_MAP_UNINIT | flags);
	if (addr == NULL) {
		fs_close(&file);
		opened = false;
		return NULL;
	}
	base = addr;
	*size = st.size;

	return addr;
}

void k_mem_paging_unmap_file(void *addr)
{
	if (!opened || addr != base) {
		return;
	}
	/* the frames stay with the page frame database until they are needed */
	k_mem_unmap(base, mapped);
	fs_close(&file);
	opened = false;
	base = NULL;
}
