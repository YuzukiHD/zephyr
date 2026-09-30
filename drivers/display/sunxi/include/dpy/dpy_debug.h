/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display pipeline framework - debug helpers used by shell front-ends.
 */
#ifndef __DPY_DEBUG_H__
#define __DPY_DEBUG_H__

typedef void (*dpy_print_t)(const char *fmt, ...);

void dpy_debug_graph(dpy_print_t print);
void dpy_debug_state(dpy_print_t print);
void dpy_debug_regs(dpy_print_t print);

/* portable test suite (debug/display_test.c), argv[0] is the sub command */
int display_test_main(int argc, char **argv);

#endif /* __DPY_DEBUG_H__ */
