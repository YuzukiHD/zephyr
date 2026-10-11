/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include "ve_h264_dpb.h"

void h264_dpb_init(struct h264_dpb *d, const struct h264_sps *sps)
{
	unsigned int size = sps->max_num_ref_frames;

	memset(d, 0, sizeof(*d));
	if (sps->bitstream_restriction && sps->max_dec_frame_buffering > size) {
		size = sps->max_dec_frame_buffering;
	}
	if (size < 1) {
		size = 1;
	}
	if (size > H264_DPB_SLOTS - 1) {
		size = H264_DPB_SLOTS - 1;
	}
	d->size = size;
	d->use_reorder = sps->bitstream_restriction;
	d->num_reorder = sps->num_reorder_frames;
}

static int frame_only(const struct h264_slice_hdr *h)
{
	return !h->field_pic;
}

/* ---- picture order count (8.2.1) ---- */

int h264_dpb_compute_poc(struct h264_dpb *d, const struct h264_sps *sps,
			 const struct h264_slice_hdr *h, int *top, int *bottom)
{
	struct h264_poc_state *s = &d->poc;
	int max_fn = 1 << sps->log2_max_frame_num;
	int frame_num_offset = 0;

	if (!frame_only(h)) {
		return -ENOTSUP;
	}
	if (sps->poc_type == 0) {
		int max_lsb = 1 << sps->log2_max_poc_lsb;
		int prev_msb = h->idr ? 0 : s->prev_poc_msb;
		int prev_lsb = h->idr ? 0 : s->prev_poc_lsb;
		int lsb = (int)h->poc_lsb;
		int msb;

		if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2) {
			msb = prev_msb + max_lsb;
		} else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2) {
			msb = prev_msb - max_lsb;
		} else {
			msb = prev_msb;
		}
		*top = msb + lsb;
		*bottom = *top + h->delta_poc_bottom;
		if (h->nal_ref_idc != 0) {
			s->prev_poc_msb = msb;
			s->prev_poc_lsb = lsb;
		}
		return 0;
	}
	if (h->idr) {
		frame_num_offset = 0;
	} else if (s->prev_frame_num > h->frame_num) {
		frame_num_offset = s->prev_frame_num_offset + max_fn;
	} else {
		frame_num_offset = s->prev_frame_num_offset;
	}
	if (sps->poc_type == 1) {
		int abs_fn = sps->poc_cycle_length != 0 ? frame_num_offset + (int)h->frame_num : 0;
		int expected = 0;

		if (h->nal_ref_idc == 0 && abs_fn > 0) {
			abs_fn--;
		}
		if (abs_fn > 0) {
			int per_cycle = 0;
			int cycle = (abs_fn - 1) / (int)sps->poc_cycle_length;
			int in_cycle = (abs_fn - 1) % (int)sps->poc_cycle_length;

			for (unsigned int i = 0; i < sps->poc_cycle_length; i++) {
				per_cycle += sps->offset_for_ref_frame[i];
			}
			expected = cycle * per_cycle;
			for (int i = 0; i <= in_cycle; i++) {
				expected += sps->offset_for_ref_frame[i];
			}
		}
		if (h->nal_ref_idc == 0) {
			expected += sps->offset_for_non_ref_pic;
		}
		*top = expected + h->delta_poc[0];
		*bottom = *top + sps->offset_for_top_to_bottom_field + h->delta_poc[1];
	} else {
		int t = 0;

		if (!h->idr) {
			t = 2 * (frame_num_offset + (int)h->frame_num);
			if (h->nal_ref_idc == 0) {
				t--;
			}
		}
		*top = *bottom = t;
	}
	s->prev_frame_num_offset = frame_num_offset;
	s->prev_frame_num = h->frame_num;

	return 0;
}

/* ---- slots ---- */

static int slot_busy(const struct h264_dpb_pic *p)
{
	return p->active && (p->is_current || p->ref != H264_REF_NONE || p->output_needed);
}

int h264_dpb_alloc(struct h264_dpb *d)
{
	unsigned int busy = 0;

	for (unsigned int i = 0; i < H264_DPB_SLOTS; i++) {
		busy += slot_busy(&d->pic[i]);
	}
	if (busy >= d->size + 1) {
		return -ENOMEM;
	}
	for (int i = 0; i < H264_DPB_SLOTS; i++) {
		if (!slot_busy(&d->pic[i])) {
			memset(&d->pic[i], 0, sizeof(d->pic[i]));
			d->pic[i].active = 1;
			d->pic[i].is_current = 1;
			return i;
		}
	}

	return -ENOMEM;
}

