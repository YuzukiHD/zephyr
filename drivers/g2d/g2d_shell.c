/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/g2d.h>
#include <zephyr/shell/shell.h>

#define BENCH_LOOPS_DEFAULT	8

static const struct device *g2d_shell_dev(const struct shell *sh)
{
	const struct device *dev = DEVICE_DT_GET_ANY(allwinner_sunxi_g2d);

	if (dev == NULL || !device_is_ready(dev)) {
		shell_error(sh, "no G2D device");
		return NULL;
	}
	return dev;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = g2d_shell_dev(sh);
	struct g2d_capabilities caps;

	if (dev == NULL) {
		return -ENODEV;
	}
	g2d_get_capabilities(dev, &caps);
	shell_print(sh, "%s: up to %ux%u, %u operations queued", dev->name, caps.max_width,
		    caps.max_height, caps.queue_depth);
	shell_print(sh, "  source formats 0x%x, destination formats 0x%x, operations 0x%x",
		    caps.src_formats, caps.dst_formats, caps.ops);
	shell_print(sh, "  rotation: %u byte addresses, %u byte destination pitch",
		    caps.rotate_addr_align, caps.rotate_pitch_align);
	return 0;
}

static uint32_t cycles_to_us(uint32_t cycles)
{
	return k_cyc_to_us_near32(cycles);
}

/* straightforward CPU versions, the baseline of the comparison */
static void cpu_fill(uint32_t *dst, size_t pixels, uint32_t color)
{
	for (size_t i = 0; i < pixels; i++) {
		dst[i] = color;
	}
}

static void cpu_blend(uint32_t *dst, const uint32_t *fg, const uint32_t *bg, size_t pixels)
{
	for (size_t i = 0; i < pixels; i++) {
		uint32_t f = fg[i], b = bg[i], out = 0xff000000;
		uint32_t a = f >> 24;

		for (int sh = 0; sh < 24; sh += 8) {
			uint32_t fc = (f >> sh) & 0xff, bc = (b >> sh) & 0xff;

			out |= ((fc * a + bc * (255 - a)) / 255) << sh;
		}
		dst[i] = out;
	}
}

static int cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev = g2d_shell_dev(sh);
	const char *what = argv[1];
	unsigned long w = strtoul(argv[2], NULL, 0);
	unsigned long h = strtoul(argv[3], NULL, 0);
	unsigned long loops = argc > 4 ? strtoul(argv[4], NULL, 0) : BENCH_LOOPS_DEFAULT;
	size_t bytes = w * h * 4;
	struct g2d_surface src = { .format = G2D_PIXFMT_ARGB8888, .width = w, .height = h };
	struct g2d_surface bg = src, dst = src;
	struct g2d_rect r = { 0, 0, w, h };
	struct g2d_blend blend = {
		.mode = G2D_BLEND_SRC_OVER,
		.fg_alpha_mode = G2D_ALPHA_PIXEL,
		.fg_alpha = 0xff,
		.bg_alpha_mode = G2D_ALPHA_PIXEL,
		.bg_alpha = 0xff,
	};
	uint32_t t0, hw = 0, cpu = 0;
	int ret = 0;

	if (dev == NULL) {
		return -ENODEV;
	}
	if (w == 0 || h == 0 || w > 4096 || h > 4096 || loops == 0) {
		shell_error(sh, "size 1..4096, loops > 0");
		return -EINVAL;
	}

	src.plane[0] = aligned_alloc(64, ROUND_UP(bytes, 64));
	bg.plane[0] = aligned_alloc(64, ROUND_UP(bytes, 64));
	dst.plane[0] = aligned_alloc(64, ROUND_UP(bytes, 64));
	if (!src.plane[0] || !bg.plane[0] || !dst.plane[0]) {
		shell_error(sh, "no memory for three %lux%lu buffers", w, h);
		ret = -ENOMEM;
		goto out;
	}
	memset(src.plane[0], 0x80, bytes);
	memset(bg.plane[0], 0x40, bytes);
	sys_cache_data_flush_range(src.plane[0], bytes);
	sys_cache_data_flush_range(bg.plane[0], bytes);

	t0 = k_cycle_get_32();
	for (unsigned long i = 0; i < loops && ret == 0; i++) {
		if (strcmp(what, "fill") == 0) {
			ret = g2d_fill(dev, &dst, &r, 0xff336699);
		} else if (strcmp(what, "copy") == 0) {
			ret = g2d_blit(dev, &src, &r, &dst, &r, G2D_ROTATE_0, 0);
		} else if (strcmp(what, "blend") == 0) {
			ret = g2d_blend(dev, &src, &r, &bg, &r, &dst, &r, &blend, 0);
		} else {
			shell_error(sh, "unknown operation %s (fill, copy, blend)", what);
			ret = -EINVAL;
		}
	}
	hw = cycles_to_us(k_cycle_get_32() - t0) / loops;
	if (ret) {
		shell_error(sh, "G2D operation failed: %d", ret);
		goto out;
	}

	t0 = k_cycle_get_32();
	for (unsigned long i = 0; i < loops; i++) {
		if (strcmp(what, "fill") == 0) {
			cpu_fill(dst.plane[0], w * h, 0xff336699);
		} else if (strcmp(what, "copy") == 0) {
			memcpy(dst.plane[0], src.plane[0], bytes);
		} else {
			cpu_blend(dst.plane[0], src.plane[0], bg.plane[0], w * h);
		}
		/* the result has to reach memory for the display, as with the G2D */
		sys_cache_data_flush_range(dst.plane[0], bytes);
	}
	cpu = cycles_to_us(k_cycle_get_32() - t0) / loops;

	shell_print(sh, "%s %lux%lu ARGB8888: G2D %u us, CPU %u us (%s)", what, w, h, hw, cpu,
		    hw < cpu ? "G2D faster" : "CPU faster");
out:
	free(src.plane[0]);
	free(bg.plane[0]);
	free(dst.plane[0]);
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(g2d_cmds,
	SHELL_CMD(info, NULL, "Show the capabilities", cmd_info),
	SHELL_CMD_ARG(bench, NULL,
		      "Time an operation against the CPU\n"
		      "Usage: g2d bench <fill|copy|blend> <width> <height> [loops]",
		      cmd_bench, 4, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(g2d, &g2d_cmds, "G2D accelerator", NULL);
