/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Zephyr specific helpers of the display OS port.
 */
#ifndef __DPY_OS_ZEPHYR_H__
#define __DPY_OS_ZEPHYR_H__

struct shell;

/*
 * While a shell command runs, dpy_os_printf() writes through the shell so
 * the output is not mixed with the log backend (printk is routed through
 * the deferred logger). Pass NULL when the command is done.
 */
void dpy_os_shell_bind(const struct shell *sh);

#endif /* __DPY_OS_ZEPHYR_H__ */