/* ---- reference lists (8.2.4) ---- */

static void sort_slots(const struct h264_dpb *d, int *s, unsigned int n, int by_poc, int desc)
{
	for (unsigned int i = 1; i < n; i++) {
		int v = s[i];
		int key = by_poc ? d->pic[v].poc : d->pic[v].frame_num_wrap;
		unsigned int j = i;

		while (j > 0) {
			int pk = by_poc ? d->pic[s[j - 1]].poc : d->pic[s[j - 1]].frame_num_wrap;

			if (desc ? pk >= key : pk <= key) {
				break;
			}
			s[j] = s[j - 1];
			j--;
		}
		s[j] = v;
	}
}

int h264_dpb_build_lists(const struct h264_dpb *d, const struct h264_sps *sps,
			 const struct h264_slice_hdr *h, int cur, struct h264_ref_lists *out)
{
	int max_fn = 1 << sps->log2_max_frame_num;
	int shorts[H264_DPB_SLOTS], longs[H264_DPB_SLOTS];
	unsigned int ns = 0, nl = 0;
	int init[2][H264_DPB_SLOTS];
	unsigned int ninit[2] = {0, 0};
	unsigned int lists = h->slice_type == H264_SLICE_B ? 2 : 1;
	struct h264_dpb_pic *cp = (struct h264_dpb_pic *)&d->pic[cur];

	if (!frame_only(h)) {
		return -ENOTSUP;
	}
	memset(out, 0, sizeof(*out));
	if (h->slice_type == H264_SLICE_I || h->slice_type == H264_SLICE_SI) {
		return 0;
	}
	for (int i = 0; i < H264_DPB_SLOTS; i++) {
		struct h264_dpb_pic *p = (struct h264_dpb_pic *)&d->pic[i];

		if (!p->active || i == cur) {
			continue;
		}
		if (p->ref == H264_REF_SHORT) {
			p->frame_num_wrap = (int)p->frame_num > (int)h->frame_num ?
						    (int)p->frame_num - max_fn : (int)p->frame_num;
			shorts[ns++] = i;
		} else if (p->ref == H264_REF_LONG) {
			longs[nl++] = i;
		}
	}
	(void)cp;
	/* long term pictures by LongTermPicNum */
	for (unsigned int i = 1; i < nl; i++) {
		int v = longs[i];
		unsigned int j = i;

		while (j > 0 && d->pic[longs[j - 1]].long_term_idx > d->pic[v].long_term_idx) {
			longs[j] = longs[j - 1];
			j--;
		}
		longs[j] = v;
	}
	if (h->slice_type == H264_SLICE_B) {
		int before[H264_DPB_SLOTS], after[H264_DPB_SLOTS];
		unsigned int nb = 0, na = 0;
		int cur_poc = d->pic[cur].poc;

		for (unsigned int i = 0; i < ns; i++) {
			if (d->pic[shorts[i]].poc < cur_poc) {
				before[nb++] = shorts[i];
			} else {
				after[na++] = shorts[i];
			}
		}
		sort_slots(d, before, nb, 1, 1);
		sort_slots(d, after, na, 1, 0);
		for (unsigned int i = 0; i < nb; i++) {
			init[0][ninit[0]++] = before[i];
		}
		for (unsigned int i = 0; i < na; i++) {
			init[0][ninit[0]++] = after[i];
		}
		for (unsigned int i = 0; i < na; i++) {
			init[1][ninit[1]++] = after[i];
		}
		for (unsigned int i = 0; i < nb; i++) {
			init[1][ninit[1]++] = before[i];
		}
		for (unsigned int i = 0; i < nl; i++) {
			init[0][ninit[0]++] = longs[i];
			init[1][ninit[1]++] = longs[i];
		}
		if (ninit[1] > 1 && ninit[0] == ninit[1] &&
		    memcmp(init[0], init[1], ninit[0] * sizeof(int)) == 0) {
			int t = init[1][0];

			init[1][0] = init[1][1];
			init[1][1] = t;
		}
	} else {
		sort_slots(d, shorts, ns, 0, 1);
		for (unsigned int i = 0; i < ns; i++) {
			init[0][ninit[0]++] = shorts[i];
		}
		for (unsigned int i = 0; i < nl; i++) {
			init[0][ninit[0]++] = longs[i];
		}
	}

	for (unsigned int l = 0; l < lists; l++) {
		unsigned int active = h->num_ref_idx_active[l];
		int list[H264_MAX_REFS + 1];
		int pred = (int)h->frame_num;
		unsigned int idx = 0;

		for (unsigned int i = 0; i <= active && i < H264_MAX_REFS + 1; i++) {
			list[i] = -1;
		}
		for (unsigned int i = 0; i < ninit[l] && i < active; i++) {
			list[i] = init[l][i];
		}
		/* modification (8.2.4.3) */
		for (unsigned int m = 0; m < h->ref_mod_count[l] && idx < active; m++) {
			const struct h264_ref_mod *op = &h->ref_mod[l][m];
			int target = -1;
			int pic_num = 0;

			if (op->idc == 0 || op->idc == 1) {
				int diff = (int)op->value + 1;

				if (op->idc == 0) {
					pred -= diff;
					if (pred < 0) {
						pred += max_fn;
					}
				} else {
					pred += diff;
					if (pred >= max_fn) {
						pred -= max_fn;
					}
				}
				pic_num = pred > (int)h->frame_num ? pred - max_fn : pred;
				for (int i = 0; i < H264_DPB_SLOTS; i++) {
					if (i != cur && d->pic[i].active && d->pic[i].ref == H264_REF_SHORT &&
					    d->pic[i].frame_num_wrap == pic_num) {
						target = i;
					}
				}
			} else if (op->idc == 2) {
				for (int i = 0; i < H264_DPB_SLOTS; i++) {
					if (i != cur && d->pic[i].active && d->pic[i].ref == H264_REF_LONG &&
					    d->pic[i].long_term_idx == op->value) {
						target = i;
					}
				}
			} else {
				continue;
			}
			for (unsigned int c = active; c > idx; c--) {
				list[c] = list[c - 1];
			}
			list[idx++] = target;
			{
				unsigned int n = idx;

				for (unsigned int c = idx; c <= active; c++) {
					if (list[c] != target) {
						list[n++] = list[c];
					}
				}
			}
		}
		out->count[l] = active;
		for (unsigned int i = 0; i < active; i++) {
			out->slot[l][i] = list[i];
		}
	}

	return 0;
}

