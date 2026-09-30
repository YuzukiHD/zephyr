/*
 * Allwinner sunxi MBUS driver.
 *
 * SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

#define DT_DRV_COMPAT allwinner_sunxi_mbus

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/mbus_sunxi/mbus_sunxi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(mbus_sunxi, CONFIG_MBUS_SUNXI_LOG_LEVEL);

#define MBUS_PMU_OFFSET		0x009c
#define MBUS_MSC_OFFSET		0x0210
#define MBUS_BWLR_OFFSET	0x0218
#define MBUS_MASTER_STRIDE	0x10

#define MCGCR			0x00		/* window in bits [31:16] (units of 1 us / 1000), enable bit 0 */
#define MCGCR_ENABLE		BIT(0)
#define PMU_COUNTER(n)		(0x04 + (n) * 4)

#define MSC_PRIORITY_MASK	GENMASK(3, 2)
#define BWLR_LIMIT_MASK		GENMASK(27, 16)
#define BWLR_ENABLE		BIT(31)

/* arbitration settings of one master, from a child node */
struct mbus_sunxi_master {
	const char *name;
	uint8_t master;
	int8_t priority;	/* -1: leave as is */
	int32_t limit_mbps;	/* -1: leave as is */
};

struct mbus_sunxi_config {
	uintptr_t base;
	uint32_t clock_mhz;
	uint32_t window_us;
	const struct mbus_sunxi_master *masters;
	size_t num_masters;
};

static const char *const pmu_names[SUNXI_MBUS_PMU_COUNT] = {
	[SUNXI_MBUS_PMU_CPU] = "cpu",
	[SUNXI_MBUS_PMU_RV_SYS] = "rv_sys",
	[SUNXI_MBUS_PMU_MAHB] = "mahb",
	[SUNXI_MBUS_PMU_DMA] = "dma",
	[SUNXI_MBUS_PMU_VE] = "ve",
	[SUNXI_MBUS_PMU_CE] = "ce",
	[SUNXI_MBUS_PMU_TVD] = "tvd",
	[SUNXI_MBUS_PMU_CSI] = "csi",
	[SUNXI_MBUS_PMU_DSP_SYS] = "dsp_sys",
	[SUNXI_MBUS_PMU_G2D] = "g2d",
	[SUNXI_MBUS_PMU_DI] = "di",
	[SUNXI_MBUS_PMU_DE] = "de",
	[SUNXI_MBUS_PMU_IOMMU] = "iommu",
	[SUNXI_MBUS_PMU_RESERVED] = "reserved",
	[SUNXI_MBUS_PMU_OTHER] = "other",
	[SUNXI_MBUS_PMU_TOTAL] = "total",
};

const char *sunxi_mbus_pmu_name(enum sunxi_mbus_pmu counter)
{
	return counter < SUNXI_MBUS_PMU_COUNT ? pmu_names[counter] : "?";
}

static uintptr_t msc_reg(const struct mbus_sunxi_config *c, uint32_t master)
{
	return c->base + MBUS_MSC_OFFSET + master * MBUS_MASTER_STRIDE;
}

static uintptr_t bwlr_reg(const struct mbus_sunxi_config *c, uint32_t master)
{
	return c->base + MBUS_BWLR_OFFSET + master * MBUS_MASTER_STRIDE;
}

int sunxi_mbus_set_priority(const struct device *dev, uint32_t master, uint32_t priority)
{
	const struct mbus_sunxi_config *c = dev->config;
	uintptr_t reg;

	if (master >= SUNXI_MBUS_MASTER_COUNT || priority > 3) {
		return -EINVAL;
	}
	reg = msc_reg(c, master);
	sys_write32((sys_read32(reg) & ~MSC_PRIORITY_MASK) | FIELD_PREP(MSC_PRIORITY_MASK, priority),
		    reg);
	return 0;
}

int sunxi_mbus_get_priority(const struct device *dev, uint32_t master, uint32_t *priority)
{
	const struct mbus_sunxi_config *c = dev->config;

	if (master >= SUNXI_MBUS_MASTER_COUNT) {
		return -EINVAL;
	}
	*priority = FIELD_GET(MSC_PRIORITY_MASK, sys_read32(msc_reg(c, master)));
	return 0;
}

