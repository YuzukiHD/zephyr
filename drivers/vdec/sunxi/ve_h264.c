/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * H.264 decoder: the syntax is read in software (ve_h264_parse.c), the reference pictures are
 * managed by ve_h264_dpb.c and the engine decodes the slice data (ve_h264_hw.c).
 * Frame coded progressive pictures with CABAC or CAVLC, one NAL unit per stream data item.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>


#include "ve_codec.h"
#include "ve_h264_dpb.h"
#include "ve_h264_hw.h"
#include "ve_h264_parse.h"
#include "ve_hw.h"
#include "ve_log.h"
#include "ve_mem.h"
#include "ve_session.h"

VE_LOG_DECLARE();

#define MVCOL_BYTES_PER_MB	32	/* per field, the size of a buffer is rounded up to 2 KiB */
#define MB_INFO_BYTES_PER_MB	64
#define DEBLOCK_BYTES_PER_MB_COL 1024
#define INTRA_PRED_BASE		(16 * 1024)	/* the engine keeps most of its line buffers on chip */
#define INTRA_PRED_PER_MB_COL	546
#define NEIGHBOR_ALIGN		(16 * 1024)
#define VBV_DATA_CTRL		7

struct slot_info {
	struct ve_picture *pic;
	int shared;		/* queued for display while still used as reference */
	uint32_t mvcol_top, mvcol_bottom;
	int32_t poc_top, poc_bottom;
};

struct h264 {
	struct ve_session *session;

	struct h264_sps *sps;		/* [H264_MAX_SPS] */
	struct h264_pps *pps;		/* [H264_MAX_PPS] */
	const struct h264_sps *act_sps;
	struct h264_dpb dpb;
	int dpb_ready;
	struct ve_h264_hw hw;
	int hw_ready;

	struct slot_info slot[H264_DPB_SLOTS];
	int cur;			/* DPB slot being decoded, -1 when none */
	struct h264_slice_hdr cur_hdr;	/* first slice of the picture */
	unsigned int cur_slices;
	int64_t cur_pts;

	void *deblock_buf, *intra_buf, *mb_intra_buf, *mb_neighbor_raw;
	/* motion buffers of the pictures, one per dpb slot, made once with the sequence */
	void *mvcol_pool[H264_DPB_SLOTS];
	size_t mvcol_half;
	uint32_t mb_neighbor_phys;

	uint8_t *rbsp;
	size_t rbsp_size;
	uint32_t *emu;
	size_t emu_size;
	int dump_count;
	int hw_broken;
	unsigned int pool_mbw, pool_mbh;	/* picture size the buffers were made for */
	int size_changed;		/* a new sequence of another size: not decodable */
	int released;			/* a picture reached the display queue during this call */
};

/* Working memory of the engine, cleared and visible to it */
static void *hw_alloc(size_t size)
{
	void *p = ve_mem_alloc(size);

	if (p != NULL) {
		memset(p, 0, size);
		ve_mem_flush(p, size);
	}

	return p;
}

#define POOL(h) ((h)->session->pool)

/* ---- codec plumbing ---- */

static void *h264_open(struct ve_session *session)
{
	struct h264 *h = calloc(1, sizeof(*h));

	if (h != NULL) {
		h->session = session;
		h->cur = -1;
	}

	return h;
}

static void release_slot(struct h264 *h, int s)
{
	struct slot_info *si = &h->slot[s];

	if (si->pic != NULL && POOL(h) != NULL) {
		ve_pool_discard(POOL(h), si->pic);
	}
	memset(si, 0, sizeof(*si));
	memset(&h->dpb.pic[s], 0, sizeof(h->dpb.pic[s]));
}

static void h264_reset(void *ctx)
{
	struct h264 *h = ctx;

	for (int i = 0; i < H264_DPB_SLOTS; i++) {
		release_slot(h, i);
	}
	if (POOL(h) != NULL) {
		ve_pool_flush(POOL(h));
	}
	h->dpb_ready = 0;
	h->cur = -1;
}

