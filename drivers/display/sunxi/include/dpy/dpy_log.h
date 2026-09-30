/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display pipeline framework - logging.
 */
#ifndef __DPY_LOG_H__
#define __DPY_LOG_H__

#include <dpy/dpy_os.h>

enum dpy_log_level {
	DPY_LOG_ERR = 0,
	DPY_LOG_WARN,
	DPY_LOG_INFO,
	DPY_LOG_DBG,
};

/* runtime verbosity, adjustable from the debug shell */
extern int dpy_log_level;

void dpy_log(int level, const char *tag, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

#ifndef DPY_LOG_TAG
#define DPY_LOG_TAG "dpy"
#endif

#define dpy_err(fmt, ...)  dpy_log(DPY_LOG_ERR, DPY_LOG_TAG, fmt, ##__VA_ARGS__)
#define dpy_warn(fmt, ...) dpy_log(DPY_LOG_WARN, DPY_LOG_TAG, fmt, ##__VA_ARGS__)
#define dpy_info(fmt, ...) dpy_log(DPY_LOG_INFO, DPY_LOG_TAG, fmt, ##__VA_ARGS__)
#define dpy_dbg(fmt, ...)  dpy_log(DPY_LOG_DBG, DPY_LOG_TAG, fmt, ##__VA_ARGS__)

#endif /* __DPY_LOG_H__ */
