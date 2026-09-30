// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * TCON pixel clock planning.
 *
 * The TCON derives the pixel clock from its module clock through an
 * integer divider. The best (module rate, divider) pair is the one whose
 * resulting pixel clock is closest to the requested one, preferring the
 * smallest divider on ties to keep the module clock low.
 */
#define DPY_LOG_TAG "tcon-clk"
#include <dpy/dpy_log.h>

#include "tcon_priv.h"

static uint32_t tcon_abs_diff(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

int tcon_clk_plan(struct dpy_clk *mod, uint32_t pixclk_hz, uint32_t min_div,
		  uint32_t max_div, uint32_t max_rate,
		  struct tcon_clk_plan *plan)
{
	uint32_t best_err = 0xffffffffU;
	uint32_t div, target, rate, pix, err;

	if (!pixclk_hz || !min_div || min_div > max_div)
		return -EINVAL;

	memset(plan, 0, sizeof(*plan));
	for (div = min_div; div <= max_div; div++) {
		if ((uint64_t)pixclk_hz * div > max_rate)
			break;
		target = pixclk_hz * div;
		rate = dpy_os_clk_round_rate(mod, target);
		if (!rate)
			rate = target;	/* no rounding information */
		pix = DPY_DIV_ROUND_CLOSEST(rate, div);
		err = tcon_abs_diff(pix, pixclk_hz);
		if (err < best_err) {
			best_err = err;
			plan->mod_rate = rate;
			plan->div = div;
			plan->pixclk = pix;
			if (!err)
				break;
		}
	}
	if (!plan->div)
		return -ERANGE;
	/* accept up to 1% error, like most panels tolerate */
	if (best_err > pixclk_hz / 100)
		dpy_warn("pixel clock %u Hz requested, %u Hz possible\n",
			 pixclk_hz, plan->pixclk);
	return 0;
}
