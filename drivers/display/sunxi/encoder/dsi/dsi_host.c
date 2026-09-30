// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * sunxi MIPI DSI host controller, register level programming.
 *
 * Only this file touches the DSI host registers. The encoder/host glue
 * (encoder/dsi/dsi.c) decides what to program and when.
 */
#define DPY_LOG_TAG "dsi-hw"
#include <dpy/dpy_log.h>

#include "dsi_host.h"
#include "dsi_host_regs.h"

#define DSI_PH_DT		DPY_GENMASK(5, 0)
#define DSI_PH_VC		DPY_GENMASK(7, 6)
#define DSI_PH_WC		DPY_GENMASK(23, 8)
#define DSI_PH_ECC		DPY_GENMASK(31, 24)

static inline void dsi_write(struct dsi_hw *hw, uint32_t reg, uint32_t val)
{
	dpy_writel(val, hw->base + reg);
}

static inline uint32_t dsi_read(struct dsi_hw *hw, uint32_t reg)
{
	return dpy_readl(hw->base + reg);
}

static inline void dsi_update(struct dsi_hw *hw, uint32_t reg, uint32_t mask,
			      uint32_t val)
{
	dpy_updatel(hw->base + reg, mask, val);
}

/* packet header word: data type, virtual channel, 16 bit field, ECC */
static uint32_t dsi_header(uint8_t dt, uint8_t vc, uint16_t wc)
{
	uint32_t ph = DPY_FIELD_PREP(DSI_PH_DT, dt) |
		      DPY_FIELD_PREP(DSI_PH_VC, vc) |
		      DPY_FIELD_PREP(DSI_PH_WC, wc);

	return ph | DPY_FIELD_PREP(DSI_PH_ECC, dpy_dsi_ecc(ph));
}

static uint32_t dsi_bits_per_pixel(uint32_t format)
{
	return dpy_dsi_format_bpp(format);
}

static void dsi_set_inst(struct dsi_hw *hw, uint32_t slot, uint32_t mode,
			 uint32_t packet, bool clock, uint32_t data_lanes)
{
	uint32_t reg = slot < 8 ? DSI_INST_FUNC(slot) : DSI_INST_FUNC1(slot);
	uint32_t val = DPY_FIELD_PREP(DSI_INST_MODE, mode) |
		       DPY_FIELD_PREP(DSI_INST_PACKET, packet) |
		       DPY_FIELD_PREP(DSI_INST_LANE_DEN, data_lanes) |
		       (clock ? DSI_INST_LANE_CEN : 0);

	if (mode == DSI_MODE_ESCAPE)
		val |= DPY_FIELD_PREP(DSI_INST_ESCAPE_ENTRY, DSI_ESCAPE_LPDT);
	dsi_write(hw, reg, val);
}

#define DSI_SLOT(slot, next)	((uint32_t)(next) << (4 * (slot)))