static void h264_close(void *ctx)
{
	struct h264 *h = ctx;

	for (int i = 0; i < H264_DPB_SLOTS; i++) {
		release_slot(h, i);
	}
	ve_pool_destroy(POOL(h));
	POOL(h) = NULL;
	ve_mem_free(h->deblock_buf);
	ve_mem_free(h->intra_buf);
	ve_mem_free(h->mb_intra_buf);
	ve_mem_free(h->mb_neighbor_raw);
	for (int i = 0; i < H264_DPB_SLOTS; i++) {
		ve_mem_free(h->mvcol_pool[i]);
	}
	free(h->rbsp);
	free(h->emu);
	free(h->sps);
	free(h->pps);
	free(h);
}

/* ---- sequence start ---- */

static int start_sequence(struct h264 *h, const struct h264_sps *sps)
{
	unsigned int mbs = sps->mb_width * sps->mb_height;

	if (sps->frame_mbs_only == 0 || sps->chroma_format_idc != 1 || sps->bit_depth_luma != 8) {
		return -ENOTSUP;
	}
	/*
	 * The hardware misdecodes pictures that are at most two macroblocks wide and more than one
	 * high (vertical neighbours of a narrow picture), so they are refused rather than shown wrong.
	 */
	if (sps->mb_width <= 2 && sps->mb_height >= 2) {
		return -ENOTSUP;
	}
	/* the buffers of a running stream are made for its picture size */
	if (POOL(h) != NULL && (sps->mb_width != h->pool_mbw || sps->mb_height != h->pool_mbh)) {
		return -ENOTSUP;
	}
	h264_dpb_init(&h->dpb, sps);
	h->dpb_ready = 1;
	h->act_sps = sps;

	if (POOL(h) == NULL) {
		struct ve_pool_config cfg = {0};

		/*
		 * A picture for every reference, one being decoded, one for the conversion and two
		 * for the application; fewer when the memory budget is smaller than that (never less
		 * than the references plus the two pictures the decoding needs).
		 */
		size_t pic_bytes = (size_t)ROUND_UP(sps->mb_width * 16, 32) * ROUND_UP(sps->mb_height * 16, 32) +
				   2 * (size_t)ROUND_UP((sps->mb_width * 16 + 1) / 2, 16) *
				   ROUND_UP((sps->mb_height * 16 + 1) / 2, 32);
		size_t fixed = (size_t)(h->dpb.size + 1) * 2 *
			       ROUND_UP((size_t)mbs * MVCOL_BYTES_PER_MB, 2048) +
			       (size_t)mbs * MB_INFO_BYTES_PER_MB * 2;
		int pictures = (int)h->dpb.size + 5;

		while (pictures > (int)h->dpb.size + 3 &&
		       (size_t)pictures * pic_bytes + fixed >
			       h->session->memory_budget) {
			pictures--;
		}
		cfg.frames = pictures;
		cfg.width = (int)(sps->mb_width * 16);
		cfg.height = (int)(sps->mb_height * 16);
		cfg.format = VE_PIX_TILED_420;
		POOL(h) = ve_pool_create(&cfg);
		h->pool_mbw = sps->mb_width;
		h->pool_mbh = sps->mb_height;
		if (POOL(h) == NULL) {
			return -ENOMEM;
		}
	}
	if (h->deblock_buf == NULL) {
		h->deblock_buf = hw_alloc((size_t)sps->mb_width * DEBLOCK_BYTES_PER_MB_COL);
		h->intra_buf = hw_alloc(INTRA_PRED_BASE + (size_t)sps->mb_width * INTRA_PRED_PER_MB_COL);
		h->mb_intra_buf = hw_alloc((size_t)mbs * MB_INFO_BYTES_PER_MB);
		h->mb_neighbor_raw = hw_alloc((size_t)mbs * MB_INFO_BYTES_PER_MB + NEIGHBOR_ALIGN);
		if (h->deblock_buf == NULL || h->intra_buf == NULL || h->mb_intra_buf == NULL ||
		    h->mb_neighbor_raw == NULL) {
			return -ENOMEM;
		}
		h->mb_neighbor_phys = ((uint32_t)(uintptr_t)h->mb_neighbor_raw + NEIGHBOR_ALIGN - 1) &
				      ~(uint32_t)(NEIGHBOR_ALIGN - 1);
		h->mvcol_half = ROUND_UP((size_t)mbs * MVCOL_BYTES_PER_MB, 2048);
		for (unsigned int i = 0; i <= h->dpb.size; i++) {
			h->mvcol_pool[i] = hw_alloc(2 * h->mvcol_half);
			if (h->mvcol_pool[i] == NULL) {
				return -ENOMEM;
			}
		}
	}
	ve_hw_select(VE_HW_H264);
	if (!h->hw_ready) {
		ve_h264_hw_open(&h->hw);
		h->hw_ready = 1;
	}

	return 0;
}

