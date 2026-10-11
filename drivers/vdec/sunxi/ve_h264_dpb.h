/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * H.264 picture order counts, reference lists, reference marking and output order for frame
 * coded pictures. Field pictures are not handled (-ENOTSUP). No hardware access.
 */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_H264_DPB_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_H264_DPB_H_

#include <stdint.h>

#include "ve_h264_parse.h"

#define H264_DPB_SLOTS 17

enum h264_ref_state {
	H264_REF_NONE = 0,
	H264_REF_SHORT = 1,
	H264_REF_LONG = 2,
};

struct h264_dpb_pic {
	int active; /* the slot holds a picture that is decoded or being decoded */
	int is_current;
	enum h264_ref_state ref;
	unsigned int frame_num;
	int frame_num_wrap;
	unsigned int long_term_idx;
	int poc, poc_top, poc_bottom;
	int output_needed;
	int64_t pts;
	void *user; /* the caller's picture buffer */
};

struct h264_poc_state {
	int prev_poc_msb, prev_poc_lsb;
	int prev_frame_num_offset;
	unsigned int prev_frame_num;
	int prev_had_mmco5;
};

struct h264_dpb {
	struct h264_dpb_pic pic[H264_DPB_SLOTS];
	unsigned int size;        /* frames that may be held */
	unsigned int num_reorder; /* pictures that may wait for output */
	int use_reorder;
	unsigned int max_long_idx_plus1;
	struct h264_poc_state poc;
};

struct h264_ref_lists {
	unsigned int count[2];      /* entries to use (num_ref_idx_active) */
	int slot[2][H264_MAX_REFS]; /* DPB slot or -1 for a missing reference */
};

void h264_dpb_init(struct h264_dpb *d, const struct h264_sps *sps);
/* Output everything that is waiting, in POC order, through repeated h264_dpb_output_next() */
void h264_dpb_flush(struct h264_dpb *d);

/* Picture order counts of the picture the header starts; updates the POC state afterwards via
 * finish */
int h264_dpb_compute_poc(struct h264_dpb *d, const struct h264_sps *sps,
			 const struct h264_slice_hdr *h, int *top, int *bottom);

/* A free slot for the new picture (-ENOMEM when the caller has to output pictures first) */
int h264_dpb_alloc(struct h264_dpb *d);

/* Reference lists of a P or B slice of the current picture in `cur` */
int h264_dpb_build_lists(const struct h264_dpb *d, const struct h264_sps *sps,
			 const struct h264_slice_hdr *h, int cur, struct h264_ref_lists *out);

/* Mark references (sliding window / MMCO) and queue the picture for output */
int h264_dpb_finish_picture(struct h264_dpb *d, const struct h264_sps *sps,
			    const struct h264_slice_hdr *h, int cur);

/* Next picture in output order. With `force` the reorder limit is ignored. -1 when none is due. */
int h264_dpb_output_next(struct h264_dpb *d, int force);

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_H264_DPB_H_ */