static void dsi_config_instructions(struct dsi_hw *hw,
				    const struct dsi_hw_cfg *cfg)
{
	const struct dpy_display_mode *m = &cfg->mode;
	uint32_t lanes = (1U << cfg->lanes) - 1;
	uint32_t n1;

	/* bank 0: normal operation */
	dsi_set_inst(hw, DSI_INST_LP11, DSI_MODE_STOP, 0, true, lanes);
	dsi_set_inst(hw, DSI_INST_TBA, DSI_MODE_TBA, 0, false, 0x1);
	dsi_set_inst(hw, DSI_INST_HSC, DSI_MODE_HS, DSI_PACKET_PIXEL, true, 0);
	dsi_set_inst(hw, DSI_INST_HSD, DSI_MODE_HS, DSI_PACKET_PIXEL, false,
		     lanes);
	dsi_set_inst(hw, DSI_INST_LPDT, DSI_MODE_ESCAPE, DSI_PACKET_COMMAND,
		     false, 0x1);
	dsi_set_inst(hw, DSI_INST_HSCEXIT, DSI_MODE_HSCEXIT, 0, true, 0);
	dsi_set_inst(hw, DSI_INST_NOP, DSI_MODE_STOP, 0, false, lanes);
	dsi_set_inst(hw, DSI_INST_DLY, DSI_MODE_NOP, 0, true, lanes);

	/* bank 1: same with an initial skew calibration step */
	dsi_set_inst(hw, DSI_INST_LP11_1, DSI_MODE_STOP, 0, true, lanes);
	dsi_set_inst(hw, DSI_INST_HSC_1, DSI_MODE_HS, DSI_PACKET_PIXEL, true, 0);
	dsi_set_inst(hw, DSI_INST_DS_1, DSI_MODE_SCINIT, DSI_PACKET_PIXEL,
		     false, lanes);
	dsi_set_inst(hw, DSI_INST_LPDT_1, DSI_MODE_ESCAPE, DSI_PACKET_COMMAND,
		     false, 0x1);
	dsi_set_inst(hw, DSI_INST_HSCEXIT_1, DSI_MODE_HSCEXIT, 0, true, 0);
	dsi_set_inst(hw, DSI_INST_NOP_1, DSI_MODE_STOP, 0, false, lanes);
	dsi_set_inst(hw, DSI_INST_DLY_1, DSI_MODE_NOP, 0, true, lanes);

	/* the stop and delay slots loop (select loop counters 2 and 3) */
	dsi_write(hw, DSI_INST_LOOP_SEL, DSI_SLOT(DSI_INST_LP11, 2) |
		  DSI_SLOT(DSI_INST_DLY, 3));
	dsi_write(hw, DSI_INST_LOOP_SEL1, DSI_SLOT(DSI_INST_LP11_1 - 8, 2) |
		  DSI_SLOT(DSI_INST_DLY_1 - 8, 3));

	switch (cfg->dsi_mode) {
	case DPY_DSI_COMMAND:
		dsi_write(hw, DSI_INST_LOOP_NUM,
			  DPY_FIELD_PREP(DSI_INST_LOOP_N0, 50) |
			  DPY_FIELD_PREP(DSI_INST_LOOP_N1, 50));
		break;
	case DPY_DSI_VIDEO_BURST:
		/* stay in LP for the horizontal blank, counted in module clock
		 * (MHz) ticks */
		n1 = (uint32_t)(m->htotal - m->hdisplay) *
		     DPY_DIV_ROUND_CLOSEST(cfg->mod_clk_hz, 1000000U) /
		     (m->clock / 1000 * 8);
		n1 = n1 > 50 ? n1 - 50 : 1;
		dsi_write(hw, DSI_INST_LOOP_NUM,
			  DPY_FIELD_PREP(DSI_INST_LOOP_N0, 50 - 1) |
			  DPY_FIELD_PREP(DSI_INST_LOOP_N1, n1));
		break;
	default:
		dsi_write(hw, DSI_INST_LOOP_NUM,
			  DPY_FIELD_PREP(DSI_INST_LOOP_N0, 50 - 1) |
			  DPY_FIELD_PREP(DSI_INST_LOOP_N1, 50 - 1));
		break;
	}
	dsi_write(hw, DSI_INST_LOOP_NUM2, dsi_read(hw, DSI_INST_LOOP_NUM));

	/*
	 * In command mode the NOP slot jumps to HS clock exit after one
	 * frame of lines; in video mode the jump is armed only to pause.
	 */
	dsi_write(hw, DSI_INST_JUMP_CFG(0),
		  DPY_FIELD_PREP(DSI_JUMP_CFG_POINT, DSI_INST_NOP) |
		  DPY_FIELD_PREP(DSI_JUMP_CFG_TO, DSI_INST_HSCEXIT) |
		  (cfg->dsi_mode == DPY_DSI_COMMAND ?
		   DSI_JUMP_CFG_EN |
		   DPY_FIELD_PREP(DSI_JUMP_CFG_NUM, m->vdisplay) :
		   DPY_FIELD_PREP(DSI_JUMP_CFG_NUM, 1)));
}