/* ---- output ---- */

static void fill_picture(struct h264 *h, int slot)
{
	struct ve_picture *pic = h->slot[slot].pic;
	const struct h264_sps *sps = h->act_sps;

	/* width and height stay the aligned sizes of the buffer: the conversion to linear output uses them */
	pic->left = (int)sps->crop_left;
	pic->top = (int)sps->crop_top;
	pic->right = (int)(sps->mb_width * 16) - (int)sps->crop_right;
	pic->bottom = (int)(sps->mb_height * 16) - (int)sps->crop_bottom;
	pic->pts = h->dpb.pic[slot].pts;
}

static void output_picture(struct h264 *h, int slot)
{
	struct slot_info *si = &h->slot[slot];

	fill_picture(h, slot);
	if (h->dpb.pic[slot].ref != H264_REF_NONE) {
		ve_pool_share(POOL(h), si->pic);
		si->shared = 1;
	} else {
		ve_pool_submit(POOL(h), si->pic);
		si->pic = NULL;
		memset(&h->dpb.pic[slot], 0, sizeof(h->dpb.pic[slot]));
	}
	h->released = 1;
}

/* Pictures that are neither referenced nor waiting any more give their buffer back */
static void release_unused(struct h264 *h)
{
	for (int i = 0; i < H264_DPB_SLOTS; i++) {
		struct h264_dpb_pic *p = &h->dpb.pic[i];

		if (p->active && !p->is_current && p->ref == H264_REF_NONE && !p->output_needed &&
		    h->slot[i].pic != NULL) {
			release_slot(h, i);
		}
	}
}

static void drain(struct h264 *h, int force)
{
	int o;

	while ((o = h264_dpb_output_next(&h->dpb, force)) >= 0) {
		output_picture(h, o);
	}
	release_unused(h);
}

static void finish_picture(struct h264 *h)
{
	if (h->cur < 0) {
		return;
	}
	h264_dpb_finish_picture(&h->dpb, h->act_sps, &h->cur_hdr, h->cur);
	h->cur = -1;
	drain(h, 0);
}

/* ---- one slice ---- */

