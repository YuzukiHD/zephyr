/*
 * SPDX-License-Identifier: Apache-2.0
 */

/* Register level access to the H.264 block of the video engine */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_H264_HW_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_H264_HW_H_

#include <stdint.h>

#include "ve_h264_regs.h"

struct ve_h264_hw {
	volatile uint8_t *reg;		/* H.264 block */
	volatile uint8_t *top;		/* common block */
	uint32_t ver_lo, ver_hi;	/* engine version */
};

/* Where the engine takes its bytes from; offsets and lengths are in bits */
struct ve_h264_stream {
	uint32_t buf_phys;		/* start of the stream buffer */
	uint32_t buf_end_phys;		/* last byte of the buffer */
	uint32_t ctrl;			/* upper address bits / mode, goes to bits 31:28 */
	uint32_t offset_bits;		/* first bit within the buffer */
	uint32_t length_bits;
};

#define VE_H264_REG_FUNC_CTRL	0x20
#define VE_H264_REG_TRIGGER	0x24
#define VE_H264_REG_STATUS	0x28

/* trigger types */
#define VE_H264_TRIGGER_ALL		7	/* configure / start a parsing step */

void ve_h264_hw_open(struct ve_h264_hw *hw);
void ve_h264_hw_init_func_ctrl(struct ve_h264_hw *hw);
void ve_h264_hw_enable_irq(struct ve_h264_hw *hw);
/* Top block: engine side line buffers in DRAM when the on chip ones are not used */
void ve_h264_hw_config_top(struct ve_h264_hw *hw, int use_dram_bufs, uint32_t deblock_phys,
			   uint32_t intra_pred_phys);

/* Sequence level values that go into the SPS register */
struct ve_h264_sps_params {
	unsigned int mb_width, mb_height;	/* in macroblocks */
	int direct_8x8_inference;
	int mb_adaptive_frame_field;
	int frame_mbs_only;
	unsigned int profile_idc;
	int scaling_matrix_present;
};

struct ve_h264_pps_params {
	int transform_8x8_mode;
	int constrained_intra_pred;
	int weighted_bipred_idc;
	int weighted_pred;
	unsigned int num_ref_idx_l0_default, num_ref_idx_l1_default;	/* active counts */
	int cabac;
	int init_qp;
	int chroma_qp_index_offset[2];
	int scaling_matrix_present;
};

enum ve_h264_pic_structure {
	VE_H264_TOP_FIELD = 1,
	VE_H264_BOTTOM_FIELD = 2,
	VE_H264_FRAME = 3,
};

/* Values of the slice being decoded */
struct ve_h264_slice_params {
	int cabac_init_idc;
	int direct_spatial_mv_pred;
	enum ve_h264_pic_structure structure;
	int first_slice_in_pic;
	unsigned int slice_type;		/* as coded in the header */
	int is_b_slice;
	int nal_ref;
	unsigned int first_mb_x, first_mb_y;
	int slice_beta_offset_div2, slice_alpha_c0_offset_div2;
	int disable_deblocking_filter_idc;
	int num_ref_idx_override;
	unsigned int num_ref_l0, num_ref_l1;	/* active counts for this slice */
	int slice_qp;
};

/* Write the SPS, PPS, slice header, deblocking and QP registers for a slice */
void ve_h264_hw_config_slice_header(struct ve_h264_hw *hw, const struct ve_h264_sps_params *sps,
				    const struct ve_h264_pps_params *pps,
				    const struct ve_h264_slice_params *sl, int *needs_quant_matrix);
/* Addresses of the per macroblock side information buffers of the picture */
void ve_h264_hw_config_mb_buffers(struct ve_h264_hw *hw, uint32_t field_intra_phys,
				  uint32_t neighbor_info_phys);
void ve_h264_hw_set_recon_index(struct ve_h264_hw *hw, unsigned int index);
void ve_h264_hw_start_slice(struct ve_h264_hw *hw);

/* Picture being decoded: its picture order counts go to the engine's side memory */
struct ve_h264_frame_params {
	enum ve_h264_pic_structure structure;
	unsigned int decode_buf_index;
	int32_t field_poc[2];
	int mb_aff_frame;
	int nal_ref_idc;
	/* reference and display buffers differ in format */
	int ref_disp_same_format;
	unsigned int disp_format;
	uint32_t ref_c_size;
	uint32_t ref_y_stride, ref_c_stride;
};

void ve_h264_hw_config_frame_info(struct ve_h264_hw *hw, const struct ve_h264_frame_params *f);