static void dsi_config_basic(struct dsi_hw *hw, const struct dsi_hw_cfg *cfg)
{
	const struct dpy_display_mode *m = &cfg->mode;
	uint32_t bpp = dsi_bits_per_pixel(cfg->format);
	uint32_t hbp = m->htotal - m->hsync_start;	/* includes sync */
	uint32_t basic = 0;

	dsi_write(hw, DSI_TRANS_START, 10);
	dsi_write(hw, DSI_TRANS_ZERO, 0);

	if (cfg->dsi_mode == DPY_DSI_COMMAND) {
		dsi_write(hw, DSI_BASIC_CTL0, DSI_CTL0_ECC_EN |
			  DSI_CTL0_CRC_EN | DSI_CTL0_HS_EOTP_EN);
		dsi_write(hw, DSI_BASIC_CTL1, 0);
		dsi_write(hw, DSI_BASIC_CTL, 0);
		return;
	}

	dsi_write(hw, DSI_BASIC_CTL0, DSI_CTL0_ECC_EN | DSI_CTL0_CRC_EN);
	/* the TCON holds the start delay; request data one line early */
	dsi_write(hw, DSI_BASIC_CTL1, DSI_CTL1_VIDEO_MODE |
		  DSI_CTL1_VIDEO_FRAME_START | DSI_CTL1_VIDEO_PREC_ALIGN |
		  DPY_FIELD_PREP(DSI_CTL1_VIDEO_START_DELAY, 1) |
		  (cfg->slave ? DPY_FIELD_PREP(DSI_CTL1_TRI_DELAY, 48) : 0));

	if (cfg->dsi_mode == DPY_DSI_VIDEO_BURST) {
		uint32_t sync_point = 40;
		uint32_t line_num, edge0, edge1;

		line_num = m->htotal * bpp / (8 * cfg->lanes) * 10 / 9;
		edge1 = sync_point + (m->hdisplay + hbp + 20) * bpp /
			(8 * cfg->lanes);
		edge1 = DPY_MIN(edge1, line_num);
		edge0 = edge1 + (m->hdisplay + 40) * 4 / 8;
		edge0 = edge0 > line_num ? edge0 - line_num : 1;
		dsi_write(hw, DSI_BURST_DRQ,
			  DPY_FIELD_PREP(DSI_BURST_DRQ_EDGE0, edge0) |
			  DPY_FIELD_PREP(DSI_BURST_DRQ_EDGE1, edge1));
		dsi_write(hw, DSI_TCON_DRQ, DSI_DRQ_MODE);
		dsi_write(hw, DSI_BURST_LINE,
			  DPY_FIELD_PREP(DSI_BURST_LINE_NUM, line_num) |
			  DPY_FIELD_PREP(DSI_BURST_SYNC_POINT, sync_point));
		basic |= DSI_BASIC_VIDEO_BURST;
		if (cfg->lanes == 4)
			basic |= DPY_FIELD_PREP(DSI_BASIC_TRAIL_INV, 0xc) |
				 DSI_BASIC_TRAIL_FILL;
	} else {
		uint32_t hfp = m->htotal - m->hdisplay - hbp;

		if (hfp < 21)
			dsi_write(hw, DSI_TCON_DRQ, 0);
		else
			dsi_write(hw, DSI_TCON_DRQ, DSI_DRQ_MODE |
				  DPY_FIELD_PREP(DSI_DRQ_SET,
						 (hfp - 20) * bpp / (8 * 4)));
	}
	if (cfg->slave)
		basic |= DSI_BASIC_START_MODE;
	dsi_write(hw, DSI_BASIC_CTL, basic);
}