static int new_picture(struct h264 *h, const struct h264_slice_hdr *hdr, int64_t pts)
{
	int slot, top, bottom;
	struct ve_picture *pic;

	if (hdr->idr) {
		drain(h, 1);
		for (int i = 0; i < H264_DPB_SLOTS; i++) {
			h->dpb.pic[i].ref = H264_REF_NONE;
		}
		release_unused(h);
		h->dpb.poc.prev_poc_msb = h->dpb.poc.prev_poc_lsb = 0;
	}
	slot = h264_dpb_alloc(&h->dpb);
	while (slot < 0) {
		int o = h264_dpb_output_next(&h->dpb, 1);

		if (o < 0) {
			return -ENOMEM;
		}
		output_picture(h, o);
		release_unused(h);
		slot = h264_dpb_alloc(&h->dpb);
	}
	pic = ve_pool_acquire(POOL(h));
	if (pic == NULL) {
		memset(&h->dpb.pic[slot], 0, sizeof(h->dpb.pic[slot]));
		return -EAGAIN;
	}
	if (h264_dpb_compute_poc(&h->dpb, h->act_sps, hdr, &top, &bottom) != 0) {
		ve_pool_discard(POOL(h), pic);
		memset(&h->dpb.pic[slot], 0, sizeof(h->dpb.pic[slot]));
		return -ENOTSUP;
	}
	h->slot[slot].pic = pic;
	h->slot[slot].shared = 0;
	h->slot[slot].poc_top = top;
	h->slot[slot].poc_bottom = bottom;
	if (h->mvcol_pool[slot] == NULL) {
		ve_pool_discard(POOL(h), pic);
		memset(&h->slot[slot], 0, sizeof(h->slot[slot]));
		memset(&h->dpb.pic[slot], 0, sizeof(h->dpb.pic[slot]));
		return -ENOMEM;
	}
	h->slot[slot].mvcol_top = (uint32_t)(uintptr_t)h->mvcol_pool[slot];
	h->slot[slot].mvcol_bottom = h->slot[slot].mvcol_top + (uint32_t)h->mvcol_half;
	h->dpb.pic[slot].poc = top < bottom ? top : bottom;
	h->dpb.pic[slot].poc_top = top;
	h->dpb.pic[slot].poc_bottom = bottom;
	h->dpb.pic[slot].pts = pts;
	h->cur = slot;
	h->cur_hdr = *hdr;
	h->cur_slices = 0;
	h->cur_pts = pts;

	return 0;
}

static void config_mvcol_all(struct h264 *h)
{
	for (int i = 0; i < H264_DPB_SLOTS; i++) {
		struct ve_h264_mvcol_params mv = {0};

		if (!h->dpb.pic[i].active || h->slot[i].pic == NULL) {
			continue;
		}
		mv.decode_buf_index = (unsigned int)i;
		mv.y_phys = (uint32_t)(uintptr_t)h->slot[i].pic->plane[0];
		mv.c_phys = (uint32_t)(uintptr_t)h->slot[i].pic->plane[1];
		mv.top_mvcol_phys = h->slot[i].mvcol_top;
		mv.bottom_mvcol_phys = h->slot[i].mvcol_bottom;
		ve_h264_hw_config_mvcol(&h->hw, &mv);
	}
}

static void config_refs(struct h264 *h, const struct h264_slice_hdr *hdr)
{
	struct h264_ref_lists lists;
	static struct ve_h264_ref_lists hw_lists;

	if (h264_dpb_build_lists(&h->dpb, h->act_sps, hdr, h->cur, &lists) != 0) {
		return;
	}
	memset(&hw_lists, 0, sizeof(hw_lists));
	hw_lists.list_count = hdr->slice_type == H264_SLICE_B ? 2 : 1;
	for (unsigned int l = 0; l < hw_lists.list_count; l++) {
		hw_lists.count[l] = lists.count[l];
		for (unsigned int i = 0; i < lists.count[l] && i < VE_H264_MAX_REFS; i++) {
			int s = lists.slot[l][i];
			struct ve_h264_ref_pic *rp = &hw_lists.pic[l][i];

			if (s < 0 || h->slot[s].pic == NULL) {
				continue;
			}
			rp->valid = 1;
			rp->buf_index = (unsigned int)s;
			rp->field_poc[0] = h->slot[s].poc_top;
			rp->field_poc[1] = h->slot[s].poc_bottom;
			rp->field_nal_ref_idc[0] = rp->field_nal_ref_idc[1] = 1;
			rp->reference = 3;
			rp->structure = VE_H264_FRAME;
			rp->coded_frame = 1;
			rp->mb_aff_frame = h->act_sps->mb_adaptive_frame_field;
		}
	}
	(void)ve_h264_hw_config_ref_lists(&h->hw, &hw_lists);
}

