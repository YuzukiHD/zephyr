/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* C library functions the archive expects that the minimal libc does not have */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "glue.h"

LOG_MODULE_DECLARE(vdec_sunxi_glue, CONFIG_VDEC_SUNXI_LOG_LEVEL);

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
	printk("assert: %s:%d %s: %s\n", file, line, func, expr);
	k_panic();
	for (;;) {
	}
}

int usleep(unsigned int us)
{
	k_usleep(us);
	return 0;
}

/* The archive can dump data to files for debugging; there is no file system here. */
FILE *fopen(const char *path, const char *mode)
{
	(void)path;
	(void)mode;
	return NULL;
}

int fclose(FILE *f)
{
	(void)f;
	return -1;
}

__attribute__((weak)) int open(const char *path, int flags, ...)
{
	(void)path;
	(void)flags;
	return -1;
}

__attribute__((weak)) int close(int fd)
{
	(void)fd;
	return -1;
}

__attribute__((weak)) int ioctl(int fd, unsigned long req, ...)
{
	(void)fd;
	(void)req;
	return -1;
}

/*
 * The archive prints its messages with printf() (the build wraps that symbol,
 * see CMakeLists.txt). They are turned into Zephyr log messages of the glue
 * module: the level comes from the word the line starts with, the colour codes
 * are removed.
 */
#define ARCHIVE_LINE_MAX 256

static void strip_ansi(char *s)
{
	char *out = s;

	while (*s != '\0') {
		if (s[0] == '\033' && s[1] == '[') {
			s += 2;
			while (*s != '\0' && *s != 'm') {
				s++;
			}
			if (*s == 'm') {
				s++;
			}
			continue;
		}
		*out++ = *s++;
	}
	*out = '\0';
}

int __wrap_printf(const char *fmt, ...)
{
	char line[ARCHIVE_LINE_MAX];
	char *text;
	va_list ap;
	size_t len;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	strip_ansi(line);
	len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
		line[--len] = '\0';
	}
	if (len == 0) {
		return n;
	}

	/* "level  : text": the level word is replaced by the log level itself */
	text = line;
	if (strncmp(line, "error", 5) == 0 || strncmp(line, "warn", 4) == 0 ||
	    strncmp(line, "info", 4) == 0) {
		char *colon = strstr(line, ": ");

		if (colon != NULL) {
			text = colon + 2;
		}
	}

	if (strncmp(line, "error", 5) == 0) {
		LOG_ERR("%s", text);
	} else if (strncmp(line, "warn", 4) == 0) {
		LOG_WRN("%s", text);
	} else if (strncmp(line, "info", 4) == 0) {
		LOG_INF("%s", text);
	} else {
		LOG_DBG("%s", text);
	}

	return n;
}
