/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Data cache maintenance for the buffers and descriptors the controllers read and write */

#include <zephyr/cache.h>

#include "usb_config.h"

#ifdef CONFIG_USB_DCACHE_ENABLE
void usb_dcache_clean(uintptr_t addr, size_t size)
{
	sys_cache_data_flush_range((void *)addr, size);
}

void usb_dcache_invalidate(uintptr_t addr, size_t size)
{
	sys_cache_data_invd_range((void *)addr, size);
}

void usb_dcache_flush(uintptr_t addr, size_t size)
{
	sys_cache_data_flush_and_invd_range((void *)addr, size);
}
#endif
