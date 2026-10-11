/*
 * SPDX-License-Identifier: Apache-2.0
 */

/* Register layouts of the H.264 block of the video engine (offsets from the block base) */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_H264_REGS_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_H264_REGS_H_

#include <stdint.h>

/*
 * 0x00  MACC_H264_SEQ_HDR: Sequence parameters: picture size in macroblocks, frame or field
 * coding, chroma format, direct 8x8 inference.
 * Register guide: MACC_H264_SEQ_HDR, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_avc_sps {
	uint32_t raw;
	struct {
		uint32_t pic_height_mb_minus1 : 8;
		uint32_t pic_width_mb_minus1 : 8;
		uint32_t direct_8x8_inference_flag : 1;
		uint32_t mb_adaptive_frame_field_flag : 1;
		uint32_t frame_mbs_only_flag : 1;
		uint32_t chroma_format_idc : 3;
		uint32_t r2 : 10;
	};
};

/*
 * 0x04  MACC_H264_PIC_HDR: Picture parameters: entropy coder (CABAC or CAVLC), weighted prediction
 * mode, number of active references, 8x8 transform, constrained intra prediction.
 * Register guide: MACC_H264_PIC_HDR, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_avc_pps {
	uint32_t raw;
	struct {
		uint32_t transform_8x8_mode_flag : 1;
		uint32_t constrained_intra_pred_flag : 1;
		uint32_t weighted_bipred_idc : 2;
		uint32_t weighted_pred_idc : 1;
		uint32_t num_ref_idx_l1_active_minus1_pic : 5;
		uint32_t num_ref_idx_l0_active_minus1_pic : 5;
		uint32_t entropy_coding_mode_flag : 1;
		uint32_t r0 : 16;
	};
};

/*
 * 0x08  MACC_H264_SLICE_HDR: Slice header, first part: slice type, position of the first
 * macroblock, field flags, CABAC initialisation table, reference flag.
 * Register guide: MACC_H264_SLICE_HDR, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_shs {
	uint32_t raw;
	struct {
		uint32_t bCabac_init_idc : 2;
		uint32_t direct_spatial_mv_pred_flag : 1;
		uint32_t bottom_field_flag : 1;
		uint32_t field_pic_flag : 1;
		uint32_t first_slice_in_pic : 1;
		uint32_t r0 : 2;
		uint32_t slice_type : 4;
		uint32_t nal_ref_flag : 1;
		uint32_t r1 : 3;
		uint32_t first_mb_y : 8;
		uint32_t first_mb_x : 8;
	};
};

/*
 * 0x0c  MACC_H264_SLICE_HDR2: Slice header, second part: number of active references of the slice
 * and the deblocking filter parameters.
 * Register guide: MACC_H264_SLICE_HDR2, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_shs2 {
	uint32_t raw;
	struct {
		uint32_t slice_beta_offset_div2 : 4;
		uint32_t slice_alpha_c0_offset_div2 : 4;
		uint32_t disable_deblocking_filter_idc : 2;
		uint32_t r0 : 2;
		uint32_t num_ref_idx_active_override_flag : 1;
		uint32_t r1 : 3;
		uint32_t num_ref_idx_l1_active_minus1_slice : 5;
		uint32_t r2 : 3;
		uint32_t num_ref_idx_l0_active_minus1_slice : 5;
		uint32_t r3 : 3;
	};
};

/*
 * 0x10  MACC_H264_PRED_WEIGHT: Weighted prediction: log2 of the weight denominator for luma and
 * for chroma.
 * Register guide: MACC_H264_PRED_WEIGHT, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_shs_wp {
	uint32_t raw;
	struct {
		uint32_t luma_log2_weight_denom : 3;
		uint32_t r0 : 1;
		uint32_t chroma_log2_weight_denom : 3;
		uint32_t r1 : 25;
	};
};

/*
 * 0x1c  MACC_H264_QP: Quantiser of the slice: QP, the two chroma QP offsets, flat scaling matrix
 * flag.
 * Register guide: MACC_H264_QP, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_shs_qp {
	uint32_t raw;
	struct {
		uint32_t slice_qpy : 6;
		uint32_t r0 : 2;
		uint32_t chroma_qp_index_offset : 6;
		uint32_t r1 : 2;
		uint32_t second_chroma_qp_index_offset : 6;
		uint32_t r2 : 2;
		uint32_t scaling_matix_flat_flag : 1;
		uint32_t r3 : 7;
	};
};

/*
 * 0x20  MACC_H264_CTRL: Control: interrupt enables (slice finished, error, more stream data
 * wanted), whether the reconstructed picture is written, start code and emulation byte handling.
 * Register guide: MACC_H264_CTRL, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_func_ctrl {
	uint32_t raw;
	struct {
		uint32_t slice_decode_finish_interrupt_enable : 1;
		uint32_t decode_error_intrupt_enable : 1;
		uint32_t vld_data_request_interrupt_enable : 1;
		uint32_t r0 : 5;
		uint32_t not_write_recons_pic_flag : 1;
		uint32_t write_scale_rotated_pic_flag : 1;
		uint32_t mcri_cache_enable : 1;
		uint32_t r1 : 13;
		uint32_t eptb_detection_by_pass : 1;
		uint32_t startcode_detect_enable : 1;
		uint32_t AVS_Demulate_Enable : 1;
		uint32_t r2 : 1;
		uint32_t isAVS : 1;
		uint32_t isVP8 : 1;
		uint32_t r3 : 2;
	};
};

/*
 * 0x24  MACC_H264_TRIG: Trigger: the kind of step to start (slice decode, or reading single codes
 * from the stream) and its parameters; writing it starts the step.
 * Register guide: MACC_H264_TRIG, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_trigger_type {
	uint32_t raw;
	struct {
		uint32_t trigger_type_pul : 4;
		uint32_t stcd_type : 2;
		uint32_t r0 : 2;
		uint32_t n_bits : 6;
		uint32_t r1 : 2;
		uint32_t bin_lens : 3;
		uint32_t r2 : 5;
		uint32_t probability : 8;
	};
};

/*
 * 0x28  MACC_H264_STATUS: Status: interrupt flags (write 1 to clear) and the busy bits of the
 * units of the engine.
 * Register guide: MACC_H264_STATUS, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_func_status {
	uint32_t raw;
	struct {
		uint32_t slice_decode_finish_interrupt : 1;
		uint32_t decode_error_interrupt : 1;
		uint32_t vld_data_req_interrupt : 1;
		uint32_t over_time_interrupt : 1;
		uint32_t r0 : 4;
		uint32_t vld_busy : 1;
		uint32_t is_busy : 1;
		uint32_t mvp_busy : 1;
		uint32_t iq_it_bust : 1;
		uint32_t mcri_busy : 1;
		uint32_t intra_pred_busy : 1;
		uint32_t irec_busy : 1;
		uint32_t dblk_busy : 1;
		uint32_t more_data_flag : 1;
		uint32_t vp8_upprob_busy : 1;
		uint32_t vp8_busy : 1;
		uint32_t r1 : 1;
		uint32_t intram_busy : 1;
		uint32_t it_busy : 1;
		uint32_t bs_dma_busy : 1;
		uint32_t wb_busy : 1;
		uint32_t avs_busy : 1;
		uint32_t avs_idct_busy : 1;
		uint32_t r2 : 1;
		uint32_t stcd_busy : 1;
		uint32_t startcode_type : 3;
		uint32_t startcode_detected : 1;
	};
};

/*
 * 0x2c  MACC_H264_CUR_MBNUM: Number of the macroblock the decoder has reached.
 * Register guide: MACC_H264_CUR_MBNUM, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_cur_mb_num {
	uint32_t raw;
	struct {
		uint32_t cur_mb_number : 16;
		uint32_t r0 : 16;
	};
};

/*
 * 0x30  MACC_H264_VLD_ADDR: Bit stream window, address: start of the stream buffer (upper 4 bits
 * apart) and the first/last slice data flags.
 * Register guide: MACC_H264_VLD_ADDR, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_vld_bitstream_addr {
	uint32_t raw;
	struct {
		uint32_t vbv_base_addr_high4 : 4;
		uint32_t vbv_base_addr_low24 : 24;
		uint32_t slice_data_valid : 1;
		uint32_t last_slice_data : 1;
		uint32_t first_slice_data : 1;
		uint32_t r0 : 1;
	};
};

/*
 * 0x34  MACC_H264_VLD_OFFSET: Bit stream window: bit offset of the slice data in the buffer.
 * Register guide: MACC_H264_VLD_OFFSET, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_vld_offset {
	uint32_t raw;
	struct {
		uint32_t vld_bit_offset : 30;
		uint32_t r0 : 2;
	};
};

/*
 * 0x38  MACC_H264_VLD_LEN: Bit stream window: length of the slice data in bits.
 * Register guide: MACC_H264_VLD_LEN, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_vld_bit_length {
	uint32_t raw;
	struct {
		uint32_t vld_bit_length : 29;
		uint32_t r0 : 3;
	};
};

/*
 * 0x3c  MACC_H264_VLD_END: Bit stream window: address of the last byte of the stream buffer.
 * Register guide: MACC_H264_VLD_END, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_vld_end_addr {
	uint32_t raw;
	struct {
		uint32_t vbv_byte_end_addr : 32;
	};
};

/*
 * 0x40  MACC_H264_SDROT_CTRL: Secondary output (scaled or rotated copy of the picture): angle,
 * scaling precision, field selection. Not used by this driver.
 * Register guide: MACC_H264_SDROT_CTRL, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_sd_rotate_ctrl {
	uint32_t raw;
	struct {
		uint32_t rot_angle : 3;
		uint32_t r0 : 5;
		uint32_t scale_precision : 4;
		uint32_t field_scale_mode : 1;
		uint32_t bottom_field_sel : 1;
		uint32_t r1 : 18;
	};
};

/*
 * 0x44  MACC_H264_SDROT_LUMA: Secondary output: address of the luma buffer. Not used by this
 * driver.
 * Register guide: MACC_H264_SDROT_LUMA, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_sd_rotate_buf_addr {
	uint32_t raw;
	struct {
		uint32_t sd_rotate_luma_buf_addr : 32;
	};
};

/*
 * 0x48  MACC_H264_SDROT_CHROMA: Secondary output: address of the chroma buffer. Not used by this
 * driver.
 * Register guide: MACC_H264_SDROT_CHROMA, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_sd_rotate_chroma_buf_addr {
	uint32_t raw;
	struct {
		uint32_t sd_rotate_chroma_buf_addr : 32;
	};
};

/*
 * 0x4c  MACC_H264_OUTPUT_FRAME_INDEX: Index in the reference list of the buffer that receives the
 * reconstructed picture.
 * Register guide: MACC_H264_OUTPUT_FRAME_INDEX, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_shs_recon_frmbuf_index {
	uint32_t raw;
	struct {
		uint32_t cur_reconstruct_frame_buf_index : 5;
		uint32_t r0 : 27;
	};
};

/*
 * 0x50  MACC_H264_FIELD_INTRA_INFO_BUF: Address of the buffer that holds the intra information of
 * the macroblocks.
 * Register guide: MACC_H264_FIELD_INTRA_INFO_BUF, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_mb_field_intra_info_addr {
	uint32_t raw;
	struct {
		uint32_t mb_field_intra_info_addr : 32;
	};
};

/*
 * 0x54  MACC_H264_NEIGHBOR_INFO_BUF: Address of the buffer that holds the neighbour information of
 * the macroblocks (aligned to 16 KiB).
 * Register guide: MACC_H264_NEIGHBOR_INFO_BUF, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_mb_neighbor_info_addr {
	uint32_t raw;
	struct {
		uint32_t mb_neighbor_info_addr : 32;
	};
};

/*
 * 0x60  MACC_H264_MB_ADDR: Position (x, y) of the macroblock being decoded.
 * Register guide: MACC_H264_MB_ADDR, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_mb_addr {
	uint32_t raw;
	struct {
		uint32_t mb_y : 7;
		uint32_t r0 : 1;
		uint32_t mb_x : 7;
		uint32_t r1 : 17;
	};
};

/*
 * 0x64  MACC_H264_MB_NB1: State of the macroblocks next to the current one (transform size,
 * macroblock types), first part. Diagnostics.
 * Register guide: MACC_H264_MB_NB1, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_mb_neighbor1 {
	uint32_t raw;
	struct {
		uint32_t r0 : 1;
		uint32_t trans_size_8X8_flag_cur_minus1 : 1;
		uint32_t trans_size_8X8_flag_b : 1;
		uint32_t trans_size_8X8_flag_b1 : 1;
		uint32_t trans_size_8X8_flag_A : 1;
		uint32_t trans_size_8X8_flag_A1 : 1;
		uint32_t sp_mb_type_cur_minus1 : 2;
		uint32_t sp_mb_type_d : 2;
		uint32_t sp_mb_type_d1 : 2;
		uint32_t sp_mb_type_c : 2;
		uint32_t sp_mb_type_c1 : 2;
		uint32_t sp_mb_type_b : 2;
		uint32_t sp_mb_type_b1 : 2;
		uint32_t sp_mb_type_a : 2;
		uint32_t sp_mb_type_a1 : 2;
		uint32_t mb_field_flag_d : 1;
		uint32_t mb_field_flag_c : 1;
		uint32_t mb_field_flag_b : 1;
		uint32_t mb_field_flag_a : 1;
		uint32_t mb_avalible_d : 1;
		uint32_t mb_avalible_c : 1;
		uint32_t mb_avalible_b : 1;
		uint32_t mb_avalible_a : 1;
	};
};

/*
 * 0x68  MACC_H264_MB_NB2: State of the neighbours: coded block flags and patterns. Diagnostics.
 * Register guide: MACC_H264_MB_NB2, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_mb_neighbor2 {
	uint32_t raw;
	struct {
		uint32_t cbf_a : 4;
		uint32_t cbf_a1 : 4;
		uint32_t cbf_b : 4;
		uint32_t cbf_b1 : 4;
		uint32_t cbf_cur_minus1 : 4;
		uint32_t cbp_a : 2;
		uint32_t cbp_a1 : 2;
		uint32_t cbp_b : 2;
		uint32_t cbp_b1 : 2;
		uint32_t vbp_cur_minus1 : 2;
		uint32_t r0 : 2;
	};
};

/*
 * 0x6c  MACC_H264_MB_NB3: State of the neighbours: QP of the macroblock to the left (luma, Cb,
 * Cr). Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_mb_neighbor3 {
	uint32_t raw;
	struct {
		uint32_t qpy_a : 6;
		uint32_t r0 : 2;
		uint32_t qpcb_a : 6;
		uint32_t r1 : 2;
		uint32_t qpcr_a : 6;
		uint32_t r2 : 10;
	};
};

/*
 * 0x70  MACC_H264_MB_NB4: State of the neighbours: QP of the second macroblock to the left.
 * Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_mb_neighbor4 {
	uint32_t raw;
	struct {
		uint32_t qpy_a1 : 6;
		uint32_t r0 : 2;
		uint32_t qpcb_a1 : 6;
		uint32_t r1 : 2;
		uint32_t qpcr_a1 : 6;
		uint32_t r2 : 10;
	};
};

/*
 * 0x74  MACC_H264_MB_NB5: State of the neighbours: QP of the macroblock above. Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_mb_neighbor5 {
	uint32_t raw;
	struct {
		uint32_t qpy_b : 6;
		uint32_t r0 : 2;
		uint32_t qpcb_b : 6;
		uint32_t r1 : 2;
		uint32_t qpcr_b : 6;
		uint32_t r2 : 10;
	};
};

/*
 * 0x78  MACC_H264_MB_NB6: State of the neighbours: QP of the second macroblock above. Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_mb_neighbor6 {
	uint32_t raw;
	struct {
		uint32_t qpy_b1 : 6;
		uint32_t r0 : 2;
		uint32_t qpcb_b : 6;
		uint32_t r1 : 2;
		uint32_t qpcr_b1 : 6;
		uint32_t r2 : 10;
	};
};

/*
 * 0x7c  MACC_H264_MB_NB7: State of the neighbours: QP of the previous macroblock. Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_mb_neighbor7 {
	uint32_t raw;
	struct {
		uint32_t qpy_minus1 : 6;
		uint32_t r0 : 2;
		uint32_t qpcb_minus1 : 6;
		uint32_t r1 : 2;
		uint32_t qp_cr_cur_minus1 : 6;
		uint32_t r2 : 10;
	};
};

/*
 * 0x80  MACC_H264_MB_NB8: Edge information that goes from the motion vector unit to the deblocking
 * filter. Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_mb_mvbs_info {
	uint32_t raw;
	struct {
		uint32_t mvp2dblk_edge_info : 32;
	};
};

/*
 * 0x84  Current macroblock when it is inter predicted: types, sub types, direct and skip flags.
 * Shares the offset with the intra layout below. Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_mb_inter_cur_info {
	uint32_t raw;
	struct {
		uint32_t sub_mb_type0 : 4;
		uint32_t sub_mb_type1 : 4;
		uint32_t sub_mb_type2 : 4;
		uint32_t sub_mb_type3 : 4;
		uint32_t inter_mb_type : 6;
		uint32_t mb_direct_8x8_flag : 1;
		uint32_t mb_direct_8x8_flag1 : 1;
		uint32_t mb_direct_8x8_flag2 : 1;
		uint32_t mb_direct_8x8_flag3 : 1;
		uint32_t mb_direct_flag : 1;
		uint32_t mb_skip_flag : 1;
		uint32_t r0 : 1;
		uint32_t transform_size_8x8_flag : 1;
		uint32_t mb_intra_flag : 1;
		uint32_t mb_field_flag : 1;
	};
};

/*
 * 0x84  Current macroblock when it is intra predicted: macroblock type, 16x16 and chroma
 * prediction modes. Shares the offset with the inter layout above. Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_mb_intra_cur_info {
	uint32_t raw;
	struct {
		uint32_t intra_chroma_pred_mode : 2;
		uint32_t r0 : 2;
		uint32_t intra_16x16_pred_mode : 2;
		uint32_t r1 : 10;
		uint32_t intra_mb_type : 2;
		uint32_t r2 : 11;
		uint32_t transform_size_8x8_flag : 1;
		uint32_t mb_intra_flag : 1;
		uint32_t mb_field_flag : 1;
	};
};

/*
 * 0x8c  Coded block pattern of the current macroblock. Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_mb_cbp_cur {
	uint32_t raw;
	struct {
		uint32_t r0 : 24;
		uint32_t cbp_cur : 6;
		uint32_t r1 : 2;
	};
};

/*
 * 0x90  MACC_H264_MB_QP: QP of the current macroblock (luma, Cb, Cr). Diagnostics.
 * Register guide: MACC_H264_MB_QP, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_mb_qp_cur {
	uint32_t raw;
	struct {
		uint32_t qpy_cur : 6;
		uint32_t r0 : 2;
		uint32_t qpcb_cur : 6;
		uint32_t r1 : 2;
		uint32_t qpcr_cur : 6;
		uint32_t r2 : 10;
	};
};

/*
 * 0x94  Temporal direct prediction: scale factors 0 and 1 of the current macroblock. Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_mb_distscale_cur1 {
	uint32_t raw;
	struct {
		uint32_t distscale_factor0 : 11;
		uint32_t r0 : 4;
		uint32_t distscale_factor0_valid_flag : 1;
		uint32_t distscale_factor1 : 11;
		uint32_t r1 : 4;
		uint32_t distscale_factor1_valid_flag : 1;
	};
};

/*
 * 0x98  Temporal direct prediction: scale factors 2 and 3 of the current macroblock. Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_mb_distscale_cur2 {
	uint32_t raw;
	struct {
		uint32_t distscale_factor2 : 11;
		uint32_t r0 : 4;
		uint32_t distscale_factor2_valid_flag : 1;
		uint32_t distscale_factor3 : 11;
		uint32_t r1 : 4;
		uint32_t distscale_factor3_valid_flag : 1;
	};
};

/*
 * 0x9c  Motion vector and reference index of the current macroblock, list 0. Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_cur_mv_mv_l0 {
	uint32_t raw;
	struct {
		uint32_t mv_x_l0 : 14;
		uint32_t mv_y_l0 : 12;
		uint32_t ref_idx_l0 : 6;
	};
};

/*
 * 0xa0  Motion vector and reference index of the current macroblock, list 1. Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_cur_mv_mv_l1 {
	uint32_t raw;
	struct {
		uint32_t mv_x_l1 : 14;
		uint32_t mv_y_l1 : 12;
		uint32_t ref_idx_l1 : 6;
	};
};

/*
 * 0xa4  Intra prediction modes of the first eight blocks of the current macroblock. Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_cur_mb_intra_pred_mode1 {
	uint32_t raw;
	struct {
		uint32_t block_8x8_4x4_0_mode : 4;
		uint32_t block_8x8_4x4_1_mode : 4;
		uint32_t block_8x8_4x4_2_mode : 4;
		uint32_t block_8x8_4x4_3_mode : 4;
		uint32_t block_4x4_4_mode : 4;
		uint32_t block_4x4_5_mode : 4;
		uint32_t block_4x4_6_mode : 4;
		uint32_t block_4x4_7_mode : 4;
	};
};

/*
 * 0xa8  Intra prediction modes of the last eight 4x4 blocks of the current macroblock.
 * Diagnostics.
 * The register guide lists this offset without a name (https://linux-sunxi.org/VE_Register_guide);
 * the fields are what this driver uses.
 */