#define VE_H264_MAX_REFS	48
#define VE_H264_MAX_BUFS	32

/* A picture in a reference list, as far as the engine registers care */
struct ve_h264_ref_pic {
	int valid;			/* a picture buffer is attached */
	unsigned int buf_index;		/* decode buffer index */
	int32_t field_poc[2];
	uint8_t field_nal_ref_idc[2];
	unsigned int reference;		/* bit 0 top field, bit 1 bottom field used for reference */
	enum ve_h264_pic_structure structure;
	int coded_frame;
	int mb_aff_frame;
};

struct ve_h264_ref_lists {
	unsigned int list_count;		/* 1 for P slices, 2 for B slices */
	unsigned int count[2];
	struct ve_h264_ref_pic pic[2][VE_H264_MAX_REFS];
};

/* Describe the reference lists to the engine; returns the number of distinct buffers used */
unsigned int ve_h264_hw_config_ref_lists(struct ve_h264_hw *hw, const struct ve_h264_ref_lists *l);

/* Prediction weights of a slice; entries without a flag keep the default weight 1 << denom */
struct ve_h264_weight_entry {
	int luma_flag;
	int16_t luma_weight, luma_offset;
	int chroma_flag;
	int16_t chroma_weight[2], chroma_offset[2];
};

struct ve_h264_weights {
	unsigned int luma_log2_denom, chroma_log2_denom;
	unsigned int num_l0, num_l1;		/* entries to send, 0 for none */
	struct ve_h264_weight_entry l0[VE_H264_MAX_BUFS];
	struct ve_h264_weight_entry l1[VE_H264_MAX_BUFS];
};

/* Explicit weighted prediction; for implicit mode pass log2 denominators of 5 and no entries */
void ve_h264_hw_config_weights(struct ve_h264_hw *hw, const struct ve_h264_weights *w);

/* Scaling lists in the order 4x4 (6 lists of 16 bytes), 8x8 (2 lists of 64 bytes) */
void ve_h264_hw_config_quant_matrix(struct ve_h264_hw *hw, const uint8_t sm4[6][16],
				    const uint8_t sm8[2][64]);

/* Picture buffer addresses for motion vector co-location and prediction */
struct ve_h264_mvcol_params {
	unsigned int decode_buf_index;
	int compressed;			/* the picture is stored compressed: no separate chroma address */
	uint32_t y_phys, c_phys;	/* picture planes, buffer offsets already added */
	uint32_t top_mvcol_phys, bottom_mvcol_phys;
};

void ve_h264_hw_config_mvcol(struct ve_h264_hw *hw, const struct ve_h264_mvcol_params *p);

/* Output stage: size reduction, rotation and the output format of the pictures */
struct ve_h264_output_params {
	unsigned int rotate;			/* 0..7 */
	unsigned int scale_horizon, scale_vertical;	/* ratio codes, 0..3 */
	uint32_t scaled_y_phys, scaled_c_phys;
	int yv12, nv21;
	int ref_disp_same_format;
	unsigned int disp_format;
	uint32_t disp_c_size, ref_c_size;
	uint32_t ref_y_stride, ref_c_stride, disp_y_stride, disp_c_stride;
};

void ve_h264_hw_config_output(struct ve_h264_hw *hw, const struct ve_h264_output_params *p);

/*
 * Second output of the decoder: besides the reconstructed (tiled) picture the engine writes a copy
 * as linear NV12, luma at @p y_phys and the interleaved chroma plane at @p c_phys, without
 * scaling or rotation. Call it after the reset that starts a picture; ve_h264_hw_clear_nv12_output()
 * turns the copy off.
 */
void ve_h264_hw_set_nv12_output(struct ve_h264_hw *hw, uint32_t y_phys, uint32_t c_phys);
void ve_h264_hw_clear_nv12_output(struct ve_h264_hw *hw);

/*
 * Take the bytes of the slice from the stream buffer. `s->buf_phys` is the buffer address in the form the
 * engine wants (see ve_h264_encode_addr), offsets and length are in bits.
 */
void ve_h264_hw_config_vbv(struct ve_h264_hw *hw, const struct ve_h264_stream *s);
/* Address word of a physical address: bits 31:28 move to 3:0 */
static inline uint32_t ve_h264_encode_addr(uint32_t phys)
{
	return (phys >> 28) | (phys & 0x0ffffff0U);
}
/* Status of a finished step (bit 0 done, 1 error, 2 stream ran out); the flags are cleared */
uint32_t ve_h264_hw_take_status(struct ve_h264_hw *hw);

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_H264_HW_H_ */
