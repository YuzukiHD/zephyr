/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include "ve_h264_parse.h"

/* ---- bit reader ---- */

struct br {
	const uint8_t *p;
	size_t nbits;
	size_t pos;
	int err;
};

static void br_init(struct br *b, const uint8_t *p, size_t len)
{
	b->p = p;
	b->nbits = len * 8;
	b->pos = 0;
	b->err = 0;
}

static unsigned int br_bit(struct br *b)
{
	unsigned int v;

	if (b->pos >= b->nbits) {
		b->err = 1;
		return 0;
	}
	v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1U;
	b->pos++;

	return v;
}

static uint32_t br_u(struct br *b, unsigned int n)
{
	uint32_t v = 0;

	while (n--) {
		v = (v << 1) | br_bit(b);
	}

	return v;
}

static uint32_t br_ue(struct br *b)
{
	unsigned int zeros = 0;
	uint32_t v;

	while (br_bit(b) == 0) {
		if (b->err || ++zeros > 31) {
			b->err = 1;
			return 0;
		}
	}
	v = br_u(b, zeros);

	return ((1U << zeros) - 1U) + v;
}

static int32_t br_se(struct br *b)
{
	uint32_t k = br_ue(b);

	return (k & 1U) ? (int32_t)((k + 1U) >> 1) : -(int32_t)(k >> 1);
}

/* ---- NAL units ---- */

size_t h264_unescape(uint8_t *dst, const uint8_t *src, size_t len, uint32_t *positions,
		     size_t max_pos, size_t *npos)
{
	size_t o = 0, n = 0, zeros = 0;

	for (size_t i = 0; i < len; i++) {
		if (zeros >= 2 && src[i] == 3) {
			if (positions != NULL && n < max_pos) {
				positions[n] = (uint32_t)i;
			}
			n++;
			zeros = 0;
			continue;
		}
		dst[o++] = src[i];
		zeros = src[i] == 0 ? zeros + 1 : 0;
	}
	if (npos != NULL) {
		*npos = n;
	}

	return o;
}

int h264_next_nal(const uint8_t *buf, size_t len, size_t *pos, size_t *nal_start, size_t *nal_len)
{
	size_t i = *pos;
	size_t start;

	while (i + 3 <= len && !(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)) {
		i++;
	}
	if (i + 3 > len) {
		return -ENOENT;
	}
	start = i + 3;
	i = start;
	while (i + 3 <= len && !(buf[i] == 0 && buf[i + 1] == 0 && (buf[i + 2] == 1 || buf[i + 2] == 0))) {
		i++;
	}
	if (i + 3 > len) {
		i = len;
	}
	*nal_start = start;
	*nal_len = i - start;
	*pos = i;

	return 0;
}

/* ---- scaling lists ---- */

static const uint8_t def4_intra[16] = {6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42};
static const uint8_t def4_inter[16] = {10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34};
static const uint8_t def8_intra[64] = {
	6,  10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23, 23, 23, 23, 23, 23, 25,
	25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31,
	31, 31, 31, 31, 31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42,
};
static const uint8_t def8_inter[64] = {
	9,  13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21, 21, 21, 21, 21, 21, 22,
	22, 22, 22, 22, 22, 22, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27,
	27, 27, 27, 27, 27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35,
};

/* returns 1 when the list asks for the default table */
static int scaling_list(struct br *b, uint8_t *list, unsigned int size)
{
	int last = 8, next = 8, use_default = 0;

	for (unsigned int j = 0; j < size; j++) {
		if (next != 0) {
			int delta = br_se(b);

			next = (last + delta + 256) % 256;
			use_default = j == 0 && next == 0;
		}
		list[j] = (uint8_t)(next == 0 ? last : next);
		last = list[j];
	}

	return use_default;
}

static void flat_lists(uint8_t sm4[6][16], uint8_t sm8[2][64])
{
	memset(sm4, 16, 6 * 16);
	memset(sm8, 16, 2 * 64);
}

/*
 * Read the scaling matrix syntax. `fallback4/8` are the lists used for a list that is not sent
 * and has no earlier list of the same kind to copy: NULL selects the default tables (rule A).
 */
