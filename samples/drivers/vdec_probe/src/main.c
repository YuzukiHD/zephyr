/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Bring up the video engine through the archive and print what it reports */

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "glue_mem.h"

struct ve_config {
	int decoder_flag;
	int encoder_flag;
	int format;
	int width;
	int enable_afbc;
	int reset_mode;
	unsigned int ve_freq;
	struct ve_mem_ops *memops;
};

/* first members of the engine access table, as the archive lays them out */
struct ve_ops {
	void *(*init)(struct ve_config *);
	void (*release)(void *);
	int (*lock)(void *);
	int (*unlock)(void *);
	void (*reset)(void *);
	int (*wait_interrupt)(void *);
	int (*get_chip_id)(void *);
	uint64_t (*get_ic_ve_version)(void *);
	void *(*get_group_reg_addr)(void *, int);
	int (*get_dram_type)(void *);
	unsigned int (*get_phy_offset)(void *);
	void (*set_dram_type)(void *);
	void (*set_ddr_mode)(void *, int);
	int (*set_speed)(void *, unsigned int);
	void (*set_enable_afbc_flag)(void *, int);
	void (*set_adjust_dram_speed_flag)(void *, int);
	void (*enable_ve)(void *);
	void (*disable_ve)(void *);
};

extern struct ve_ops *GetVeOpsS(int type);

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