union ve_h264_cur_mb_intra_pred_mode2 {
	uint32_t raw;
	struct {
		uint32_t block_4x4_8_mode : 4;
		uint32_t block_4x4_9_mode : 4;
		uint32_t block_4x4_10_mode : 4;
		uint32_t block_4x4_11_mode : 4;
		uint32_t block_4x4_12_mode : 4;
		uint32_t block_4x4_13_mode : 4;
		uint32_t block_4x4_14_mode : 4;
		uint32_t block_4x4_15_mode : 4;
	};
};

/*
 * 0xb8  MACC_H264_ERROR: Error flags of the last step: stream ended early, macroblock header
 * error, bad reference index, block error.
 * Register guide: MACC_H264_ERROR, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_error_case {
	uint32_t raw;
	struct {
		uint32_t no_more_data_error : 1;
		uint32_t mbh_error : 1;
		uint32_t ref_idx_error : 1;
		uint32_t block_error : 1;
		uint32_t r0 : 28;
	};
};

/*
 * 0xc0  Slice queue: address of the command list and the enables of its interrupt and of the queue
 * mode.
 * Not in the register guide (https://linux-sunxi.org/VE_Register_guide); the fields are what this
 * driver uses.
 */
union ve_h264_slice_queue_cmd_addr {
	uint32_t raw;
	struct {
		uint32_t slice_queue_cmd_addr_28_31 : 4;
		uint32_t r0 : 4;
		uint32_t slice_queue_cmd_addr_8_27 : 20;
		uint32_t r1 : 2;
		uint32_t slice_queue_decode_interrupt_enable : 1;
		uint32_t slice_queue_decode_enable : 1;
	};
};

