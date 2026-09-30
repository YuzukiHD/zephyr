/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Bring up the video engine through the archive and print what it reports */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "ve_abi.h"
#include "glue_mem.h"


int main(void)
{
	struct ve_ops *ops = GetVeOpsS(0);
	struct ve_config cfg = { .decoder_flag = 1, .memops = ve_mem_get_ops(), .ve_freq = 300 };
	void *self;

	printk("ops %p\n", ops);
	if (ops == NULL) {
		return 0;
	}
	self = ops->init(&cfg);
	printk("init -> %p\n", self);
	if (self == NULL) {
		return 0;
	}
	printk("lock %d\n", ops->lock(self));
	printk("ve version %08x%08x\n", (uint32_t)(ops->get_ic_ve_version(self) >> 32),
	       (uint32_t)ops->get_ic_ve_version(self));
	for (int g = 0; g < 8; g++) {
		printk("group %d at %p\n", g, ops->get_group_reg_addr(self, g));
	}
	ops->unlock(self);
	ops->release(self);
	printk("probe done\n");

	return 0;
}