static void dsi_write_blank(struct dsi_hw *hw, uint32_t reg0, uint32_t reg1,
			    uint8_t vc, uint32_t size)
{
	dsi_write(hw, reg0, dsi_header(DPY_DSI_BLANKING_PACKET, vc,
				       (uint16_t)size));
	dsi_write(hw, reg1, DPY_FIELD_PREP(DSI_BLK_PD, 0) |
		  DPY_FIELD_PREP(DSI_BLK_PF, dpy_dsi_crc_repeat(0, size)));
}

/*
 * In the non-burst video modes every horizontal blanking period is sent
 * as a packet; it has to be long enough to carry the packet overheads
 * subtracted in dsi_config_packets().
 */
int dsi_hw_check_mode(const struct dsi_hw_cfg *cfg)
{
	const struct dpy_display_mode *m = &cfg->mode;
	uint32_t bpp = dsi_bits_per_pixel(cfg->format);

	if (cfg->dsi_mode == DPY_DSI_COMMAND ||
	    cfg->dsi_mode == DPY_DSI_VIDEO_BURST)
		return 0;
	if ((m->hsync_end - m->hsync_start) * bpp / 8 <= 4 + 4 + 2 ||
	    (m->htotal - m->hsync_end) * bpp / 8 <= 10 ||
	    (m->hsync_start - m->hdisplay) * bpp / 8 <= 6 + 6)
		return -EINVAL;
	return 0;
}

