/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/arch/cpu.h>

#include "ve_hw.h"
#include "ve_h264_hw.h"


/* function control register */
#define FUNC_CTRL_RESET_VALUE	0x400
#define FUNC_CTRL_IRQ_MASK	0x7
#define FUNC_CTRL_BIT_NO_DMA	(1U << 24)	/* the stream unit does not read through DMA */
#define FUNC_CTRL_BIT_25	(1U << 25)

/* status register */
#define STATUS_BS_DMA_BUSY	(1U << 22)
#define BS_DMA_BUSY_POLLS	0x2711

/*
 * Stream registers of the H.264 block (MACC_H264_VLD_ADDR, _OFFSET, _LEN and _END in the register
 * guide, https://linux-sunxi.org/VE_Register_guide)
 */
#define REG_VLD_ADDR		0x30
#define REG_VLD_OFFSET		0x34
#define REG_VLD_LENGTH		0x38
#define REG_VLD_END		0x3c

/*
 * Registers of the common block that the H.264 decoder needs (MACC_VE_IPD_DBLK_BUF_CTRL, _IPD_BUF
 * and _DBLK_BUF in the register guide, https://linux-sunxi.org/VE_Register_guide). The guide gives 0x54 to the intra prediction buffer
 * and 0x58 to the deblocking buffer, the other way round than the names here; the order here is
 * the one the decoder was verified with on this chip.
 */
#define TOP_MODE		0x50
#define TOP_DEBLOCK_BUF		0x54
#define TOP_INTRA_PRED_BUF	0x58

void ve_h264_hw_open(struct ve_h264_hw *hw)
{
	uint64_t version = ve_hw_version();

	hw->reg = ve_hw_regs(VE_HW_H264_REGS);
	hw->top = ve_hw_regs(VE_HW_TOP);
	hw->ver_lo = (uint32_t)version;
	hw->ver_hi = (uint32_t)(version >> 32);
}

void ve_h264_hw_init_func_ctrl(struct ve_h264_hw *hw)
{
	sys_write32(FUNC_CTRL_RESET_VALUE, (mem_addr_t)(hw->reg) + (VE_H264_REG_FUNC_CTRL));
}

void ve_h264_hw_enable_irq(struct ve_h264_hw *hw)
{
	sys_write32(sys_read32((mem_addr_t)(hw->reg) + (VE_H264_REG_FUNC_CTRL)) | FUNC_CTRL_IRQ_MASK, (mem_addr_t)(hw->reg) + (VE_H264_REG_FUNC_CTRL));
}

/* Engine versions whose common block selects the line buffer mode 2 */
static int version_uses_mode2(const struct ve_h264_hw *hw)
{
	if (hw->ver_hi == 0 && (hw->ver_lo == 0x1625 || hw->ver_lo == 0x1663)) {
		return 1;
	}

	return hw->ver_lo == 0x10210 && hw->ver_hi == 0x11120;
}

void ve_h264_hw_config_top(struct ve_h264_hw *hw, int use_dram_bufs, uint32_t deblock_phys,
			   uint32_t intra_pred_phys)
{
	uint32_t v;
	uint32_t mode = version_uses_mode2(hw) ? 2 : 1;

	if (!use_dram_bufs) {
		return;
	}
	v = sys_read32((mem_addr_t)(hw->top) + (TOP_MODE));
	v = (v & ~0x3U) | mode;
	sys_write32(v, (mem_addr_t)(hw->top) + (TOP_MODE));
	v = sys_read32((mem_addr_t)(hw->top) + (TOP_MODE));
	v = (v & ~0xcU) | (mode << 2);
	sys_write32(v, (mem_addr_t)(hw->top) + (TOP_MODE));
	sys_write32(deblock_phys, (mem_addr_t)(hw->top) + (TOP_DEBLOCK_BUF));
	sys_write32(intra_pred_phys, (mem_addr_t)(hw->top) + (TOP_INTRA_PRED_BUF));
}

static inline uint32_t bit1(int v)
{
	return (uint32_t)v & 1U;
}

