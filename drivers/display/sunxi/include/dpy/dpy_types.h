/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display pipeline framework - basic types and helpers.
 *
 * Everything in the display stack (core, display-engine, tcon, outputs,
 * panels) only depends on this header, <dpy/dpy_os.h> and the C library.
 * No RTOS header may be included outside of osal/.
 */
#ifndef __DPY_TYPES_H__
#define __DPY_TYPES_H__

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef ENOTSUP
#define ENOTSUP 134
#endif

#define DPY_ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

#define DPY_BIT(n)		(1U << (n))
/* contiguous bitmask from bit l to bit h, both inclusive */
#define DPY_GENMASK(h, l)	(((~0U) << (l)) & (~0U >> (31 - (h))))
/* put @val into the field described by @mask */
#define DPY_FIELD_PREP(mask, val) \
	(((uint32_t)(val) << __builtin_ctz(mask)) & (mask))
/* extract the field described by @mask from @reg */
#define DPY_FIELD_GET(mask, reg) \
	(((uint32_t)(reg) & (mask)) >> __builtin_ctz(mask))

#define DPY_MIN(a, b)		((a) < (b) ? (a) : (b))
#define DPY_MAX(a, b)		((a) > (b) ? (a) : (b))
#define DPY_CLAMP(v, lo, hi)	DPY_MIN(DPY_MAX((v), (lo)), (hi))
#define DPY_ALIGN(x, a)		(((x) + ((a) - 1)) & ~((a) - 1))
#define DPY_DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define DPY_DIV_ROUND_CLOSEST(n, d) (((n) + ((d) / 2)) / (d))

#define dpy_container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

/* 16.16 fixed point helpers, the unit used by plane source rectangles */
#define DPY_FP16(x)		((int32_t)(x) << 16)
#define DPY_FP16_INT(x)		((int32_t)(x) >> 16)

#define DPY_UNUSED		__attribute__((unused))
#define DPY_PACKED		__attribute__((packed))

/* Bus/DMA address as seen by display hardware. */
typedef uintptr_t dpy_dma_addr_t;

/* ------------------------------------------------------------------ */
/* Minimal intrusive doubly linked list (no dependency on RTOS lists) */
/* ------------------------------------------------------------------ */
struct dpy_list {
	struct dpy_list *next, *prev;
};

#define DPY_LIST_INIT(name)	{ &(name), &(name) }

static inline void dpy_list_init(struct dpy_list *l)
{
	l->next = l;
	l->prev = l;
}

static inline bool dpy_list_empty(const struct dpy_list *l)
{
	return l->next == l;
}

static inline void dpy_list_add_tail(struct dpy_list *n, struct dpy_list *head)
{
	n->prev = head->prev;
	n->next = head;
	head->prev->next = n;
	head->prev = n;
}

static inline void dpy_list_del(struct dpy_list *n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
	n->next = n;
	n->prev = n;
}

#define dpy_list_entry(ptr, type, member) dpy_container_of(ptr, type, member)

#define dpy_list_for_each_entry(pos, head, member)				\
	for (pos = dpy_list_entry((head)->next, __typeof__(*pos), member);	\
	     &pos->member != (head);						\
	     pos = dpy_list_entry(pos->member.next, __typeof__(*pos), member))

#define dpy_list_for_each_entry_reverse(pos, head, member)			\
	for (pos = dpy_list_entry((head)->prev, __typeof__(*pos), member);	\
	     &pos->member != (head);						\
	     pos = dpy_list_entry(pos->member.prev, __typeof__(*pos), member))

#define dpy_list_for_each_entry_safe(pos, n, head, member)			\
	for (pos = dpy_list_entry((head)->next, __typeof__(*pos), member),	\
	     n = dpy_list_entry(pos->member.next, __typeof__(*pos), member);	\
	     &pos->member != (head);						\
	     pos = n, n = dpy_list_entry(n->member.next, __typeof__(*n), member))

/* ------------------------------------------------------------------ */
/* Rectangles                                                          */
/* ------------------------------------------------------------------ */
struct dpy_rect {
	int32_t x, y;
	uint32_t w, h;
};

/* source rectangle in 16.16 fixed point */
struct dpy_rect_fp {
	int32_t x, y;
	uint32_t w, h;
};

static inline bool dpy_rect_empty(const struct dpy_rect *r)
{
	return r->w == 0 || r->h == 0;
}

static inline bool dpy_rect_equal(const struct dpy_rect *a,
				  const struct dpy_rect *b)
{
	return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

/* bounding box of two rectangles; an empty rectangle is ignored */
static inline struct dpy_rect dpy_rect_union(struct dpy_rect a,
					     struct dpy_rect b)
{
	struct dpy_rect r;
	int32_t x2, y2;

	if (dpy_rect_empty(&a))
		return b;
	if (dpy_rect_empty(&b))
		return a;
	r.x = DPY_MIN(a.x, b.x);
	r.y = DPY_MIN(a.y, b.y);
	x2 = DPY_MAX(a.x + (int32_t)a.w, b.x + (int32_t)b.w);
	y2 = DPY_MAX(a.y + (int32_t)a.h, b.y + (int32_t)b.h);
	r.w = (uint32_t)(x2 - r.x);
	r.h = (uint32_t)(y2 - r.y);
	return r;
}

#endif /* __DPY_TYPES_H__ */