static void dsi_config_packets(struct dsi_hw *hw, const struct dsi_hw_cfg *cfg)
{
	static const uint8_t pixel_dt[] = {
		[DPY_DSI_FMT_RGB888] = DPY_DSI_PACKED_PIXEL_STREAM_24,
		[DPY_DSI_FMT_RGB666] = DPY_DSI_PIXEL_STREAM_3BYTE_18,
		[DPY_DSI_FMT_RGB666_PACKED] = DPY_DSI_PACKED_PIXEL_STREAM_18,
		[DPY_DSI_FMT_RGB565] = DPY_DSI_PACKED_PIXEL_STREAM_16,
	};
	const struct dpy_display_mode *m = &cfg->mode;
	uint32_t bpp = dsi_bits_per_pixel(cfg->format);
	uint32_t hspw = m->hsync_end - m->hsync_start;
	uint32_t hbp = m->htotal - m->hsync_start;	/* includes sync */
	uint32_t vspw = m->vsync_end - m->vsync_start;
	uint32_t vbp = m->vtotal - m->vsync_start;
	uint32_t hsa, hbp_b, hact, hfp, hblk, vblk;

	if (cfg->dsi_mode == DPY_DSI_COMMAND) {
		dsi_write(hw, DSI_PIXEL_CTL0,
			  DPY_FIELD_PREP(DSI_PIXEL_FORMAT, cfg->format));
		dsi_write(hw, DSI_PIXEL_PH,
			  dsi_header(DPY_DSI_DCS_LONG_WRITE, cfg->channel,
				     (uint16_t)(1 + m->hdisplay * bpp / 8)));
		dsi_write(hw, DSI_PIXEL_PD,
			  DPY_FIELD_PREP(DSI_PIXEL_PD_TRAN0,
					 DPY_DCS_WRITE_MEMORY_START) |
			  DPY_FIELD_PREP(DSI_PIXEL_PD_TRANN,
					 DPY_DCS_WRITE_MEMORY_CONTINUE));
		dsi_write(hw, DSI_PIXEL_PF0, 0xffff);
		/* CRC seeds of the command byte for first/next lines */
		dsi_write(hw, DSI_PIXEL_PF1, 0xe4e9 | (0xf468U << 16));
		return;
	}

	dsi_write(hw, DSI_PIXEL_CTL0, DSI_PIXEL_PD_PLUG_DIS |
		  DPY_FIELD_PREP(DSI_PIXEL_FORMAT, 8 + cfg->format));
	dsi_write(hw, DSI_PIXEL_PH, dsi_header(pixel_dt[cfg->format], 0,
					       (uint16_t)(m->hdisplay * bpp / 8)));
	dsi_write(hw, DSI_PIXEL_PF0, 0xffff);
	dsi_write(hw, DSI_PIXEL_PF1, 0xffffffff);

	hact = m->hdisplay * bpp / 8;
	if (cfg->dsi_mode == DPY_DSI_VIDEO_BURST) {
		hsa = 0;
		hbp_b = 0;
		hfp = 0;
		hblk = hact;
		vblk = 0;
		dsi_update(hw, DSI_BASIC_CTL,
			   DSI_BASIC_HSA_HSE_DIS | DSI_BASIC_HBP_DIS,
			   DSI_BASIC_HSA_HSE_DIS | DSI_BASIC_HBP_DIS);
	} else {
		/* blanking payload sizes minus the packet overheads */
		hsa = hspw * bpp / 8 - (4 + 4 + 2);
		hbp_b = (hbp - hspw) * bpp / 8 - 10;
		hblk = (m->htotal - hspw) * bpp / 8 - (4 + 4 + 2);
		hfp = (m->htotal - hbp - m->hdisplay) * bpp / 8 - 6 - 6;
		if (cfg->lanes == 4) {
			uint32_t t = (m->htotal * bpp / 8) * m->vtotal -
				     (4 + hblk + 2);

			vblk = cfg->lanes - t % cfg->lanes;
		} else {
			vblk = 0;
		}
	}

	dsi_write(hw, DSI_SYNC_HSS, dsi_header(DPY_DSI_H_SYNC_START, 0, 0));
	dsi_write(hw, DSI_SYNC_HSE, dsi_header(DPY_DSI_H_SYNC_END, 0, 0));
	dsi_write(hw, DSI_SYNC_VSS, dsi_header(DPY_DSI_V_SYNC_START, 0, 0));
	dsi_write(hw, DSI_SYNC_VSE, dsi_header(DPY_DSI_V_SYNC_END, 0, 0));

	dsi_write(hw, DSI_BASIC_SIZE0, DPY_FIELD_PREP(DSI_SIZE0_VSA, vspw) |
		  DPY_FIELD_PREP(DSI_SIZE0_VBP, vbp - vspw));
	dsi_write(hw, DSI_BASIC_SIZE1, DPY_FIELD_PREP(DSI_SIZE1_VACT, m->vdisplay) |
		  DPY_FIELD_PREP(DSI_SIZE1_VT, m->vtotal));

	dsi_write_blank(hw, DSI_BLK_HSA0, DSI_BLK_HSA1, 0, hsa);
	dsi_write_blank(hw, DSI_BLK_HBP0, DSI_BLK_HBP1, 0, hbp_b);
	dsi_write_blank(hw, DSI_BLK_HFP0, DSI_BLK_HFP1, 0, hfp);
	dsi_write_blank(hw, DSI_BLK_HBLK0, DSI_BLK_HBLK1, 0, hblk);
	dsi_write_blank(hw, DSI_BLK_VBLK0, DSI_BLK_VBLK1, 0, vblk);
}

void dsi_hw_config(struct dsi_hw *hw, const struct dsi_hw_cfg *cfg)
{
	dsi_config_basic(hw, cfg);
	dsi_config_instructions(hw, cfg);
	dsi_config_packets(hw, cfg);
	dsi_write(hw, DSI_DEBUG_DATA, 0xff);
	dsi_write(hw, DSI_CTL, DSI_CTL_EN);
}

void dsi_hw_disable(struct dsi_hw *hw)
{
	dsi_write(hw, DSI_GINT0, 0);
	dsi_write(hw, DSI_CTL, 0);
}