static void config_weights(struct h264 *h, const struct h264_pps *pps, const struct h264_slice_hdr *hdr)
{
	static struct ve_h264_weights w;

	memset(&w, 0, sizeof(w));
	if (hdr->slice_type == H264_SLICE_B && pps->weighted_bipred_idc == 2) {
		w.luma_log2_denom = w.chroma_log2_denom = 5;
	} else if ((pps->weighted_pred && hdr->slice_type == H264_SLICE_P) ||
		   (pps->weighted_bipred_idc == 1 && hdr->slice_type == H264_SLICE_B)) {
		w.luma_log2_denom = hdr->luma_log2_weight_denom;
		w.chroma_log2_denom = hdr->chroma_log2_weight_denom;
		w.num_l0 = hdr->num_ref_idx_active[0];
		w.num_l1 = hdr->slice_type == H264_SLICE_B ? hdr->num_ref_idx_active[1] : 0;
		for (unsigned int i = 0; i < w.num_l0 && i < VE_H264_MAX_BUFS; i++) {
			const struct h264_weight *s = &hdr->weight[0][i];

			w.l0[i].luma_flag = s->luma_flag;
			w.l0[i].luma_weight = (int16_t)s->luma_weight;
			w.l0[i].luma_offset = (int16_t)s->luma_offset;
			w.l0[i].chroma_flag = s->chroma_flag;
			for (int j = 0; j < 2; j++) {
				w.l0[i].chroma_weight[j] = (int16_t)s->chroma_weight[j];
				w.l0[i].chroma_offset[j] = (int16_t)s->chroma_offset[j];
			}
		}
		for (unsigned int i = 0; i < w.num_l1 && i < VE_H264_MAX_BUFS; i++) {
			const struct h264_weight *s = &hdr->weight[1][i];

			w.l1[i].luma_flag = s->luma_flag;
			w.l1[i].luma_weight = (int16_t)s->luma_weight;
			w.l1[i].luma_offset = (int16_t)s->luma_offset;
			w.l1[i].chroma_flag = s->chroma_flag;
			for (int j = 0; j < 2; j++) {
				w.l1[i].chroma_weight[j] = (int16_t)s->chroma_weight[j];
				w.l1[i].chroma_offset[j] = (int16_t)s->chroma_offset[j];
			}
		}
	} else {
		return;
	}
	ve_h264_hw_config_weights(&h->hw, &w);
}

/*
 * Decode one slice. `payload` is the NAL unit without start code and header byte, in the stream
 * buffer at offset `payload_off`; `emu`/`nemu` are the positions of the removed emulation bytes.
 */