/*
 * 0xc4  Slice queue: length and offset of the data of the queued slices.
 * Not in the register guide (https://linux-sunxi.org/VE_Register_guide); the fields are what this
 * driver uses.
 */
union ve_h264_slice_queue_length {
	uint32_t raw;
	struct {
		uint32_t slice_queue_decode_length : 16;
		uint32_t slice_queue_decode_offset : 16;
	};
};

/*
 * 0xc8  Slice queue: starts the decoding of one queued slice.
 * Not in the register guide (https://linux-sunxi.org/VE_Register_guide); the fields are what this
 * driver uses.
 */
union ve_h264_slice_queue_start {
	uint32_t raw;
	struct {
		uint32_t one_slice_queue_decode_start : 1;
		uint32_t r0 : 31;
	};
};

/*
 * 0xcc  Slice queue: interrupt flag, command error and busy bits.
 * Not in the register guide (https://linux-sunxi.org/VE_Register_guide); the fields are what this
 * driver uses.
 */
union ve_h264_slice_queue_status {
	uint32_t raw;
	struct {
		uint32_t slice_queue_decode_interrupt : 1;
		uint32_t r0 : 23;
		uint32_t slice_queue_cmd_err : 1;
		uint32_t r1 : 6;
		uint32_t slice_queue_busy : 1;
	};
};

