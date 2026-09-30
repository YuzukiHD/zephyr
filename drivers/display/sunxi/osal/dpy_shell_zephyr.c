// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Zephyr shell commands of the display pipeline. Only the registration is
 * OS specific; the work is done by core/dpy_debug.c and debug/display_test.c.
 *
 *   display info | graph | regs | stats | log <0..3> | backlight <0..255> |
 *           blank <on|off>
 *   display_test <sub command> ...      (CONFIG_HAL_TEST_DISPLAY_ENGINE)
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>

#include <dpy/dpy_debug.h>
#include <dpy/dpy_log.h>
#include <dpy/dpy_os.h>
#include <hal/display/display_engine.h>

#include "dpy_os_zephyr.h"

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	dpy_os_shell_bind(sh);
	dpy_debug_state(dpy_os_printf);
	dpy_os_shell_bind(NULL);
	return 0;
}

static int cmd_graph(const struct shell *sh, size_t argc, char **argv)
{
	dpy_os_shell_bind(sh);
	dpy_debug_graph(dpy_os_printf);
	dpy_os_shell_bind(NULL);
	return 0;
}

static int cmd_regs(const struct shell *sh, size_t argc, char **argv)
{
	dpy_os_shell_bind(sh);
	dpy_debug_regs(dpy_os_printf);
	dpy_os_shell_bind(NULL);
	return 0;
}

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct display_stats st;
	int ret = display_get_stats(&st);

	if (ret) {
		shell_error(sh, "no display: %d", ret);
		return ret;
	}
	shell_print(sh, "vblank %llu, commits %u (timeouts %u), last commit %u us",
		    (unsigned long long)st.vblank_count, st.commit_count,
		    st.commit_timeout, st.last_commit_us);
	shell_print(sh, "fifo underflow %u, refresh %u.%03u Hz", st.fifo_underflow,
		    st.refresh_mhz / 1000, st.refresh_mhz % 1000);
	return 0;
}

static int cmd_log(const struct shell *sh, size_t argc, char **argv)
{
	int level = (int)strtol(argv[1], NULL, 0);

	if (level < DPY_LOG_ERR || level > DPY_LOG_DBG) {
		shell_error(sh, "level 0 (err) .. 3 (debug)");
		return -EINVAL;
	}
	dpy_log_level = level;
	return 0;
}

static int cmd_backlight(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t level;
	int ret;

	if (argc < 2) {
		ret = display_get_backlight(&level);
		if (ret) {
			return ret;
		}
		shell_print(sh, "backlight %u", level);
		return 0;
	}
	ret = display_set_backlight(strtoul(argv[1], NULL, 0));
	if (ret) {
		shell_error(sh, "backlight: %d", ret);
	}
	return ret;
}

static int cmd_blank(const struct shell *sh, size_t argc, char **argv)
{
	bool on;

	if (!strcmp(argv[1], "on")) {
		on = true;
	} else if (!strcmp(argv[1], "off")) {
		on = false;
	} else {
		shell_error(sh, "blank <on|off>");
		return -EINVAL;
	}
	return display_blank(on);
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_display,
	SHELL_CMD_ARG(info, NULL, "Pipeline state", cmd_info, 1, 0),
	SHELL_CMD_ARG(graph, NULL, "Nodes and links", cmd_graph, 1, 0),
	SHELL_CMD_ARG(regs, NULL, "Dump the display registers", cmd_regs, 1, 0),
	SHELL_CMD_ARG(stats, NULL, "Vblank, commit and underflow counters", cmd_stats, 1, 0),
	SHELL_CMD_ARG(log, NULL, "log <0..3>: set the verbosity", cmd_log, 2, 0),
	SHELL_CMD_ARG(backlight, NULL, "backlight [0..255]", cmd_backlight, 1, 1),
	SHELL_CMD_ARG(blank, NULL, "blank <on|off>", cmd_blank, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(display, &sub_display, "Display pipeline", NULL);

#ifdef CONFIG_HAL_TEST_DISPLAY_ENGINE
static int cmd_display_test(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	dpy_os_shell_bind(sh);
	ret = display_test_main(argc - 1, argv + 1);
	dpy_os_shell_bind(NULL);

	if (ret) {
		shell_error(sh, "display_test: %d", ret);
	}
	return ret;
}

SHELL_CMD_ARG_REGISTER(display_test, NULL,
		       "Display engine tests: info|pattern|fill|hold|overlay|off|...",
		       cmd_display_test, 1, 16);
#endif