void dsi_hw_run(struct dsi_hw *hw, enum dsi_seq seq)
{
	uint32_t jump;

	switch (seq) {
	case DSI_SEQ_HS_CLOCK:
		jump = DSI_SLOT(DSI_INST_LP11, DSI_INST_HSC) |
		       DSI_SLOT(DSI_INST_HSC, DSI_INST_END);
		break;
	case DSI_SEQ_HS_VIDEO:
		jump = DSI_SLOT(DSI_INST_LP11, DSI_INST_HSC) |
		       DSI_SLOT(DSI_INST_HSC, DSI_INST_NOP) |
		       DSI_SLOT(DSI_INST_NOP, DSI_INST_HSD) |
		       DSI_SLOT(DSI_INST_HSD, DSI_INST_DLY) |
		       DSI_SLOT(DSI_INST_DLY, DSI_INST_NOP) |
		       DSI_SLOT(DSI_INST_HSCEXIT, DSI_INST_END);
		break;
	case DSI_SEQ_HS_DATA:
		jump = DSI_SLOT(DSI_INST_LP11, DSI_INST_NOP) |
		       DSI_SLOT(DSI_INST_NOP, DSI_INST_HSD) |
		       DSI_SLOT(DSI_INST_HSD, DSI_INST_DLY) |
		       DSI_SLOT(DSI_INST_DLY, DSI_INST_NOP) |
		       DSI_SLOT(DSI_INST_HSCEXIT, DSI_INST_END);
		break;
	case DSI_SEQ_LP_TX:
		jump = DSI_SLOT(DSI_INST_LP11, DSI_INST_LPDT) |
		       DSI_SLOT(DSI_INST_LPDT, DSI_INST_END);
		break;
	case DSI_SEQ_LP_RX:
		jump = DSI_SLOT(DSI_INST_LP11, DSI_INST_LPDT) |
		       DSI_SLOT(DSI_INST_LPDT, DSI_INST_DLY) |
		       DSI_SLOT(DSI_INST_DLY, DSI_INST_TBA) |
		       DSI_SLOT(DSI_INST_TBA, DSI_INST_END);
		break;
	default:
		jump = DSI_SLOT(DSI_INST_LP11, DSI_INST_END);
		break;
	}
	dsi_write(hw, DSI_INST_JUMP_SEL, jump);
	/* a rising edge of INST_ST starts the engine */
	dsi_update(hw, DSI_BASIC_CTL0, DSI_CTL0_INST_ST, 0);
	dsi_update(hw, DSI_BASIC_CTL0, DSI_CTL0_INST_ST, DSI_CTL0_INST_ST);

	if (seq == DSI_SEQ_HS_CLOCK) {
		/* keep the clock lane in HS unless pixel data is plugged */
		bool plug_dis = dsi_read(hw, DSI_PIXEL_CTL0) &
				DSI_PIXEL_PD_PLUG_DIS;

		dsi_update(hw, DSI_INST_FUNC(DSI_INST_LP11), DSI_INST_LANE_CEN,
			   plug_dis ? 0 : DSI_INST_LANE_CEN);
	}
}

int dsi_hw_wait_idle(struct dsi_hw *hw, uint32_t timeout_us)
{
	while (dsi_read(hw, DSI_BASIC_CTL0) & DSI_CTL0_INST_ST) {
		if (!timeout_us--)
			return -ETIMEDOUT;
		dpy_os_udelay(1);
	}
	return 0;
}

void dsi_hw_video_hold(struct dsi_hw *hw, bool hold)
{
	dsi_update(hw, DSI_INST_JUMP_CFG(0), DSI_JUMP_CFG_EN,
		   hold ? DSI_JUMP_CFG_EN : 0);
	if (!hold)
		dsi_hw_run(hw, DSI_SEQ_HS_VIDEO);
}

static bool dsi_is_read(uint8_t type)
{
	return type == DPY_DSI_DCS_READ || type == DPY_DSI_GENERIC_READ_0 ||
	       type == DPY_DSI_GENERIC_READ_1 || type == DPY_DSI_GENERIC_READ_2;
}

static bool dsi_is_long(uint8_t type)
{
	return type == DPY_DSI_DCS_LONG_WRITE ||
	       type == DPY_DSI_GENERIC_LONG_WRITE;
}

