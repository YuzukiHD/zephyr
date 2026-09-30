/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The one reset line of the video engine, handed out as an opaque handle */

#include <errno.h>
#include <stdint.h>
#include <zephyr/drivers/reset.h>

#include "glue.h"

static const struct reset_dt_spec ve_reset = RESET_DT_SPEC_GET(VE_NODE);

int hal_rst_get(uint8_t rc_id, uint16_t rst_id, void **rst)
{
	if (rc_id != GLUE_CCU_SYS || rst_id != GLUE_RST_BUS_VE) {
		return -ENOENT;
	}
	*rst = (void *)&ve_reset;

	return 0;
}

int hal_rst_put(void *rst)
{
	(void)rst;
	return 0;
}

int hal_rst_assert(void *rst)
{
	return reset_line_assert_dt(rst);
}

int hal_rst_deassert(void *rst)
{
	return reset_line_deassert_dt(rst);
}
