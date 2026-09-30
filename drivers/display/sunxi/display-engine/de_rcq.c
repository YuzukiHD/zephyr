// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display engine register command queue (RCQ).
 *
 * The RCQ is a list of headers in memory, one per shadow register block.
 * When triggered, the engine walks the list at the next frame start and
 * copies the blocks marked dirty into its registers. Only blocks whose
 * shadow content changed are marked, so a page flip reloads a few words
 * instead of the whole engine configuration.
 */
#define DPY_LOG_TAG "de-rcq"
#include "de_priv.h"

/* frames to wait for the load before assuming it was lost */
#define DE_RCQ_MAX_WAIT_VBLANKS	3
/* how long a commit may wait for a safe point in the frame */
#define DE_RCQ_SAFE_TIMEOUT_US	40000

int de_rcq_init(struct de_engine *de, uint8_t disp)
{
	struct de_rcq *rcq = &de->crtc[disp].rcq;
	uint32_t i, n = 0;

	for (i = 0; i < de->regs.nblks; i++) {
		struct de_regblk *b = &de->regs.blks[i];

		if (b->rcq && b->disp == disp)
			rcq->blks[n++] = b;
	}
	if (!n)
		return -EINVAL;

	rcq->hdrs = dpy_os_dma_alloc(n * sizeof(struct de_rcq_hdr), 64,
				     &rcq->hdrs_dma);
	if (!rcq->hdrs)
		return -ENOMEM;
	rcq->nhdrs = n;

	for (i = 0; i < n; i++) {
		struct de_regblk *b = rcq->blks[i];
		uint64_t dma = dpy_os_virt_to_dma(b->shadow);

		rcq->hdrs[i].low_addr = (uint32_t)dma;
		rcq->hdrs[i].dw0 = (b->size & 0xffffff) |
				   ((uint32_t)((dma >> 32) & 0xff) << 24);
		rcq->hdrs[i].dirty = 0;
		rcq->hdrs[i].reg_offset = b->offset;
		b->hdr = &rcq->hdrs[i];
	}
	dpy_os_dcache_clean(rcq->hdrs, n * sizeof(struct de_rcq_hdr));
	dpy_info("rcq%u: %u blocks, %u bytes of shadow registers\n", disp, n,
		 de->regs.used);
	return 0;
}

void de_rcq_exit(struct de_engine *de, uint8_t disp)
{
	struct de_rcq *rcq = &de->crtc[disp].rcq;
	uint32_t i;

	for (i = 0; i < rcq->nhdrs; i++)
		rcq->blks[i]->hdr = NULL;
	dpy_os_dma_free(rcq->hdrs);
	rcq->hdrs = NULL;
	rcq->nhdrs = 0;
}

/*
 * Sleep until the scan position is well inside the frame, so the load
 * requested now cannot race with the frame start it is meant for.
 */
static void de_rcq_wait_safe_point(struct dpy_timing_ctrl *tc)
{
	uint64_t deadline = dpy_os_time_us() + DE_RCQ_SAFE_TIMEOUT_US;

	while (dpy_os_time_us() < deadline) {
		if (dpy_timing_ctrl_in_safe_window(tc))
			return;
		dpy_os_udelay(200);
	}
	dpy_warn("no safe point to trigger the update\n");
}

uint32_t de_rcq_prepare(struct de_engine *de, uint8_t disp)
{
	struct de_rcq *rcq = &de->crtc[disp].rcq;
	uint32_t i, ndirty = 0;

	for (i = 0; i < rcq->nhdrs; i++) {
		struct de_regblk *b = rcq->blks[i];

		rcq->hdrs[i].dirty = b->dirty ? 1 : 0;
		if (b->dirty) {
			dpy_os_dcache_clean(b->shadow, b->size);
			b->dirty = false;
			ndirty++;
		}
	}
	if (ndirty)
		dpy_os_dcache_clean(rcq->hdrs,
				    rcq->nhdrs * sizeof(struct de_rcq_hdr));
	return ndirty;
}

/*
 * Arm the flip completion and start the load in one critical section: a
 * vblank finishing an older load must not complete the new flip.
 */
void de_rcq_trigger(struct de_engine *de, uint8_t disp)
{
	struct dpy_crtc *crtc = &de->crtc[disp].base;
	struct de_rcq *rcq = &de->crtc[disp].rcq;
	unsigned long flags;

	de_rcq_wait_safe_point(crtc->tc);
	dpy_crtc_flip_drain(crtc);

	flags = dpy_os_spin_lock_irqsave(&crtc->lock);
	dpy_crtc_arm_flip_locked(crtc);
	rcq->pending = true;
	rcq->wait_vblanks = 0;
	de_top_rcq_trigger(de, disp);
	dpy_os_spin_unlock_irqrestore(&crtc->lock, flags);
	rcq->commits++;
}

/* give up on a load the hardware never confirmed */
void de_rcq_abort(struct de_engine *de, uint8_t disp)
{
	struct dpy_crtc *crtc = &de->crtc[disp].base;
	unsigned long flags;

	flags = dpy_os_spin_lock_irqsave(&crtc->lock);
	de->crtc[disp].rcq.pending = false;
	dpy_os_spin_unlock_irqrestore(&crtc->lock, flags);
	de_regs_mark_all_dirty(de, disp);
}

bool de_rcq_check_done(struct de_engine *de, uint8_t disp, bool vblank)
{
	struct de_rcq *rcq = &de->crtc[disp].rcq;

	if (!rcq->pending)
		return false;
	if (de_top_rcq_finished(de, disp)) {
		rcq->pending = false;
		if (!vblank)
			rcq->early_done++;
		return true;
	}
	if (!vblank)
		return false;
	if (++rcq->wait_vblanks >= DE_RCQ_MAX_WAIT_VBLANKS) {
		/* never observed the load: reload everything next time */
		rcq->pending = false;
		rcq->timeouts++;
		de_regs_mark_all_dirty(de, disp);
		return true;
	}
	return false;
}