static int decode_slice(struct h264 *h, const struct h264_slice_hdr *hdr, size_t payload_off, size_t sc_len,
			size_t payload_len, const uint32_t *emu, size_t nemu)
{
	const struct h264_sps *sps = h->act_sps;
	const struct h264_pps *pps = &h->pps[hdr->pps_id];
	struct ve_h264_sps_params sp = {
		.mb_width = sps->mb_width,
		.mb_height = sps->mb_height,
		.direct_8x8_inference = sps->direct_8x8_inference,
		.mb_adaptive_frame_field = sps->mb_adaptive_frame_field,
		.frame_mbs_only = sps->frame_mbs_only,
		.profile_idc = sps->profile_idc,
		.scaling_matrix_present = sps->scaling_matrix_present,
	};
	struct ve_h264_pps_params pp = {
		.transform_8x8_mode = pps->transform_8x8_mode,
		.constrained_intra_pred = pps->constrained_intra_pred,
		.weighted_bipred_idc = (int)pps->weighted_bipred_idc,
		.weighted_pred = pps->weighted_pred,
		.num_ref_idx_l0_default = pps->num_ref_idx_default[0],
		.num_ref_idx_l1_default = pps->num_ref_idx_default[1],
		.cabac = pps->cabac,
		.init_qp = pps->init_qp,
		.chroma_qp_index_offset = {pps->chroma_qp_index_offset[0], pps->chroma_qp_index_offset[1]},
		.scaling_matrix_present = pps->scaling_matrix_present,
	};
	struct ve_h264_slice_params sl = {
		.cabac_init_idc = (int)hdr->cabac_init_idc,
		.direct_spatial_mv_pred = hdr->direct_spatial_mv_pred,
		.structure = VE_H264_FRAME,
		.first_slice_in_pic = h->cur_slices == 0,
		.slice_type = hdr->slice_type,
		.is_b_slice = hdr->slice_type == H264_SLICE_B,
		.nal_ref = hdr->nal_ref_idc != 0,
		.first_mb_x = hdr->first_mb % sps->mb_width,
		.first_mb_y = hdr->first_mb / sps->mb_width,
		.slice_beta_offset_div2 = hdr->slice_beta_offset_div2,
		.slice_alpha_c0_offset_div2 = hdr->slice_alpha_c0_offset_div2,
		.disable_deblocking_filter_idc = (int)hdr->disable_deblocking_filter_idc,
		.num_ref_idx_override = hdr->num_ref_idx_override,
		.num_ref_l0 = hdr->num_ref_idx_active[0] ? hdr->num_ref_idx_active[0] : pps->num_ref_idx_default[0],
		.num_ref_l1 = hdr->num_ref_idx_active[1] ? hdr->num_ref_idx_active[1] : pps->num_ref_idx_default[1],
		.slice_qp = pps->init_qp + hdr->slice_qp_delta,
	};
	struct ve_h264_frame_params fp = {
		.structure = VE_H264_FRAME,
		.decode_buf_index = (unsigned int)h->cur,
		.field_poc = {h->slot[h->cur].poc_top, h->slot[h->cur].poc_bottom},
		.mb_aff_frame = sps->mb_adaptive_frame_field,
		.nal_ref_idc = (int)hdr->nal_ref_idc,
	};
	struct ve_h264_stream st;
	const uint8_t *vbv_base = ve_stream_base(h->session->stream);
	uint32_t vbv_size = (uint32_t)ve_stream_size(h->session->stream);
	uint32_t vbv_phys = (uint32_t)(uintptr_t)vbv_base;
	size_t data_byte;
	size_t esc;
	int need_qm;
	uint32_t status = 0;

	/* where the slice data starts in the escaped stream */
	data_byte = (size_t)(hdr->header_bits / 8);
	esc = data_byte;
	for (size_t i = 0; i < nemu; i++) {
		if (emu[i] <= esc) {
			esc++;
		} else {
			break;
		}
	}
	st.offset_bits = (uint32_t)((payload_off + esc) * 8 + (hdr->header_bits & 7));
	st.length_bits = (uint32_t)((payload_off + payload_len) * 8) - st.offset_bits;
	st.buf_phys = ve_h264_encode_addr(vbv_phys);
	st.buf_end_phys = vbv_phys + vbv_size - 1;
	st.ctrl = VBV_DATA_CTRL;

	if (h->hw_broken) {
		h->cur_slices++;
		return 0;
	}
	ve_hw_lock();
	if (h->cur_slices == 0) {
		/* the memory mode for very wide pictures is not used */
		ve_hw_reset(VE_HW_H264, 0);
		ve_h264_hw_init_func_ctrl(&h->hw);
	}
	ve_h264_hw_config_top(&h->hw, 1, (uint32_t)(uintptr_t)h->deblock_buf, (uint32_t)(uintptr_t)h->intra_buf);
	ve_h264_hw_config_slice_header(&h->hw, &sp, &pp, &sl, &need_qm);
	if (need_qm) {
		ve_h264_hw_config_quant_matrix(&h->hw, pps->sm4, pps->sm8);
	}
	ve_h264_hw_config_mb_buffers(&h->hw, (uint32_t)(uintptr_t)h->mb_intra_buf, h->mb_neighbor_phys);
	if (h->cur_slices == 0) {
		ve_h264_hw_set_recon_index(&h->hw, (unsigned int)h->cur);
	}
	config_mvcol_all(h);
	ve_h264_hw_config_frame_info(&h->hw, &fp);
	if (hdr->slice_type != H264_SLICE_I && hdr->slice_type != H264_SLICE_SI) {
		config_refs(h, hdr);
		config_weights(h, pps, hdr);
	}

	/* the slice data is loaded from where the header ended (CABAC data starts on a byte boundary) */
	if (pps->cabac && (st.offset_bits & 7U)) {
		uint32_t adj = 8U - (st.offset_bits & 7U);

		st.offset_bits += adj;
		st.length_bits -= adj;
	}
	ve_h264_hw_config_vbv(&h->hw, &st);
	ve_h264_hw_enable_irq(&h->hw);
	ve_h264_hw_start_slice(&h->hw);
	ve_hw_wait();
	status = ve_h264_hw_take_status(&h->hw);
	ve_hw_unlock();

	h->cur_slices++;
	if (status != 1) {
		LOG_WRN("slice status %u (first mb %u, type %d, %u bits at %u, frame_num %u, idr %d)", status, hdr->first_mb,
			(int)hdr->slice_type, st.length_bits, st.offset_bits, hdr->frame_num, (int)hdr->idr);
		h->hw_broken = 1;
	}

	return 0;
}