/* copy @len bytes to the TX FIFO words starting at byte offset @off */
static void dsi_fill_tx(struct dsi_hw *hw, const uint8_t *bytes, size_t len)
{
	uint32_t word = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		word |= (uint32_t)bytes[i] << (8 * (i & 3));
		if ((i & 3) == 3 || i == len - 1) {
			dsi_write(hw, DSI_CMD_TX(i / 4), word);
			word = 0;
		}
	}
}

static int dsi_read_response(struct dsi_hw *hw, const struct dpy_dsi_msg *msg)
{
	uint32_t ctl = dsi_read(hw, DSI_CMD_CTL);
	uint8_t rx[32];
	uint32_t i, n;
	uint8_t type;

	if (!(ctl & DSI_CMD_RX_FLAG))
		return -EIO;
	if (ctl & DSI_CMD_RX_OVERFLOW)
		return -EOVERFLOW;

	for (i = 0; i < 8; i++) {
		uint32_t w = dsi_read(hw, DSI_CMD_RX(i));

		rx[i * 4 + 0] = (uint8_t)w;
		rx[i * 4 + 1] = (uint8_t)(w >> 8);
		rx[i * 4 + 2] = (uint8_t)(w >> 16);
		rx[i * 4 + 3] = (uint8_t)(w >> 24);
	}

	type = rx[0] & 0x3f;
	switch (type) {
	case DPY_DSI_RX_ACK_ERR_REPORT:
		return -EIO;
	case DPY_DSI_RX_DCS_SHORT_1:
	case DPY_DSI_RX_GENERIC_SHORT_1:
		n = 1;
		break;
	case DPY_DSI_RX_DCS_SHORT_2:
	case DPY_DSI_RX_GENERIC_SHORT_2:
		n = 2;
		break;
	case DPY_DSI_RX_DCS_LONG:
	case DPY_DSI_RX_GENERIC_LONG:
		/* rx size counts header (4) + payload + crc (2) - 1 */
		n = DPY_FIELD_GET(DSI_CMD_RX_SIZE, ctl) + 1;
		n = n > 6 ? n - 6 : 0;
		n = DPY_MIN(n, (uint32_t)sizeof(rx) - 4);
		if (msg->rx_buf)
			memcpy(msg->rx_buf, &rx[4], DPY_MIN(n, msg->rx_len));
		return (int)n;
	default:
		return -EIO;
	}
	if (msg->rx_buf)
		memcpy(msg->rx_buf, &rx[1], DPY_MIN(n, msg->rx_len));
	return (int)n;
}

int dsi_hw_transfer(struct dsi_hw *hw, const struct dpy_dsi_msg *msg)
{
	const uint8_t *tx = msg->tx_buf;
	uint8_t pkt[DSI_CMD_TX_BYTES];
	uint32_t header;
	size_t len;
	uint16_t crc;
	int ret;

	if (dsi_hw_wait_idle(hw, 5000))
		dsi_update(hw, DSI_BASIC_CTL0, DSI_CTL0_INST_ST, 0);

	if (dsi_is_long(msg->type)) {
		/* the TX size field is 8 bits wide: 256 bytes per packet */
		if (msg->tx_len + 6 > DPY_MIN(sizeof(pkt), 256U))
			return -EINVAL;
		header = dsi_header(msg->type, msg->channel,
				    (uint16_t)msg->tx_len);
		memcpy(&pkt[4], tx, msg->tx_len);
		crc = dpy_dsi_crc(tx, msg->tx_len);
		pkt[4 + msg->tx_len] = (uint8_t)crc;
		pkt[5 + msg->tx_len] = (uint8_t)(crc >> 8);
		len = 4 + msg->tx_len + 2;
	} else {
		uint16_t data = 0;

		if (msg->tx_len > 0)
			data = tx[0];
		if (msg->tx_len > 1)
			data |= (uint16_t)tx[1] << 8;
		header = dsi_header(msg->type, msg->channel, data);
		len = 4;
	}
	pkt[0] = (uint8_t)header;
	pkt[1] = (uint8_t)(header >> 8);
	pkt[2] = (uint8_t)(header >> 16);
	pkt[3] = (uint8_t)(header >> 24);

	dsi_fill_tx(hw, pkt, len);
	dsi_update(hw, DSI_CMD_CTL, DSI_CMD_TX_SIZE,
		   DPY_FIELD_PREP(DSI_CMD_TX_SIZE, len - 1));

	if (!dsi_is_read(msg->type)) {
		dsi_hw_run(hw, DSI_SEQ_LP_TX);
		ret = dsi_hw_wait_idle(hw, 5000);
		return ret ? ret : 0;
	}

	dsi_hw_run(hw, DSI_SEQ_LP_RX);
	ret = dsi_hw_wait_idle(hw, 5000);
	if (ret) {
		/* the peripheral did not answer: recover the link */
		dsi_update(hw, DSI_BASIC_CTL0, DSI_CTL0_INST_ST, 0);
		return ret;
	}
	return dsi_read_response(hw, msg);
}

