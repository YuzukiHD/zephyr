/*
 * Shell commands of the sunxi MBUS driver.
 *
 *   mbus stats [ms]                bandwidth of every master over an interval
 *   mbus prio <master> [0..3]      read or set the priority of a master
 *   mbus limit <master> [MB/s]     read or set (0 = off) the bandwidth limit
 *
 * SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

#include <errno.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/mbus_sunxi/mbus_sunxi.h>
#include <zephyr/shell/shell.h>

#define MBUS_DEV DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(allwinner_sunxi_mbus))

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t before[SUNXI_MBUS_PMU_COUNT], after[SUNXI_MBUS_PMU_COUNT];
	uint32_t interval_ms = argc > 1 ? strtoul(argv[1], NULL, 0) : 1000;
	int64_t t0, t1;
	enum sunxi_mbus_pmu i;

	if (!interval_ms) {
		interval_ms = 1000;
	}
	t0 = k_uptime_get();
	for (i = 0; i < SUNXI_MBUS_PMU_COUNT; i++) {
		sunxi_mbus_get_traffic(MBUS_DEV, i, &before[i]);
	}
	k_msleep(interval_ms);
	t1 = k_uptime_get();
	for (i = 0; i < SUNXI_MBUS_PMU_COUNT; i++) {
		sunxi_mbus_get_traffic(MBUS_DEV, i, &after[i]);
	}

	shell_print(sh, "over %lld ms:", (long long)(t1 - t0));
	for (i = 0; i < SUNXI_MBUS_PMU_COUNT; i++) {
		uint32_t delta = after[i] - before[i];	/* wraps correctly */
		/* bytes / ms = kB/s, shown as MB/s with two decimals */
		uint32_t kbps = (uint32_t)((uint64_t)delta / (uint64_t)(t1 - t0));

		if (!delta) {
			continue;
		}
		shell_print(sh, "%-8s %4u.%02u MB/s", sunxi_mbus_pmu_name(i), kbps / 1000,
			    (kbps % 1000) / 10);
	}
	return 0;
}

static int cmd_prio(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t master = strtoul(argv[1], NULL, 0), prio;
	int ret;

	if (argc > 2) {
		ret = sunxi_mbus_set_priority(MBUS_DEV, master, strtoul(argv[2], NULL, 0));
	} else {
		ret = sunxi_mbus_get_priority(MBUS_DEV, master, &prio);
		if (!ret) {
			shell_print(sh, "master %u priority %u", master, prio);
		}
	}
	if (ret) {
		shell_error(sh, "bad master or priority (%d)", ret);
	}
	return ret;
}

static int cmd_limit(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t master = strtoul(argv[1], NULL, 0), mbps;
	int ret;

	if (argc > 2) {
		ret = sunxi_mbus_set_limit(MBUS_DEV, master, strtoul(argv[2], NULL, 0));
	} else {
		ret = sunxi_mbus_get_limit(MBUS_DEV, master, &mbps);
		if (!ret) {
			shell_print(sh, "master %u limit %u MB/s", master, mbps);
		}
	}
	if (ret) {
		shell_error(sh, "bad master (%d)", ret);
	}
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_mbus,
	SHELL_CMD_ARG(stats, NULL, "stats [interval ms]: bandwidth per master", cmd_stats, 1, 1),
	SHELL_CMD_ARG(prio, NULL, "prio <master> [0..3]", cmd_prio, 2, 1),
	SHELL_CMD_ARG(limit, NULL, "limit <master> [MB/s]", cmd_limit, 2, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(mbus, &sub_mbus, "Memory bus (MBUS)", NULL);