int sunxi_mbus_set_limit(const struct device *dev, uint32_t master, uint32_t mbps)
{
	const struct mbus_sunxi_config *c = dev->config;
	uintptr_t reg;
	uint32_t v;

	if (master >= SUNXI_MBUS_MASTER_COUNT) {
		return -EINVAL;
	}
	reg = bwlr_reg(c, master);
	v = sys_read32(reg) & ~(BWLR_LIMIT_MASK | BWLR_ENABLE);
	if (mbps) {
		v |= FIELD_PREP(BWLR_LIMIT_MASK, 256U * mbps / c->clock_mhz) | BWLR_ENABLE;
	}
	sys_write32(v, reg);
	return 0;
}

int sunxi_mbus_get_limit(const struct device *dev, uint32_t master, uint32_t *mbps)
{
	const struct mbus_sunxi_config *c = dev->config;
	uint32_t v;

	if (master >= SUNXI_MBUS_MASTER_COUNT) {
		return -EINVAL;
	}
	v = sys_read32(bwlr_reg(c, master));
	*mbps = (v & BWLR_ENABLE) ? FIELD_GET(BWLR_LIMIT_MASK, v) * c->clock_mhz / 256U : 0;
	return 0;
}

int sunxi_mbus_get_traffic(const struct device *dev, enum sunxi_mbus_pmu counter,
			   uint32_t *bytes)
{
	const struct mbus_sunxi_config *c = dev->config;

	if (counter >= SUNXI_MBUS_PMU_COUNT) {
		return -EINVAL;
	}
	*bytes = sys_read32(c->base + MBUS_PMU_OFFSET + PMU_COUNTER(counter));
	return 0;
}

static int mbus_sunxi_init(const struct device *dev)
{
	const struct mbus_sunxi_config *c = dev->config;
	size_t i;
	int ret;

	/* start the bandwidth monitor: the window is counted in units of 1000 */
	sys_write32(sys_read32(c->base + MBUS_PMU_OFFSET + MCGCR) & ~MCGCR_ENABLE,
		    c->base + MBUS_PMU_OFFSET + MCGCR);
	sys_write32(((c->window_us) << 16) | MCGCR_ENABLE, c->base + MBUS_PMU_OFFSET + MCGCR);

	for (i = 0; i < c->num_masters; i++) {
		const struct mbus_sunxi_master *m = &c->masters[i];

		if (m->priority >= 0) {
			ret = sunxi_mbus_set_priority(dev, m->master, m->priority);
			if (ret) {
				LOG_ERR("%s: bad master %u or priority %d", m->name, m->master,
					m->priority);
				return ret;
			}
		}
		if (m->limit_mbps >= 0) {
			ret = sunxi_mbus_set_limit(dev, m->master, m->limit_mbps);
			if (ret) {
				LOG_ERR("%s: bad master %u", m->name, m->master);
				return ret;
			}
		}
		LOG_DBG("%s (master %u): priority %d, limit %d MB/s", m->name, m->master,
			m->priority, m->limit_mbps);
	}
	return 0;
}

#define MBUS_SUNXI_MASTER(node)                                                                    \
	{                                                                                          \
		.name = DT_NODE_FULL_NAME(node),                                                   \
		.master = DT_REG_ADDR(node),                                                       \
		.priority = DT_PROP_OR(node, priority, -1),                                        \
		.limit_mbps = DT_PROP_OR(node, bandwidth_limit_mbps, -1),                          \
	},

#define MBUS_SUNXI_INIT(n)                                                                         \
	static const struct mbus_sunxi_master mbus_masters_##n[] = {                               \
		DT_INST_FOREACH_CHILD_STATUS_OKAY_SEP(n, MBUS_SUNXI_MASTER, ())                    \
		{ .name = NULL },                                                                  \
	};                                                                                         \
	static const struct mbus_sunxi_config mbus_sunxi_cfg_##n = {                               \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.clock_mhz = DT_INST_PROP(n, mbus_clock_mhz),                                      \
		.window_us = DT_INST_PROP(n, pmu_window_us),                                       \
		.masters = mbus_masters_##n,                                                       \
		.num_masters = ARRAY_SIZE(mbus_masters_##n) - 1,                                   \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, mbus_sunxi_init, NULL, NULL, &mbus_sunxi_cfg_##n, POST_KERNEL,    \
			      CONFIG_MBUS_SUNXI_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MBUS_SUNXI_INIT)
