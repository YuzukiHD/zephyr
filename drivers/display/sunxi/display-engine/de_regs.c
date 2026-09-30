// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display engine shadow register blocks.
 */
#define DPY_LOG_TAG "de-regs"
#include "de_priv.h"

/* the RCQ fetches blocks with 32 byte bursts; use a cache line */
#define DE_REGBLK_ALIGN		64

struct de_regblk *de_regblk_create(struct de_engine *de, const char *name,
				   uint32_t offset, uint32_t size, bool rcq,
				   uint8_t disp)
{
	struct de_regs *regs = &de->regs;
	struct de_regblk *b;
	uint32_t start;

	if (regs->nblks >= DE_MAX_REGBLKS) {
		dpy_err("too many register blocks\n");
		return NULL;
	}
	start = DPY_ALIGN(regs->used, DE_REGBLK_ALIGN);
	if (start + size > regs->arena_size) {
		dpy_err("register arena exhausted (%u bytes)\n",
			regs->arena_size);
		return NULL;
	}
	regs->used = start + size;

	b = &regs->blks[regs->nblks++];
	b->name = name;
	b->offset = offset;
	b->size = size;
	b->shadow = (uint32_t *)(regs->arena + start);
	b->rcq = rcq;
	b->disp = disp;
	b->dirty = true;
	memset(b->shadow, 0, size);
	return b;
}

void de_regs_mark_all_dirty(struct de_engine *de, uint8_t disp)
{
	uint32_t i;

	for (i = 0; i < de->regs.nblks; i++)
		if (de->regs.blks[i].disp == disp)
			de->regs.blks[i].dirty = true;
}

void de_regs_flush_direct(struct de_engine *de, uint8_t disp, bool all)
{
	uint32_t i, j;

	for (i = 0; i < de->regs.nblks; i++) {
		struct de_regblk *b = &de->regs.blks[i];

		if (b->disp != disp || (!all && !b->dirty))
			continue;
		for (j = 0; j < b->size / 4; j++)
			de_write(de, b->offset + j * 4, b->shadow[j]);
		b->dirty = false;
	}
}