/* Profiles with the high profile syntax (chroma format, scaling lists) */
static int profile_has_scaling_lists(unsigned int profile)
{
	unsigned int d = profile - 100U;

	return d <= 28U && ((0x10040001U >> d) & 1U);
}

void ve_h264_hw_config_slice_header(struct ve_h264_hw *hw, const struct ve_h264_sps_params *sps,
				    const struct ve_h264_pps_params *pps,
				    const struct ve_h264_slice_params *sl, int *needs_quant_matrix)
{
	union ve_h264_avc_sps r_sps = {.raw = 0};
	union ve_h264_avc_pps r_pps = {.raw = 0};
	union ve_h264_shs r_shs = {.raw = 0};
	union ve_h264_shs2 r_shs2 = {.raw = 0};
	union ve_h264_shs_qp r_qp = {.raw = 0};

	r_sps.pic_height_mb_minus1 = sps->mb_height - 1;
	r_sps.pic_width_mb_minus1 = sps->mb_width - 1;
	r_sps.direct_8x8_inference_flag = bit1(sps->direct_8x8_inference);
	r_sps.mb_adaptive_frame_field_flag = bit1(sps->mb_adaptive_frame_field);
	r_sps.frame_mbs_only_flag = bit1(sps->frame_mbs_only);
	r_sps.chroma_format_idc = 1;
	sys_write32(r_sps.raw, (mem_addr_t)(hw->reg) + (0x00));

	r_pps.transform_8x8_mode_flag = bit1(pps->transform_8x8_mode);
	r_pps.constrained_intra_pred_flag = bit1(pps->constrained_intra_pred);
	r_pps.weighted_bipred_idc = pps->weighted_bipred_idc & 3;
	r_pps.weighted_pred_idc = bit1(pps->weighted_pred);
	r_pps.num_ref_idx_l1_active_minus1_pic = (pps->num_ref_idx_l1_default - 1) & 31;
	r_pps.num_ref_idx_l0_active_minus1_pic = (pps->num_ref_idx_l0_default - 1) & 31;
	r_pps.entropy_coding_mode_flag = bit1(pps->cabac);
	sys_write32(r_pps.raw, (mem_addr_t)(hw->reg) + (0x04));

	r_shs.bCabac_init_idc = sl->cabac_init_idc & 3;
	r_shs.direct_spatial_mv_pred_flag = bit1(sl->direct_spatial_mv_pred);
	r_shs.bottom_field_flag = sl->structure == VE_H264_BOTTOM_FIELD;
	r_shs.field_pic_flag = sl->structure != VE_H264_FRAME;
	r_shs.first_slice_in_pic = sl->first_slice_in_pic == 1;
	r_shs.slice_type = sl->slice_type & 15;
	r_shs.nal_ref_flag = bit1(sl->nal_ref);
	r_shs.first_mb_y = sl->first_mb_y;
	r_shs.first_mb_x = sl->first_mb_x;
	sys_write32(r_shs.raw, (mem_addr_t)(hw->reg) + (0x08));

	r_shs2.slice_beta_offset_div2 = sl->slice_beta_offset_div2 & 15;
	r_shs2.slice_alpha_c0_offset_div2 = sl->slice_alpha_c0_offset_div2 & 15;
	r_shs2.disable_deblocking_filter_idc = sl->disable_deblocking_filter_idc & 3;
	r_shs2.num_ref_idx_active_override_flag = bit1(sl->num_ref_idx_override);
	if (sl->is_b_slice) {
		r_shs2.num_ref_idx_l1_active_minus1_slice = (sl->num_ref_l1 - 1) & 31;
	}
	r_shs2.num_ref_idx_l0_active_minus1_slice = (sl->num_ref_l0 - 1) & 31;
	sys_write32(r_shs2.raw, (mem_addr_t)(hw->reg) + (0x0c));

	r_qp.slice_qpy = sl->slice_qp & 63;
	r_qp.chroma_qp_index_offset = pps->chroma_qp_index_offset[0] & 63;
	r_qp.second_chroma_qp_index_offset = pps->chroma_qp_index_offset[1] & 63;
	r_qp.scaling_matix_flat_flag = 1;
	*needs_quant_matrix = 0;
	if (profile_has_scaling_lists(sps->profile_idc) &&
	    (sps->scaling_matrix_present == 1 || pps->scaling_matrix_present == 1)) {
		*needs_quant_matrix = 1;
		r_qp.scaling_matix_flat_flag = 0;
	}
	sys_write32(r_qp.raw, (mem_addr_t)(hw->reg) + (0x1c));
}