void dsi_hw_irq_enable(struct dsi_hw *hw, uint32_t irq, bool on)
{
	uint32_t val = dsi_read(hw, DSI_GINT0) & DPY_GENMASK(15, 0);

	val = on ? val | DSI_GINT0_EN(irq) : val & ~DSI_GINT0_EN(irq);
	/* do not acknowledge anything while changing the enables */
	dsi_write(hw, DSI_GINT0, val);
}

uint32_t dsi_hw_irq_ack(struct dsi_hw *hw)
{
	uint32_t val = dsi_read(hw, DSI_GINT0);
	uint32_t pending = (val >> 16) & val & DPY_GENMASK(15, 0);

	/* flags are write-one-to-clear */
	if (pending)
		dsi_write(hw, DSI_GINT0, (val & DPY_GENMASK(15, 0)) |
			  (pending << 16));
	return pending;
}

uint32_t dsi_hw_get_line(struct dsi_hw *hw)
{
	uint32_t line = DPY_FIELD_GET(DSI_DEBUG_CUR_LINE,
				      dsi_read(hw, DSI_DEBUG_VIDEO0));
	uint32_t size0 = dsi_read(hw, DSI_BASIC_SIZE0);
	uint32_t size1 = dsi_read(hw, DSI_BASIC_SIZE1);
	uint32_t vt = DPY_FIELD_GET(DSI_SIZE1_VT, size1);
	uint32_t vact = DPY_FIELD_GET(DSI_SIZE1_VACT, size1);
	uint32_t vsa = DPY_FIELD_GET(DSI_SIZE0_VSA, size0);
	uint32_t vbp = DPY_FIELD_GET(DSI_SIZE0_VBP, size0);
	uint32_t vfp = vt - vsa - vbp - vact;

	/* the host counts from the vsync start, the TCON from the front porch */
	line += vfp;
	if (line >= vt)
		line -= vt;
	return line;
}

void dsi_hw_dump(struct dsi_hw *hw, void (*print)(const char *fmt, ...))
{
	static const uint16_t regs[] = {
		DSI_CTL, DSI_GINT0, DSI_BASIC_CTL, DSI_BASIC_CTL0,
		DSI_BASIC_CTL1, DSI_BASIC_SIZE0, DSI_BASIC_SIZE1,
		DSI_INST_JUMP_SEL, DSI_INST_JUMP_CFG(0), DSI_TCON_DRQ,
		DSI_PIXEL_CTL0, DSI_PIXEL_PH, DSI_CMD_CTL, DSI_DEBUG_VIDEO0,
		DSI_DEBUG_INST,
	};
	unsigned int i;

	for (i = 0; i < DPY_ARRAY_SIZE(regs); i++)
		print("  +%03x: %08x\n", regs[i], dsi_read(hw, regs[i]));
}