/*
 * 0xdc  MACC_H264_BASIC_BITS_DATA: Value of the bits read from the stream by a trigger that reads
 * single codes.
 * Register guide: MACC_H264_BASIC_BITS_DATA, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_basic_bits_return_data {
	uint32_t raw;
	struct {
		uint32_t basic_bits_data : 32;
	};
};

/*
 * 0xe0  MACC_H264_RAM_WRITE_PTR: Port to the table memory of the block (weights, scaling lists,
 * reference tables): address, which counts up after each access to the data register.
 * Register guide: MACC_H264_RAM_WRITE_PTR, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_sram_port_rw_offset {
	uint32_t raw;
	struct {
		uint32_t sram_addr : 32;
	};
};

/*
 * 0xe4  MACC_H264_RAM_WRITE_DATA: Port to the table memory: the data register.
 * Register guide: MACC_H264_RAM_WRITE_DATA, https://linux-sunxi.org/VE_Register_guide
 */
union ve_h264_sram_port_rw_data {
	uint32_t raw;
	struct {
		uint32_t sram_data : 32;
	};
};

/*
 * 0xe4  One entry of the reference table behind the table memory port: whether the reference is a
 * frame or fields, and the type of each field. Written through the data register above.
 * Not in the register guide (https://linux-sunxi.org/VE_Register_guide); the fields are what this
 * driver uses.
 */
union ve_h264_frame_struct_ref_info {
	uint32_t raw;
	struct {
		uint32_t top_ref_type : 2;
		uint32_t r0 : 2;
		uint32_t bot_ref_type : 2;
		uint32_t r1 : 2;
		uint32_t frm_struct : 2;
		uint32_t r2 : 22;
	};
};

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_H264_REGS_H_ */