#define SLICE_HEAD_BYTES	512

/* ---- decoding ---- */

static int ensure_scratch(struct h264 *h, size_t len)
{
	if (h->rbsp_size < len) {
		uint8_t *n = realloc(h->rbsp, len);

		if (n == NULL) {
			return -ENOMEM;
		}
		h->rbsp = n;
		h->rbsp_size = len;
	}
	if (h->emu_size < len / 3 + 1) {
		uint32_t *n = realloc(h->emu, (len / 3 + 1) * sizeof(uint32_t));

		if (n == NULL) {
			return -ENOMEM;
		}
		h->emu = n;
		h->emu_size = len / 3 + 1;
	}

	return 0;
}

static enum ve_step h264_step(void *ctx, bool eos)
{
	struct h264 *h = ctx;
	struct ve_stream *stream = h->session->stream;
	struct ve_unit *unit;
	const uint8_t *data, *vbv_base;
	size_t sc, len, nemu = 0, rlen;
	unsigned int type, ref_idc;
	enum ve_step ret = VE_STEP_PROGRESS;

	if (h->sps == NULL) {
		h->sps = calloc(H264_MAX_SPS, sizeof(*h->sps));
		h->pps = calloc(H264_MAX_PPS, sizeof(*h->pps));
		if (h->sps == NULL || h->pps == NULL) {
			return VE_STEP_UNSUPPORTED;
		}
	}
	h->released = 0;
	unit = ve_stream_next(stream);
	if (unit == NULL) {
		if (eos) {
			finish_picture(h);
			drain(h, 1);
			return h->released ? VE_STEP_PROGRESS : VE_STEP_NEED_DATA;
		}
		return VE_STEP_NEED_DATA;
	}
	data = unit->data;
	len = (size_t)unit->length;
	vbv_base = ve_stream_base(stream);
	sc = (len > 3 && data[2] == 0) ? 4 : 3;
	if (len <= sc + 1) {
		ve_stream_done(stream, unit);
		return VE_STEP_PROGRESS;
	}
	type = data[sc] & 31U;
	ref_idc = (data[sc] >> 5) & 3U;
	if (ensure_scratch(h, len) != 0) {
		ve_stream_unread(stream, unit);
		return VE_STEP_UNSUPPORTED;
	}
	/*
	 * A slice is parsed only as far as its header: the engine reads the slice data itself and
	 * handles the emulation prevention bytes, so the rest of a (large) NAL unit is not copied.
	 */
	size_t payload = len - sc - 1;
	size_t head = (type == H264_NAL_SLICE || type == H264_NAL_IDR) ? MIN(payload, SLICE_HEAD_BYTES)
									: payload;

