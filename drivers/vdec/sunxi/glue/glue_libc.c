/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* C library functions the archive expects that the minimal libc does not have */

#include <stddef.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

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

int open(const char *path, int flags, ...)
{
	(void)path;
	(void)flags;
	return -1;
}

int close(int fd)
{
	(void)fd;
	return -1;
}

int ioctl(int fd, unsigned long req, ...)
{
	(void)fd;
	(void)req;
	return -1;
}