void ve_h264_hw_config_mb_buffers(struct ve_h264_hw *hw, uint32_t field_intra_phys,
				  uint32_t neighbor_info_phys)
{
	sys_write32(field_intra_phys, (mem_addr_t)(hw->reg) + (0x50));
	sys_write32(neighbor_info_phys, (mem_addr_t)(hw->reg) + (0x54));
}

void ve_h264_hw_set_recon_index(struct ve_h264_hw *hw, unsigned int index)
{
	sys_write32(0, (mem_addr_t)(hw->reg) + (0x2c));
	sys_write32(0, (mem_addr_t)(hw->reg) + (0x60));
	sys_write32(index & 31U, (mem_addr_t)(hw->reg) + (0x4c));
}

void ve_h264_hw_start_slice(struct ve_h264_hw *hw)
{
	sys_write32(sys_read32((mem_addr_t)(hw->top) + (0x1c)) | 0x40U, (mem_addr_t)(hw->top) + (0x1c));
	sys_write32(8, (mem_addr_t)(hw->reg) + (VE_H264_REG_TRIGGER));
}

/* Side memory of the engine, reached through an address / data port */
#define SRAM_PORT_OFFSET	0xe0
#define SRAM_PORT_DATA		0xe4

static void sram_write(struct ve_h264_hw *hw, uint32_t offset, uint32_t value)
{
	sys_write32(offset, (mem_addr_t)(hw->reg) + (SRAM_PORT_OFFSET));
	sys_write32(value, (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
}

void ve_h264_hw_config_frame_info(struct ve_h264_hw *hw, const struct ve_h264_frame_params *f)
{
	union ve_h264_frame_struct_ref_info info = {.raw = 0};
	uint32_t idx = f->decode_buf_index;
	int non_ref = f->nal_ref_idc == 0;

	switch (f->structure) {
	case VE_H264_FRAME:
		sys_write32((idx + 32U) << 5, (mem_addr_t)(hw->reg) + (SRAM_PORT_OFFSET));
		sys_write32((uint32_t)f->field_poc[0], (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
		sys_write32((uint32_t)f->field_poc[1], (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
		info.frm_struct = f->mb_aff_frame != 0 ? 2 : 0;
		info.top_ref_type = non_ref ? 2 : 0;
		info.bot_ref_type = non_ref ? 2 : 0;
		break;
	case VE_H264_TOP_FIELD:
		sram_write(hw, (idx + 32U) << 5, (uint32_t)f->field_poc[0]);
		sys_write32((idx << 5) + 1032U, (mem_addr_t)(hw->reg) + (SRAM_PORT_OFFSET));
		info.frm_struct = 1;
		info.top_ref_type = non_ref ? 2 : 0;
		break;
	case VE_H264_BOTTOM_FIELD:
		sram_write(hw, (idx << 5) + 1028U, (uint32_t)f->field_poc[1]);
		info.frm_struct = 1;
		info.bot_ref_type = non_ref ? 2 : 0;
		break;
	default:
		return;
	}
	sys_write32(info.raw, (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));

	if (f->ref_disp_same_format == 1) {
		uint32_t v = sys_read32((mem_addr_t)(hw->top) + (0xec));

		sys_write32((v & ~0x70U) | ((f->disp_format & 7U) << 4), (mem_addr_t)(hw->top) + (0xec));
		sys_write32(f->ref_c_size, (mem_addr_t)(hw->top) + (0xc4));
		sys_write16((uint16_t)f->ref_y_stride, (mem_addr_t)(hw->top) + 0xc8);
		sys_write16((uint16_t)f->ref_c_stride, (mem_addr_t)(hw->top) + 0xca);
	}
}

struct ref_frame_regs {
	int used;
	unsigned int buf_index;
	int32_t first_field_poc, second_field_poc;
	unsigned int frame_struct, top_ref_type, bottom_ref_type;
};

static void calc_ref_frame_regs(struct ref_frame_regs *r, const struct ve_h264_ref_pic *p)
{
	/* both fields (3), the top field (1) or the bottom field (2) are referenced */
	if (p->reference == 3) {
		r->first_field_poc = p->field_poc[0];
		r->second_field_poc = p->field_poc[1];
		r->top_ref_type = p->field_nal_ref_idc[0] == 0 ? 2 : 0;
		r->bottom_ref_type = p->field_nal_ref_idc[1] == 0 ? 2 : 0;
	} else if (p->reference == 1) {
		r->first_field_poc = p->field_poc[0];
		r->top_ref_type = p->field_nal_ref_idc[0] == 0 ? 2 : 0;
	} else if (p->reference == 2) {
		r->second_field_poc = p->field_poc[1];
		r->bottom_ref_type = p->field_nal_ref_idc[1] == 0 ? 2 : 0;
	}
	r->frame_struct = p->coded_frame == 0 ? 1 : (p->mb_aff_frame ? 2 : 0);
}

static void write_ref_frame_regs(struct ve_h264_hw *hw, const struct ref_frame_regs *r)
{
	union ve_h264_frame_struct_ref_info info = {.raw = 0};

	sys_write32((r->buf_index + 32U) << 5, (mem_addr_t)(hw->reg) + (SRAM_PORT_OFFSET));
	sys_write32((uint32_t)r->first_field_poc, (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
	sys_write32((uint32_t)r->second_field_poc, (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
	info.frm_struct = r->frame_struct & 3;
	info.top_ref_type = r->top_ref_type & 3;
	info.bot_ref_type = r->bottom_ref_type & 3;
	sys_write32(info.raw, (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
}

#define REF_LIST_SRAM_BASE	1600
#define REF_LIST_SRAM_STRIDE	36

unsigned int ve_h264_hw_config_ref_lists(struct ve_h264_hw *hw, const struct ve_h264_ref_lists *l)
{
	struct ref_frame_regs regs[VE_H264_MAX_BUFS] = {0};
	uint8_t used[VE_H264_MAX_BUFS];
	unsigned int nused = 0;

	/* one set of registers per distinct buffer */
	for (unsigned int li = 0; li < l->list_count && li < 2; li++) {
		for (unsigned int k = 0; k < l->count[li]; k++) {
			const struct ve_h264_ref_pic *p = &l->pic[li][k];
			unsigned int idx;

			if (k >= VE_H264_MAX_REFS || !p->valid || p->buf_index >= VE_H264_MAX_BUFS) {
				continue;
			}
			idx = p->buf_index;
			calc_ref_frame_regs(&regs[idx], p);
			regs[idx].buf_index = idx;
			used[nused] = (uint8_t)idx;
			nused += regs[idx].used == 0;
			regs[idx].used = 1;
		}
	}
	for (unsigned int i = 0; i < nused; i++) {
		write_ref_frame_regs(hw, &regs[used[i]]);
	}

	/* per list: one byte per entry, (buffer index << 1) | bottom field */
	for (unsigned int li = 0; li < l->list_count && li < 2; li++) {
		uint32_t words[VE_H264_MAX_REFS / 4 + 1] = {0};
		unsigned int count = l->count[li];

		if (count == 0) {
			continue;
		}
		for (int k = (int)count - 1; k >= 0; k--) {
			const struct ve_h264_ref_pic *p = &l->pic[li][k];

			if (k >= VE_H264_MAX_REFS || !p->valid) {
				continue;
			}
			words[k >> 2] = (words[k >> 2] << 8) |
					(p->buf_index << 1) | (p->structure == VE_H264_BOTTOM_FIELD);
		}
		sys_write32(REF_LIST_SRAM_BASE + REF_LIST_SRAM_STRIDE * li, (mem_addr_t)(hw->reg) + (SRAM_PORT_OFFSET));
		for (unsigned int i = 0; i <= count / 4; i++) {
			sys_write32(words[i], (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
		}
	}

	return nused;
}

#define WEIGHT_L0_LUMA		0
#define WEIGHT_L0_CHROMA	128
#define WEIGHT_L1_LUMA		384
#define WEIGHT_L1_CHROMA	512

static uint32_t pack_weight(int16_t weight, int16_t offset)
{
	return (uint32_t)(uint16_t)weight | ((uint32_t)(uint16_t)offset << 16);
}

static void send_weights(struct ve_h264_hw *hw, const struct ve_h264_weight_entry *e,
			 unsigned int count, uint32_t luma_base, uint32_t chroma_base)
{
	for (unsigned int i = 0; i < count; i++) {
		if (e[i].luma_flag) {
			sys_write32(luma_base + i * 4U, (mem_addr_t)(hw->reg) + (SRAM_PORT_OFFSET));
			sys_write32(pack_weight(e[i].luma_weight, e[i].luma_offset), (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
		}
		if (e[i].chroma_flag) {
			sys_write32(chroma_base + i * 8U, (mem_addr_t)(hw->reg) + (SRAM_PORT_OFFSET));
			sys_write32(pack_weight(e[i].chroma_weight[0], e[i].chroma_offset[0]), (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
			sys_write32(pack_weight(e[i].chroma_weight[1], e[i].chroma_offset[1]), (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
		}
	}
}

void ve_h264_hw_config_weights(struct ve_h264_hw *hw, const struct ve_h264_weights *w)
{
	union ve_h264_shs_wp wp = {.raw = 0};
	uint32_t luma_def = 1U << w->luma_log2_denom;
	uint32_t chroma_def = 1U << w->chroma_log2_denom;

	wp.luma_log2_weight_denom = w->luma_log2_denom & 7;
	wp.chroma_log2_weight_denom = w->chroma_log2_denom & 7;
	sys_write32(wp.raw, (mem_addr_t)(hw->reg) + (0x10));

	/* defaults for all entries of both lists: luma, chroma (cb, cr), luma, chroma */
	sys_write32(0, (mem_addr_t)(hw->reg) + (SRAM_PORT_OFFSET));
	for (int i = 0; i < 32; i++) {
		sys_write32(luma_def, (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
	}
	for (int i = 0; i < 64; i++) {
		sys_write32(chroma_def, (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
	}
	for (int i = 0; i < 32; i++) {
		sys_write32(luma_def, (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
	}
	for (int i = 0; i < 64; i++) {
		sys_write32(chroma_def, (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
	}
	send_weights(hw, w->l0, w->num_l0, WEIGHT_L0_LUMA, WEIGHT_L0_CHROMA);
	send_weights(hw, w->l1, w->num_l1, WEIGHT_L1_LUMA, WEIGHT_L1_CHROMA);
}

#define QUANT_8X8_BASE		0x800	/* two lists of 64 bytes */
#define QUANT_4X4_BASE		0x880	/* six lists of 16 bytes */

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Frame zig-zag scans: position in the list -> raster position */
static const uint8_t zigzag4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const uint8_t zigzag8[64] = {
	0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* The lists come in scan order; the engine takes them in raster order */
void ve_h264_hw_config_quant_matrix(struct ve_h264_hw *hw, const uint8_t sm4[6][16],
				    const uint8_t sm8[2][64])
{
	uint8_t r[64];

	for (int l = 0; l < 2; l++) {
		for (int i = 0; i < 64; i++) {
			r[zigzag8[i]] = sm8[l][i];
		}
		sys_write32(QUANT_8X8_BASE + 64U * l, (mem_addr_t)(hw->reg) + (SRAM_PORT_OFFSET));
		for (int i = 0; i < 16; i++) {
			sys_write32(le32(&r[i * 4]), (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
		}
	}
	for (int l = 0; l < 6; l++) {
		for (int i = 0; i < 16; i++) {
			r[zigzag4[i]] = sm4[l][i];
		}
		sys_write32(QUANT_4X4_BASE + 16U * l, (mem_addr_t)(hw->reg) + (SRAM_PORT_OFFSET));
		for (int i = 0; i < 4; i++) {
			sys_write32(le32(&r[i * 4]), (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
		}
	}
}

void ve_h264_hw_config_mvcol(struct ve_h264_hw *hw, const struct ve_h264_mvcol_params *p)
{
	uint32_t second = p->compressed ? p->y_phys : p->c_phys;

	sys_write32((p->decode_buf_index << 5) + 1036U, (mem_addr_t)(hw->reg) + (SRAM_PORT_OFFSET));
	sys_write32(p->y_phys, (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
	sys_write32(second, (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
	sys_write32(p->top_mvcol_phys, (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
	sys_write32(p->bottom_mvcol_phys, (mem_addr_t)(hw->reg) + (SRAM_PORT_DATA));
}

#define REG_ROTATE_CTRL		0x40
#define REG_ROTATE_Y		0x44
#define REG_ROTATE_C		0x48

#define TOP_OUT_CTRL		0xe8
#define TOP_OUT_FORMAT		0xec

void ve_h264_hw_config_output(struct ve_h264_hw *hw, const struct ve_h264_output_params *p)
{
	uint32_t v;

	v = (p->rotate & 7U) | ((p->scale_vertical & 3U) << 10) | ((p->scale_horizon & 3U) << 8);
	sys_write32(v, (mem_addr_t)(hw->reg) + (REG_ROTATE_CTRL));
	sys_write32(p->scaled_y_phys, (mem_addr_t)(hw->reg) + (REG_ROTATE_Y));
	sys_write32(p->scaled_c_phys, (mem_addr_t)(hw->reg) + (REG_ROTATE_C));
	v = sys_read32((mem_addr_t)(hw->reg) + (VE_H264_REG_FUNC_CTRL)) | 0x200U;
	sys_write32(v & ~0x100U, (mem_addr_t)(hw->reg) + (VE_H264_REG_FUNC_CTRL));

	sys_write32(sys_read32((mem_addr_t)(hw->top) + (TOP_OUT_CTRL)) & 0x3fffffffU, (mem_addr_t)(hw->top) + (TOP_OUT_CTRL));
	if (p->yv12) {
		v = sys_read32((mem_addr_t)(hw->top) + (TOP_OUT_CTRL)) | 0xc0000000U | (1U << 29);
		v = (v & 0xf0000000U) | (p->disp_c_size & 0x0fffffffU);
		sys_write32(v, (mem_addr_t)(hw->top) + (TOP_OUT_CTRL));
	} else if (p->nv21) {
		sys_write32((sys_read32((mem_addr_t)(hw->top) + (TOP_OUT_CTRL)) & 0x3fffffffU) | (1U << 30), (mem_addr_t)(hw->top) + (TOP_OUT_CTRL));
		v = sys_read32((mem_addr_t)(hw->top) + (TOP_OUT_FORMAT)) & ~0x70U;
		sys_write32((v & ~7U) | 5U, (mem_addr_t)(hw->top) + (TOP_OUT_FORMAT));
	} else if (p->ref_disp_same_format == 1) {
		uint32_t fmt = p->disp_format;

		v = sys_read32((mem_addr_t)(hw->top) + (TOP_OUT_FORMAT)) & ~0x70U;
		sys_write32(v | ((fmt & 7U) << 4), (mem_addr_t)(hw->top) + (TOP_OUT_FORMAT));
		sys_write32(p->ref_c_size, (mem_addr_t)(hw->top) + (0xc4));
		sys_write16((uint16_t)p->ref_y_stride, (mem_addr_t)(hw->top) + 0xc8);
		sys_write16((uint16_t)p->ref_c_stride, (mem_addr_t)(hw->top) + 0xca);
		sys_write16((uint16_t)p->disp_y_stride, (mem_addr_t)(hw->top) + 0xcc);
		sys_write16((uint16_t)p->disp_c_stride, (mem_addr_t)(hw->top) + 0xce);
		v = sys_read32((mem_addr_t)(hw->top) + (TOP_OUT_CTRL)) & 0xf0000000U;
		sys_write32(v | (p->disp_c_size & 0x0fffffffU), (mem_addr_t)(hw->top) + (TOP_OUT_CTRL));
		v = sys_read32((mem_addr_t)(hw->top) + (TOP_OUT_FORMAT)) & ~7U;
		if (fmt - 2U < 2U) {
			sys_write32(v | (fmt & 7U), (mem_addr_t)(hw->top) + (TOP_OUT_FORMAT));
			sys_write32((sys_read32((mem_addr_t)(hw->top) + (TOP_OUT_CTRL)) & 0x3fffffffU) | (fmt << 30), (mem_addr_t)(hw->top) + (TOP_OUT_CTRL));
		} else {
			sys_write32(v | (fmt & 7U), (mem_addr_t)(hw->top) + (TOP_OUT_FORMAT));
			sys_write32((sys_read32((mem_addr_t)(hw->top) + (TOP_OUT_CTRL)) & 0x3fffffffU) | (1U << 30), (mem_addr_t)(hw->top) + (TOP_OUT_CTRL));
		}
	}
}

void ve_h264_hw_set_nv12_output(struct ve_h264_hw *hw, uint32_t y_phys, uint32_t c_phys)
{
	uint32_t v;

	/* no rotation, no size reduction */
	sys_write32(0, (mem_addr_t)(hw->reg) + (REG_ROTATE_CTRL));
	sys_write32(y_phys, (mem_addr_t)(hw->reg) + (REG_ROTATE_Y));
	sys_write32(c_phys, (mem_addr_t)(hw->reg) + (REG_ROTATE_C));
	/* write the second picture as well as the reconstructed one */
	v = sys_read32((mem_addr_t)(hw->reg) + (VE_H264_REG_FUNC_CTRL)) | 0x200U;
	sys_write32(v & ~0x100U, (mem_addr_t)(hw->reg) + (VE_H264_REG_FUNC_CTRL));
	/* its format: the one named by the low bits of the format register, 4 is NV12 */
	sys_write32((sys_read32((mem_addr_t)(hw->top) + (TOP_OUT_CTRL)) & 0x3fffffffU) | (1U << 30),
		    (mem_addr_t)(hw->top) + (TOP_OUT_CTRL));
	v = sys_read32((mem_addr_t)(hw->top) + (TOP_OUT_FORMAT)) & ~7U;
	sys_write32(v | 4U, (mem_addr_t)(hw->top) + (TOP_OUT_FORMAT));
}

void ve_h264_hw_clear_nv12_output(struct ve_h264_hw *hw)
{
	uint32_t v = sys_read32((mem_addr_t)(hw->reg) + (VE_H264_REG_FUNC_CTRL));

	sys_write32(v & ~0x200U, (mem_addr_t)(hw->reg) + (VE_H264_REG_FUNC_CTRL));
}

void ve_h264_hw_config_vbv(struct ve_h264_hw *hw, const struct ve_h264_stream *s)
{
	uint32_t v = sys_read32((mem_addr_t)(hw->reg) + (VE_H264_REG_FUNC_CTRL));

	sys_write32(v & ~(FUNC_CTRL_BIT_25 | FUNC_CTRL_BIT_NO_DMA), (mem_addr_t)(hw->reg) + (VE_H264_REG_FUNC_CTRL));
	sys_write32(s->offset_bits, (mem_addr_t)(hw->reg) + (REG_VLD_OFFSET));
	sys_write32(s->length_bits, (mem_addr_t)(hw->reg) + (REG_VLD_LENGTH));
	sys_write32(s->buf_end_phys, (mem_addr_t)(hw->reg) + (REG_VLD_END));
	sys_write32((s->ctrl << 28) | s->buf_phys, (mem_addr_t)(hw->reg) + (REG_VLD_ADDR));
	sys_write32(VE_H264_TRIGGER_ALL, (mem_addr_t)(hw->reg) + (VE_H264_REG_TRIGGER));
}

uint32_t ve_h264_hw_take_status(struct ve_h264_hw *hw)
{
	uint32_t st = sys_read32((mem_addr_t)(hw->reg) + (VE_H264_REG_STATUS));

	sys_write32(st, (mem_addr_t)(hw->reg) + (VE_H264_REG_STATUS));

	return st & 7U;
}