	rlen = h264_unescape(h->rbsp, data + sc + 1, head, h->emu, h->emu_size, &nemu);

	switch (type) {
	case H264_NAL_SPS: {
		struct h264_sps t;

		int r = h264_parse_sps(h->rbsp, rlen, &t);

		if (r != 0) {
			LOG_WRN("sps r=%d", r);
		}
		if (r == 0) {
			if (POOL(h) != NULL &&
			    (t.mb_width != h->pool_mbw || t.mb_height != h->pool_mbh)) {
				/* another picture size: deliver the pictures of the old sequence, then stop */
				finish_picture(h);
				drain(h, 1);
				h->size_changed = 1;
			} else {
				h->sps[t.id] = t;
			}
		}
		break;
	}
	case H264_NAL_PPS: {
		struct h264_pps t;

		int r = h264_parse_pps(h->rbsp, rlen, h->sps, &t);

		LOG_DBG("pps r=%d id %u cabac %d", r, t.id, t.cabac);
		if (r == 0) {
			h->pps[t.id] = t;
		}
		break;
	}
	case H264_NAL_SLICE:
	case H264_NAL_IDR: {
		struct h264_slice_hdr hdr;
		int r = h264_parse_slice_header(h->rbsp, rlen, type, ref_idc, h->sps, h->pps, &hdr);

		if ((r != 0 || hdr.header_bits / 8 + 8 > rlen) && head < payload) {
			/* a long header: take the whole unit */
			rlen = h264_unescape(h->rbsp, data + sc + 1, payload, h->emu, h->emu_size, &nemu);
			r = h264_parse_slice_header(h->rbsp, rlen, type, ref_idc, h->sps, h->pps, &hdr);
		}

		if (r != 0) {
			LOG_WRN("slice header r=%d", r);
		}
		if (r != 0) {
			break;
		}
		if (h->size_changed) {
			ret = VE_STEP_UNSUPPORTED;
			break;
		}
		if (hdr.first_mb == 0 && h->cur >= 0) {
			finish_picture(h);
		}
		if (h->cur < 0) {
			const struct h264_sps *sps = &h->sps[h->pps[hdr.pps_id].sps_id];

			if (!h->dpb_ready || h->act_sps != sps) {
				if (start_sequence(h, sps) != 0) {
					ret = VE_STEP_UNSUPPORTED;
					break;
				}
			}
			if (hdr.first_mb != 0) {
				break;	/* the start of this picture was lost */
			}
			r = new_picture(h, &hdr, unit->pts);
			if (r == -EAGAIN) {
				ve_stream_unread(stream, unit);
				return VE_STEP_NEED_PICTURE;
			}
			if (r != 0) {
				ret = VE_STEP_UNSUPPORTED;
				break;
			}
		}
		decode_slice(h, &hdr, (size_t)((data + sc + 1) - vbv_base), sc, payload, h->emu,
			     nemu < h->emu_size ? nemu : h->emu_size);
		break;
	}
	case H264_NAL_AUD:
	case H264_NAL_END_SEQ:
	case H264_NAL_END_STREAM:
		finish_picture(h);
		if (type != H264_NAL_AUD) {
			drain(h, 1);
		}
		break;
	default:
		break;
	}
	ve_stream_done(stream, unit);

	/* a picture that came out is progress even when the unit after it was refused */
	if (h->released) {
		return VE_STEP_PROGRESS;
	}

	return ret;
}

const struct ve_codec ve_h264_codec = {
	.name = "h264",
	.id = VE_CODEC_H264,
	.hw = VE_HW_H264,
	.open = h264_open,
	.close = h264_close,
	.reset = h264_reset,
	.step = h264_step,
};