static void scaling_matrix(struct br *b, unsigned int nlists, uint8_t sm4[6][16],
			   uint8_t sm8[2][64], const uint8_t fb4[6][16], const uint8_t fb8[2][64])
{
	for (unsigned int i = 0; i < nlists; i++) {
		int present = br_u(b, 1);
		int use_default = 0;

		if (i < 6) {
			if (present) {
				use_default = scaling_list(b, sm4[i], 16);
			}
			if (!present || use_default) {
				const uint8_t *def = i < 3 ? def4_intra : def4_inter;

				if (use_default) {
					memcpy(sm4[i], def, 16);
				} else if (i == 0 || i == 3) {
					memcpy(sm4[i], fb4 != NULL ? fb4[i] : def, 16);
				} else {
					memcpy(sm4[i], sm4[i - 1], 16);
				}
			}
		} else if (i < 8) {
			unsigned int k = i - 6;

			if (present) {
				use_default = scaling_list(b, sm8[k], 64);
			}
			if (!present || use_default) {
				const uint8_t *def = k == 0 ? def8_intra : def8_inter;

				memcpy(sm8[k], use_default || fb8 == NULL ? def : fb8[k], 64);
			}
		} else {
			/* 4:4:4 chroma lists are read and dropped */
			uint8_t tmp[64];

			if (present) {
				scaling_list(b, tmp, i < 12 ? 64 : 16);
			}
		}
	}
}

/* ---- SPS ---- */

static void hrd_parameters(struct br *b)
{
	unsigned int cnt = br_ue(b) + 1;

	br_u(b, 8);
	for (unsigned int i = 0; i < cnt && i < 32 && !b->err; i++) {
		br_ue(b);
		br_ue(b);
		br_u(b, 1);
	}
	br_u(b, 20);
}

static void vui_parameters(struct br *b, struct h264_sps *sps)
{
	int nal_hrd, vcl_hrd;

	if (br_u(b, 1)) {
		if (br_u(b, 8) == 255) {
			br_u(b, 32);
		}
	}
	if (br_u(b, 1)) {
		br_u(b, 1);
	}
	if (br_u(b, 1)) {
		br_u(b, 4);
		if (br_u(b, 1)) {
			br_u(b, 24);
		}
	}
	if (br_u(b, 1)) {
		br_ue(b);
		br_ue(b);
	}
	if (br_u(b, 1)) {
		br_u(b, 32);
		br_u(b, 32);
		br_u(b, 1);
	}
	nal_hrd = br_u(b, 1);
	if (nal_hrd) {
		hrd_parameters(b);
	}
	vcl_hrd = br_u(b, 1);
	if (vcl_hrd) {
		hrd_parameters(b);
	}
	if (nal_hrd || vcl_hrd) {
		br_u(b, 1);
	}
	br_u(b, 1);
	if (br_u(b, 1)) {
		br_u(b, 1);
		br_ue(b);
		br_ue(b);
		br_ue(b);
		br_ue(b);
		sps->bitstream_restriction = 1;
		sps->num_reorder_frames = br_ue(b);
		sps->max_dec_frame_buffering = br_ue(b);
	}
}

