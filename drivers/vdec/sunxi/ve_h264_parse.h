/*
 * SPDX-License-Identifier: Apache-2.0
 */

/* H.264 syntax: NAL unit splitting, SPS, PPS and slice headers. No hardware access. */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_H264_PARSE_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_H264_PARSE_H_

#include <stddef.h>
#include <stdint.h>

enum ve_h264_nal_type {
	H264_NAL_SLICE = 1,
	H264_NAL_IDR = 5,
	H264_NAL_SEI = 6,
	H264_NAL_SPS = 7,
	H264_NAL_PPS = 8,
	H264_NAL_AUD = 9,
	H264_NAL_END_SEQ = 10,
	H264_NAL_END_STREAM = 11,
};

enum ve_h264_slice_type {
	H264_SLICE_P = 0,
	H264_SLICE_B = 1,
	H264_SLICE_I = 2,
	H264_SLICE_SP = 3,
	H264_SLICE_SI = 4,
};

#define H264_MAX_SPS		32
#define H264_MAX_PPS		256
#define H264_MAX_REFS		32
#define H264_MAX_MMCO		66

struct h264_sps {
	int valid;
	unsigned int profile_idc, level_idc, constraint_flags;
	unsigned int id;
	unsigned int chroma_format_idc;
	unsigned int bit_depth_luma, bit_depth_chroma;
	int scaling_matrix_present;
	/* in the order of the stream (zigzag); flat 16 when not sent */
	uint8_t sm4[6][16];
	uint8_t sm8[2][64];
	unsigned int log2_max_frame_num;
	unsigned int poc_type;
	unsigned int log2_max_poc_lsb;
	int delta_pic_order_always_zero;
	int offset_for_non_ref_pic, offset_for_top_to_bottom_field;
	unsigned int poc_cycle_length;
	int offset_for_ref_frame[256];
	unsigned int max_num_ref_frames;
	int gaps_in_frame_num_allowed;
	unsigned int mb_width, mb_height;	/* height in map units until frame_mbs_only is known */
	int frame_mbs_only;
	int mb_adaptive_frame_field;
	int direct_8x8_inference;
	unsigned int crop_left, crop_right, crop_top, crop_bottom;	/* in luma samples */
	int bitstream_restriction;
	unsigned int num_reorder_frames;
	unsigned int max_dec_frame_buffering;
	unsigned int frame_width, frame_height;	/* cropped size in luma samples */
};

struct h264_pps {
	int valid;
	unsigned int id, sps_id;
	int cabac;
	int bottom_field_pic_order_in_frame_present;
	unsigned int num_ref_idx_default[2];
	int weighted_pred;
	unsigned int weighted_bipred_idc;
	int init_qp, init_qs;
	int chroma_qp_index_offset[2];
	int deblocking_filter_control_present;
	int constrained_intra_pred;
	int redundant_pic_cnt_present;
	int transform_8x8_mode;
	int scaling_matrix_present;
	uint8_t sm4[6][16];
	uint8_t sm8[2][64];
};

struct h264_mmco {
	unsigned int op;
	unsigned int difference_of_pic_nums_minus1;
	unsigned int long_term_pic_num;
	unsigned int long_term_frame_idx;
	unsigned int max_long_term_frame_idx_plus1;
};

struct h264_ref_mod {
	unsigned int idc;
	unsigned int value;	/* abs_diff_pic_num_minus1 or long_term_pic_num */
};

struct h264_weight {
	int luma_flag, chroma_flag;
	int luma_weight, luma_offset;
	int chroma_weight[2], chroma_offset[2];
};

struct h264_slice_hdr {
	unsigned int nal_type, nal_ref_idc;
	int idr;
	unsigned int first_mb;
	unsigned int slice_type;		/* 0..4 */
	unsigned int pps_id;
	unsigned int frame_num;
	int field_pic, bottom_field;
	unsigned int idr_pic_id;
	unsigned int poc_lsb;
	int delta_poc_bottom;
	int delta_poc[2];
	unsigned int redundant_pic_cnt;
	int direct_spatial_mv_pred;
	int num_ref_idx_override;
	unsigned int num_ref_idx_active[2];
	unsigned int ref_mod_count[2];
	struct h264_ref_mod ref_mod[2][H264_MAX_REFS + 1];
	unsigned int luma_log2_weight_denom, chroma_log2_weight_denom;
	struct h264_weight weight[2][H264_MAX_REFS];
	int no_output_of_prior_pics, long_term_reference_flag;
	int adaptive_ref_pic_marking;
	unsigned int mmco_count;
	struct h264_mmco mmco[H264_MAX_MMCO];
	unsigned int cabac_init_idc;
	int slice_qp_delta;
	int sp_for_switch;
	int slice_qs_delta;
	unsigned int disable_deblocking_filter_idc;
	int slice_alpha_c0_offset_div2, slice_beta_offset_div2;
	/* size of the header in bits of the unescaped payload (after the NAL header byte) */
	size_t header_bits;
};

/*
 * Remove emulation prevention bytes from `src` (payload after the NAL header byte).
 * Returns the unescaped length; positions[] gets the index in `src` of every removed byte
 * (up to max_pos entries), *npos the total number.
 */
size_t h264_unescape(uint8_t *dst, const uint8_t *src, size_t len, uint32_t *positions,
		     size_t max_pos, size_t *npos);

/* Find the next NAL unit in an Annex B stream: start of its first byte, length without start code */
int h264_next_nal(const uint8_t *buf, size_t len, size_t *pos, size_t *nal_start, size_t *nal_len);

int h264_parse_sps(const uint8_t *rbsp, size_t len, struct h264_sps *sps);
int h264_parse_pps(const uint8_t *rbsp, size_t len, const struct h264_sps *sps_table,
		   struct h264_pps *pps);
int h264_parse_slice_header(const uint8_t *rbsp, size_t len, unsigned int nal_type,
			    unsigned int nal_ref_idc, const struct h264_sps *sps_table,
			    const struct h264_pps *pps_table, struct h264_slice_hdr *hdr);

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_H264_PARSE_H_ */
