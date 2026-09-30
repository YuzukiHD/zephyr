// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - logging.
 *
 * The messages go through the Zephyr logging subsystem (module "display",
 * level CONFIG_DISPLAY_LOG_LEVEL, runtime filter "log enable ... display"),
 * the pipeline tag becomes the message prefix.
 */
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include <dpy/dpy_log.h>

LOG_MODULE_REGISTER(display, CONFIG_DISPLAY_LOG_LEVEL);

/* extra runtime verbosity cap, adjustable with "display log <0..3>" */
int dpy_log_level = DPY_LOG_DBG;

void dpy_log(int level, const char *tag, const char *fmt, ...)
{
	char msg[192];
	va_list ap;
	size_t len;

	if (level > dpy_log_level)
		return;

	va_start(ap, fmt);
	vsnprintk(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	/* the pipeline messages carry their own line ending */
	len = strlen(msg);
	while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
		msg[--len] = '\0';

	switch (level) {
	case DPY_LOG_ERR:
		LOG_ERR("%s: %s", tag, msg);
		break;
	case DPY_LOG_WARN:
		LOG_WRN("%s: %s", tag, msg);
		break;
	case DPY_LOG_INFO:
		LOG_INF("%s: %s", tag, msg);
		break;
	default:
		LOG_DBG("%s: %s", tag, msg);
		break;
	}
}