int h264_parse_sps(const uint8_t *rbsp, size_t len, struct h264_sps *sps)
{
	struct br b;
	unsigned int profile;
	unsigned int map_height;
	unsigned int sub_w = 2, sub_h = 2;

	memset(sps, 0, sizeof(*sps));
	br_init(&b, rbsp, len);
	profile = br_u(&b, 8);
	sps->profile_idc = profile;
	sps->constraint_flags = br_u(&b, 8);
	sps->level_idc = br_u(&b, 8);
	sps->id = br_ue(&b);
	if (sps->id >= H264_MAX_SPS) {
		return -EINVAL;
	}
	sps->chroma_format_idc = 1;
	sps->bit_depth_luma = sps->bit_depth_chroma = 8;
	flat_lists(sps->sm4, sps->sm8);
	switch (profile) {
	case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128:
	case 138: case 139: case 134: case 135:
		sps->chroma_format_idc = br_ue(&b);
		if (sps->chroma_format_idc == 3 && br_u(&b, 1)) {
			return -ENOTSUP;	/* separate colour planes */
		}
		sps->bit_depth_luma = br_ue(&b) + 8;
		sps->bit_depth_chroma = br_ue(&b) + 8;
		br_u(&b, 1);
		sps->scaling_matrix_present = br_u(&b, 1);
		if (sps->scaling_matrix_present) {
			scaling_matrix(&b, sps->chroma_format_idc != 3 ? 8 : 12, sps->sm4,
				       sps->sm8, NULL, NULL);
		}
		break;
	default:
		break;
	}
	sps->log2_max_frame_num = br_ue(&b) + 4;
	sps->poc_type = br_ue(&b);
	if (sps->poc_type == 0) {
		sps->log2_max_poc_lsb = br_ue(&b) + 4;
	} else if (sps->poc_type == 1) {
		sps->delta_pic_order_always_zero = br_u(&b, 1);
		sps->offset_for_non_ref_pic = br_se(&b);
		sps->offset_for_top_to_bottom_field = br_se(&b);
		sps->poc_cycle_length = br_ue(&b);
		if (sps->poc_cycle_length > 255) {
			return -EINVAL;
		}
		for (unsigned int i = 0; i < sps->poc_cycle_length; i++) {
			sps->offset_for_ref_frame[i] = br_se(&b);
		}
	} else if (sps->poc_type != 2) {
		return -EINVAL;
	}
	sps->max_num_ref_frames = br_ue(&b);
	sps->gaps_in_frame_num_allowed = br_u(&b, 1);
	sps->mb_width = br_ue(&b) + 1;
	map_height = br_ue(&b) + 1;
	sps->frame_mbs_only = br_u(&b, 1);
	if (!sps->frame_mbs_only) {
		sps->mb_adaptive_frame_field = br_u(&b, 1);
	}
	sps->direct_8x8_inference = br_u(&b, 1);
	sps->mb_height = map_height * (2U - sps->frame_mbs_only);
	if (br_u(&b, 1)) {
		sps->crop_left = br_ue(&b);
		sps->crop_right = br_ue(&b);
		sps->crop_top = br_ue(&b);
		sps->crop_bottom = br_ue(&b);
	}
	if (sps->chroma_format_idc == 0) {
		sub_w = sub_h = 1;
	} else if (sps->chroma_format_idc == 2) {
		sub_h = 1;
	} else if (sps->chroma_format_idc == 3) {
		sub_w = sub_h = 1;
	}
	sub_h *= 2U - sps->frame_mbs_only;
	sps->crop_left *= sub_w;
	sps->crop_right *= sub_w;
	sps->crop_top *= sub_h;
	sps->crop_bottom *= sub_h;
	sps->frame_width = sps->mb_width * 16U - sps->crop_left - sps->crop_right;
	sps->frame_height = sps->mb_height * 16U - sps->crop_top - sps->crop_bottom;
	if (br_u(&b, 1)) {
		vui_parameters(&b, sps);
	}
	if (b.err) {
		/* a damaged or truncated VUI does not matter for decoding */
		sps->bitstream_restriction = 0;
	}
	if (sps->mb_width == 0 || sps->mb_width > 512 || sps->mb_height == 0 || sps->mb_height > 512) {
		return -EINVAL;
	}
	sps->valid = 1;

	return 0;
}

/* ---- PPS ---- */

