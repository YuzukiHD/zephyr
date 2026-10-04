/*
 * Copyright (c) 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The fuses are read word by word through a small command interface: the word
 * index goes to the address register, a read command (with the key that guards
 * the register) to the control register, which clears the start bit when the data
 * is in the read data register. The chip identifier is the first 16 bytes.
 */

#define DT_DRV_COMPAT allwinner_sunxi_sid

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/hwinfo/hwinfo_sunxi_sid.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/sys_io.h>
#include <string.h>

#define SID_BASE	DT_INST_REG_ADDR(0)
#define SID_PRCTL	(SID_BASE + 0x00)
#define SID_PR_ADDR	(SID_BASE + 0x04)
#define SID_RDKEY	(SID_BASE + 0x0c)

#define SID_OP_LOCK_SHIFT	16
#define SID_OP_LOCK_MASK	(0xffffU << SID_OP_LOCK_SHIFT)
#define SID_READ_OP_LOCK	0xadbfU
#define SID_READ_START		BIT(1)
#define SID_START_MASK		(BIT(0) | BIT(1))
#define SID_INDEX_MASK		0xfU

#define SID_CHIPID_BYTES	16
#define SID_WORDS		(SUNXI_SID_BITS / 32)

static struct k_spinlock lock;

static int sid_read_word(unsigned int index, uint32_t *val)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t reg;
	int tries = 100000;

	reg = sys_read32(SID_PR_ADDR);
	reg &= ~SID_INDEX_MASK;
	reg |= index & SID_INDEX_MASK;
	sys_write32(reg, SID_PR_ADDR);

	reg = sys_read32(SID_PRCTL);
	reg &= ~(SID_OP_LOCK_MASK | SID_START_MASK);
	reg |= (SID_READ_OP_LOCK << SID_OP_LOCK_SHIFT) | SID_READ_START;
	sys_write32(reg, SID_PRCTL);

	while ((sys_read32(SID_PRCTL) & SID_READ_START) != 0U) {
		if (--tries == 0) {
			k_spin_unlock(&lock, key);
			return -ETIMEDOUT;
		}
	}
	reg &= ~(SID_OP_LOCK_MASK | SID_START_MASK);
	sys_write32(reg, SID_PRCTL);
	*val = sys_read32(SID_RDKEY);
	k_spin_unlock(&lock, key);

	return 0;
}

ssize_t sunxi_sid_read(uint32_t offset, void *buf, size_t len)
{
	uint8_t *out = buf;

	if ((offset | len) % 4U != 0U || offset + len > SUNXI_SID_BITS / 8) {
		return -EINVAL;
	}
	for (size_t i = 0; i < len; i += 4) {
		uint32_t w;
		int ret = sid_read_word((offset + i) / 4, &w);

		if (ret != 0) {
			return ret;
		}
		memcpy(out + i, &w, 4);
	}

	return len;
}

ssize_t z_impl_hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
	uint8_t id[SID_CHIPID_BYTES];
	ssize_t ret = sunxi_sid_read(0, id, sizeof(id));

	if (ret < 0) {
		return ret;
	}
	length = MIN(length, sizeof(id));
	memcpy(buffer, id, length);

	return length;
}