/* ---- marking (8.2.5) ---- */

static void unmark_all(struct h264_dpb *d, int except)
{
	for (int i = 0; i < H264_DPB_SLOTS; i++) {
		if (i != except && d->pic[i].active) {
			d->pic[i].ref = H264_REF_NONE;
		}
	}
}

static int find_short(struct h264_dpb *d, int cur, int pic_num)
{
	for (int i = 0; i < H264_DPB_SLOTS; i++) {
		if (i != cur && d->pic[i].active && d->pic[i].ref == H264_REF_SHORT &&
		    d->pic[i].frame_num_wrap == pic_num) {
			return i;
		}
	}

	return -1;
}

static void sliding_window(struct h264_dpb *d, const struct h264_sps *sps, int cur)
{
	unsigned int n = 0;
	int oldest = -1;
	unsigned int refs = sps->max_num_ref_frames > 0 ? sps->max_num_ref_frames : 1;

	for (int i = 0; i < H264_DPB_SLOTS; i++) {
		if (i == cur || !d->pic[i].active || d->pic[i].ref == H264_REF_NONE) {
			continue;
		}
		n++;
		if (d->pic[i].ref == H264_REF_SHORT &&
		    (oldest < 0 || d->pic[i].frame_num_wrap < d->pic[oldest].frame_num_wrap)) {
			oldest = i;
		}
	}
	if (n >= refs && oldest >= 0) {
		d->pic[oldest].ref = H264_REF_NONE;
	}
}