/* True when a 1 bit follows the current position other than the final stop bit */
static int more_rbsp_data(const struct br *b)
{
	size_t last = b->nbits;

	while (last > 0 && !((b->p[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1U)) {
		last--;
	}
	/* `last` is one past the stop bit */

	return last > 0 && b->pos + 1 < last;
}

int h264_parse_pps(const uint8_t *rbsp, size_t len, const struct h264_sps *sps_table,
		   struct h264_pps *pps)
{
	struct br b;
	const struct h264_sps *sps;

	memset(pps, 0, sizeof(*pps));
	br_init(&b, rbsp, len);
	pps->id = br_ue(&b);
	pps->sps_id = br_ue(&b);
	if (pps->id >= H264_MAX_PPS || pps->sps_id >= H264_MAX_SPS) {
		return -EINVAL;
	}
	sps = &sps_table[pps->sps_id];
	if (!sps->valid) {
		return -ENOENT;
	}
	pps->cabac = br_u(&b, 1);
	pps->bottom_field_pic_order_in_frame_present = br_u(&b, 1);
	if (br_ue(&b) != 0) {
		return -ENOTSUP;	/* slice groups */
	}
	pps->num_ref_idx_default[0] = br_ue(&b) + 1;
	pps->num_ref_idx_default[1] = br_ue(&b) + 1;
	pps->weighted_pred = br_u(&b, 1);
	pps->weighted_bipred_idc = br_u(&b, 2);
	pps->init_qp = 26 + br_se(&b);
	pps->init_qs = 26 + br_se(&b);
	pps->chroma_qp_index_offset[0] = br_se(&b);
	pps->deblocking_filter_control_present = br_u(&b, 1);
	pps->constrained_intra_pred = br_u(&b, 1);
	pps->redundant_pic_cnt_present = br_u(&b, 1);
	pps->chroma_qp_index_offset[1] = pps->chroma_qp_index_offset[0];
	memcpy(pps->sm4, sps->sm4, sizeof(pps->sm4));
	memcpy(pps->sm8, sps->sm8, sizeof(pps->sm8));
	/* more data: transform 8x8, scaling matrix, second chroma offset */
	if (more_rbsp_data(&b)) {
		pps->transform_8x8_mode = br_u(&b, 1);
		pps->scaling_matrix_present = br_u(&b, 1);
		if (pps->scaling_matrix_present) {
			unsigned int n = 6 + (sps->chroma_format_idc != 3 ? 2 : 6) *
					     pps->transform_8x8_mode;

			/* rule A when the sequence has no matrix, rule B (sequence lists) otherwise */
			scaling_matrix(&b, n, pps->sm4, pps->sm8,
				       sps->scaling_matrix_present ? sps->sm4 : NULL,
				       sps->scaling_matrix_present ? sps->sm8 : NULL);
		}
		pps->chroma_qp_index_offset[1] = br_se(&b);
	}
	if (b.err) {
		return -EINVAL;
	}
	pps->valid = 1;

	return 0;
}

/* ---- slice header ---- */

static int chroma_array_type(const struct h264_sps *sps)
{
	return sps->chroma_format_idc;
}

static void pred_weight_table(struct br *b, const struct h264_sps *sps, struct h264_slice_hdr *h)
{
	unsigned int lists = h->slice_type == H264_SLICE_B ? 2 : 1;

	h->luma_log2_weight_denom = br_ue(b);
	if (chroma_array_type(sps) != 0) {
		h->chroma_log2_weight_denom = br_ue(b);
	}
	for (unsigned int l = 0; l < lists; l++) {
		for (unsigned int i = 0; i < h->num_ref_idx_active[l] && i < H264_MAX_REFS; i++) {
			struct h264_weight *w = &h->weight[l][i];

			w->luma_weight = 1 << h->luma_log2_weight_denom;
			w->chroma_weight[0] = w->chroma_weight[1] = 1 << h->chroma_log2_weight_denom;
			w->luma_flag = br_u(b, 1);
			if (w->luma_flag) {
				w->luma_weight = br_se(b);
				w->luma_offset = br_se(b);
			}
			if (chroma_array_type(sps) != 0) {
				w->chroma_flag = br_u(b, 1);
				if (w->chroma_flag) {
					for (int j = 0; j < 2; j++) {
						w->chroma_weight[j] = br_se(b);
						w->chroma_offset[j] = br_se(b);
					}
				}
			}
		}
	}
}

static void ref_list_modification(struct br *b, struct h264_slice_hdr *h)
{
	unsigned int lists = h->slice_type == H264_SLICE_B ? 2 : 1;

	if (h->slice_type == H264_SLICE_I || h->slice_type == H264_SLICE_SI) {
		return;
	}
	for (unsigned int l = 0; l < lists; l++) {
		if (!br_u(b, 1)) {
			continue;
		}
		for (;;) {
			unsigned int idc = br_ue(b);

			if (idc == 3 || b->err || h->ref_mod_count[l] >= H264_MAX_REFS) {
				break;
			}
			h->ref_mod[l][h->ref_mod_count[l]].idc = idc;
			h->ref_mod[l][h->ref_mod_count[l]].value = br_ue(b);
			h->ref_mod_count[l]++;
		}
	}
}

static void dec_ref_pic_marking(struct br *b, struct h264_slice_hdr *h)
{
	if (h->idr) {
		h->no_output_of_prior_pics = br_u(b, 1);
		h->long_term_reference_flag = br_u(b, 1);
		return;
	}
	h->adaptive_ref_pic_marking = br_u(b, 1);
	if (!h->adaptive_ref_pic_marking) {
		return;
	}
	for (;;) {
		struct h264_mmco *m;
		unsigned int op = br_ue(b);

		if (op == 0 || b->err || h->mmco_count >= H264_MAX_MMCO) {
			break;
		}
		m = &h->mmco[h->mmco_count++];
		m->op = op;
		if (op == 1 || op == 3) {
			m->difference_of_pic_nums_minus1 = br_ue(b);
		}
		if (op == 2) {
			m->long_term_pic_num = br_ue(b);
		}
		if (op == 3 || op == 6) {
			m->long_term_frame_idx = br_ue(b);
		}
		if (op == 4) {
			m->max_long_term_frame_idx_plus1 = br_ue(b);
		}
	}
}

int h264_parse_slice_header(const uint8_t *rbsp, size_t len, unsigned int nal_type,
			    unsigned int nal_ref_idc, const struct h264_sps *sps_table,
			    const struct h264_pps *pps_table, struct h264_slice_hdr *h)
{
	struct br b;
	const struct h264_pps *pps;
	const struct h264_sps *sps;
	unsigned int type;

	memset(h, 0, sizeof(*h));
	br_init(&b, rbsp, len);
	h->nal_type = nal_type;
	h->nal_ref_idc = nal_ref_idc;
	h->idr = nal_type == H264_NAL_IDR;
	h->first_mb = br_ue(&b);
	type = br_ue(&b);
	h->slice_type = type % 5;
	h->pps_id = br_ue(&b);
	if (h->pps_id >= H264_MAX_PPS || !pps_table[h->pps_id].valid) {
		return -ENOENT;
	}
	pps = &pps_table[h->pps_id];
	sps = &sps_table[pps->sps_id];
	if (!sps->valid) {
		return -ENOENT;
	}
	h->frame_num = br_u(&b, sps->log2_max_frame_num);
	if (!sps->frame_mbs_only) {
		h->field_pic = br_u(&b, 1);
		if (h->field_pic) {
			h->bottom_field = br_u(&b, 1);
		}
	}
	if (h->idr) {
		h->idr_pic_id = br_ue(&b);
	}
	if (sps->poc_type == 0) {
		h->poc_lsb = br_u(&b, sps->log2_max_poc_lsb);
		if (pps->bottom_field_pic_order_in_frame_present && !h->field_pic) {
			h->delta_poc_bottom = br_se(&b);
		}
	}
	if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
		h->delta_poc[0] = br_se(&b);
		if (pps->bottom_field_pic_order_in_frame_present && !h->field_pic) {
			h->delta_poc[1] = br_se(&b);
		}
	}
	if (pps->redundant_pic_cnt_present) {
		h->redundant_pic_cnt = br_ue(&b);
	}
	if (h->slice_type == H264_SLICE_B) {
		h->direct_spatial_mv_pred = br_u(&b, 1);
	}
	h->num_ref_idx_active[0] = pps->num_ref_idx_default[0];
	h->num_ref_idx_active[1] = pps->num_ref_idx_default[1];
	if (h->slice_type == H264_SLICE_P || h->slice_type == H264_SLICE_SP ||
	    h->slice_type == H264_SLICE_B) {
		h->num_ref_idx_override = br_u(&b, 1);
		if (h->num_ref_idx_override) {
			h->num_ref_idx_active[0] = br_ue(&b) + 1;
			if (h->slice_type == H264_SLICE_B) {
				h->num_ref_idx_active[1] = br_ue(&b) + 1;
			}
		}
	} else {
		h->num_ref_idx_active[0] = h->num_ref_idx_active[1] = 0;
	}
	if (h->slice_type != H264_SLICE_B) {
		h->num_ref_idx_active[1] = 0;
	}
	if (h->num_ref_idx_active[0] > H264_MAX_REFS || h->num_ref_idx_active[1] > H264_MAX_REFS) {
		return -EINVAL;
	}
	ref_list_modification(&b, h);
	if ((pps->weighted_pred && (h->slice_type == H264_SLICE_P || h->slice_type == H264_SLICE_SP)) ||
	    (pps->weighted_bipred_idc == 1 && h->slice_type == H264_SLICE_B)) {
		pred_weight_table(&b, sps, h);
	}
	if (nal_ref_idc != 0) {
		dec_ref_pic_marking(&b, h);
	}
	if (pps->cabac && h->slice_type != H264_SLICE_I && h->slice_type != H264_SLICE_SI) {
		h->cabac_init_idc = br_ue(&b);
	}
	h->slice_qp_delta = br_se(&b);
	if (h->slice_type == H264_SLICE_SP || h->slice_type == H264_SLICE_SI) {
		if (h->slice_type == H264_SLICE_SP) {
			h->sp_for_switch = br_u(&b, 1);
		}
		h->slice_qs_delta = br_se(&b);
	}
	if (pps->deblocking_filter_control_present) {
		h->disable_deblocking_filter_idc = br_ue(&b);
		if (h->disable_deblocking_filter_idc != 1) {
			h->slice_alpha_c0_offset_div2 = br_se(&b);
			h->slice_beta_offset_div2 = br_se(&b);
		}
	}
	h->header_bits = b.pos;

	return b.err ? -EINVAL : 0;
}