int h264_dpb_finish_picture(struct h264_dpb *d, const struct h264_sps *sps,
			    const struct h264_slice_hdr *h, int cur)
{
	struct h264_dpb_pic *p = &d->pic[cur];
	int max_fn = 1 << sps->log2_max_frame_num;
	int long_cur = 0;

	if (!frame_only(h)) {
		return -ENOTSUP;
	}
	p->frame_num = h->frame_num;
	p->frame_num_wrap = (int)h->frame_num;
	if (h->nal_ref_idc != 0) {
		if (h->idr) {
			unmark_all(d, cur);
			if (h->long_term_reference_flag) {
				long_cur = 1;
				p->long_term_idx = 0;
				d->max_long_idx_plus1 = 1;
			} else {
				d->max_long_idx_plus1 = 0;
			}
		} else {
			/* FrameNumWrap of the others relative to this picture */
			for (int i = 0; i < H264_DPB_SLOTS; i++) {
				struct h264_dpb_pic *q = &d->pic[i];

				if (i != cur && q->active && q->ref == H264_REF_SHORT) {
					q->frame_num_wrap = (int)q->frame_num > (int)h->frame_num ?
								    (int)q->frame_num - max_fn :
								    (int)q->frame_num;
				}
			}
			if (h->adaptive_ref_pic_marking) {
				for (unsigned int m = 0; m < h->mmco_count; m++) {
					const struct h264_mmco *op = &h->mmco[m];
					int x;

					switch (op->op) {
					case 1:
						x = find_short(d, cur, (int)h->frame_num -
									       ((int)op->difference_of_pic_nums_minus1 + 1));
						if (x >= 0) {
							d->pic[x].ref = H264_REF_NONE;
						}
						break;
					case 2:
						for (int i = 0; i < H264_DPB_SLOTS; i++) {
							if (i != cur && d->pic[i].active && d->pic[i].ref == H264_REF_LONG &&
							    d->pic[i].long_term_idx == op->long_term_pic_num) {
								d->pic[i].ref = H264_REF_NONE;
							}
						}
						break;
					case 3:
						x = find_short(d, cur, (int)h->frame_num -
									       ((int)op->difference_of_pic_nums_minus1 + 1));
						for (int i = 0; i < H264_DPB_SLOTS; i++) {
							if (i != cur && i != x && d->pic[i].active &&
							    d->pic[i].ref == H264_REF_LONG &&
							    d->pic[i].long_term_idx == op->long_term_frame_idx) {
								d->pic[i].ref = H264_REF_NONE;
							}
						}
						if (x >= 0) {
							d->pic[x].ref = H264_REF_LONG;
							d->pic[x].long_term_idx = op->long_term_frame_idx;
						}
						break;
					case 4:
						d->max_long_idx_plus1 = op->max_long_term_frame_idx_plus1;
						for (int i = 0; i < H264_DPB_SLOTS; i++) {
							if (i != cur && d->pic[i].active && d->pic[i].ref == H264_REF_LONG &&
							    d->pic[i].long_term_idx >= op->max_long_term_frame_idx_plus1) {
								d->pic[i].ref = H264_REF_NONE;
							}
						}
						break;
					case 5:
						unmark_all(d, cur);
						d->max_long_idx_plus1 = 0;
						d->poc.prev_had_mmco5 = 1;
						break;
					case 6:
						for (int i = 0; i < H264_DPB_SLOTS; i++) {
							if (i != cur && d->pic[i].active && d->pic[i].ref == H264_REF_LONG &&
							    d->pic[i].long_term_idx == op->long_term_frame_idx) {
								d->pic[i].ref = H264_REF_NONE;
							}
						}
						long_cur = 1;
						p->long_term_idx = op->long_term_frame_idx;
						break;
					default:
						break;
					}
				}
			} else {
				sliding_window(d, sps, cur);
			}
		}
		p->ref = long_cur ? H264_REF_LONG : H264_REF_SHORT;
	}
	p->is_current = 0;
	p->output_needed = 1;

	return 0;
}

/* ---- output order ---- */

int h264_dpb_output_next(struct h264_dpb *d, int force)
{
	int best = -1;
	unsigned int waiting = 0, busy = 0;

	for (int i = 0; i < H264_DPB_SLOTS; i++) {
		struct h264_dpb_pic *p = &d->pic[i];

		if (!p->active || p->is_current) {
			continue;
		}
		busy += slot_busy(p);
		if (p->output_needed) {
			waiting++;
			if (best < 0 || p->poc < d->pic[best].poc) {
				best = i;
			}
		}
	}
	if (best < 0) {
		return -1;
	}
	if (!force) {
		int due = d->use_reorder ? waiting > d->num_reorder : busy >= d->size;

		if (!due) {
			return -1;
		}
	}
	d->pic[best].output_needed = 0;

	return best;
}

void h264_dpb_flush(struct h264_dpb *d)
{
	(void)d;
}
