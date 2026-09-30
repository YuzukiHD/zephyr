/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * TCON LCD and TCON top (display interface top) register map, sunxi
 * "tcon lcd" generation found on sun252iw2.
 *
 * Fields marked (?) are unverified.
 */
#ifndef __TCON_REGS_H__
#define __TCON_REGS_H__

#include <dpy/dpy_types.h>

/* ------------------------------------------------------------------ */
/* TCON LCD                                                            */
/* ------------------------------------------------------------------ */
#define TCON_GCTL			0x000
#define   TCON_GCTL_EN			DPY_BIT(31)
#define   TCON_GCTL_GAMMA_EN		DPY_BIT(30)
#define   TCON_GCTL_PAD_SEL		DPY_BIT(1)
#define   TCON_GCTL_IO_MAP_SEL		DPY_BIT(0)

#define TCON_GINT0			0x004
#define   TCON_GINT0_EN(irq)		DPY_BIT(16 + (irq))
#define   TCON_GINT0_FLAG(irq)		DPY_BIT(irq)
#define   TCON_GINT0_FLAGS		DPY_GENMASK(15, 0)
/* interrupt numbers of GINT0 */
#define   TCON_IRQ_VBLK			15
#define   TCON_IRQ_LINE			13
#define   TCON_IRQ_TRIF			11	/* cpu/dsi trigger finished */
#define   TCON_IRQ_CNTR			10	/* cpu/dsi trigger counter */
#define   TCON_IRQ_FSYNC		2

#define TCON_GINT1			0x008
#define   TCON_GINT1_LINE_NUM		DPY_GENMASK(27, 16)

#define TCON0_FRM_CTL			0x010
#define   TCON0_FRM_EN			DPY_BIT(31)
#define   TCON0_FRM_MODE_R		DPY_BIT(6)
#define   TCON0_FRM_MODE_G		DPY_BIT(5)
#define   TCON0_FRM_MODE_B		DPY_BIT(4)
#define TCON0_FRM_SEED(n)		(0x014 + (n) * 4)	/* n = 0..5 */
#define TCON0_FRM_TBL(n)		(0x02c + (n) * 4)	/* n = 0..3 */

#define TCON0_CTL			0x040
#define   TCON0_CTL_EN			DPY_BIT(31)
#define   TCON0_CTL_IF			DPY_GENMASK(25, 24)
#define     TCON0_IF_HV			0
#define     TCON0_IF_CPU		1	/* also used by DSI */
#define   TCON0_CTL_RB_SWAP		DPY_BIT(23)
#define   TCON0_CTL_RGB_SWAP		DPY_GENMASK(18, 16)
#define   TCON0_CTL_START_DELAY		DPY_GENMASK(8, 4)
#define   TCON0_CTL_SRC_SEL		DPY_GENMASK(2, 0)
#define     TCON0_SRC_DE		0
#define     TCON0_SRC_COLORBAR		1
#define     TCON0_SRC_GRAYSCALE		2
#define     TCON0_SRC_BLACK_WHITE	3
#define     TCON0_SRC_BLACK		4
#define     TCON0_SRC_WHITE		5
#define     TCON0_SRC_GRID		7

#define TCON0_DCLK			0x044
#define   TCON0_DCLK_EN			DPY_GENMASK(31, 28)
#define   TCON0_DCLK_DIV		DPY_GENMASK(6, 0)

#define TCON0_BASIC0			0x048	/* active size */
#define   TCON0_BASIC0_X		DPY_GENMASK(27, 16)
#define   TCON0_BASIC0_Y		DPY_GENMASK(11, 0)
#define TCON0_BASIC1			0x04c	/* horizontal */
#define   TCON0_BASIC1_HT		DPY_GENMASK(28, 16)
#define   TCON0_BASIC1_HBP		DPY_GENMASK(11, 0)
#define TCON0_BASIC2			0x050	/* vertical */
#define   TCON0_BASIC2_VT		DPY_GENMASK(28, 16)
#define   TCON0_BASIC2_VBP		DPY_GENMASK(11, 0)
#define TCON0_BASIC3			0x054	/* sync widths */
#define   TCON0_BASIC3_HSPW		DPY_GENMASK(25, 16)
#define   TCON0_BASIC3_VSPW		DPY_GENMASK(9, 0)

#define TCON0_HV_CTL			0x058
#define   TCON0_HV_MODE			DPY_GENMASK(31, 28)
#define   TCON0_HV_SRGB_SEQ		DPY_GENMASK(27, 24)
#define   TCON0_HV_SYUV_SEQ		DPY_GENMASK(23, 22)
#define   TCON0_HV_SYUV_FDLY		DPY_GENMASK(21, 20)
#define   TCON0_HV_CCIR_CSC_DIS		DPY_BIT(19)

#define TCON0_CPU_CTL			0x060
#define   TCON0_CPU_MODE		DPY_GENMASK(31, 28)
#define     TCON0_CPU_MODE_DSI		1
#define   TCON0_CPU_DA			DPY_BIT(26)
#define   TCON0_CPU_CA			DPY_BIT(25)
#define   TCON0_CPU_AUTO		DPY_BIT(17)
#define   TCON0_CPU_FLUSH		DPY_BIT(16)
#define   TCON0_CPU_TRI_FIFO_EN		DPY_BIT(2)
#define   TCON0_CPU_TRI_START		DPY_BIT(1)
#define   TCON0_CPU_TRI_EN		DPY_BIT(0)

#define TCON0_LVDS_CTL			0x084
#define   TCON0_LVDS_EN			DPY_BIT(31)
#define   TCON0_LVDS_LINK		DPY_BIT(30)	/* dual link */
#define   TCON0_LVDS_EVEN_ODD_DIR	DPY_BIT(29)
#define   TCON0_LVDS_DIR		DPY_BIT(28)
#define   TCON0_LVDS_MODE		DPY_BIT(27)	/* 1: JEIDA */
#define   TCON0_LVDS_BITWIDTH		DPY_BIT(26)	/* 1: 18 bit */
#define   TCON0_LVDS_DEBUG_EN		DPY_BIT(25)
#define   TCON0_LVDS_CORRECT_MODE	DPY_BIT(23)
#define   TCON0_LVDS_CLK_SEL		DPY_BIT(20)
#define   TCON0_LVDS_CLK_REVERT		DPY_BIT(4)
#define   TCON0_LVDS_DATA_REVERT	DPY_GENMASK(3, 0)

#define TCON0_IO_POL			0x088
#define   TCON0_IO_OUTPUT_SEL		DPY_BIT(31)
#define   TCON0_IO_DCLK_SEL		DPY_GENMASK(30, 28)
#define   TCON0_IO_DE_INV		DPY_BIT(27)
#define   TCON0_IO_CLK_INV		DPY_BIT(26)
#define   TCON0_IO_HSYNC_POSITIVE	DPY_BIT(25)
#define   TCON0_IO_VSYNC_POSITIVE	DPY_BIT(24)
#define   TCON0_IO_DATA_INV		DPY_GENMASK(23, 0)

#define TCON0_IO_TRI			0x08c
#define   TCON0_IO_RGB_ENDIAN		DPY_BIT(28)
#define   TCON0_IO_TRI_IO(n)		DPY_BIT(24 + (n))
#define   TCON0_IO_TRI_DATA		DPY_GENMASK(23, 0)

#define TCON_IO_ADJ			0x090

#define TCON_ECC_FIFO			0x0f8
#define   TCON_ECC_FIFO_SETTING		DPY_GENMASK(7, 0)

#define TCON_DEBUG			0x0fc
#define   TCON_DEBUG_TCON0_UNDERFLOW	DPY_BIT(31)
#define   TCON_DEBUG_TCON0_LINE		DPY_GENMASK(27, 16)

#define TCON0_CPU_TRI0			0x160
#define   TCON0_TRI0_BLOCK_SPACE	DPY_GENMASK(27, 16)
#define   TCON0_TRI0_BLOCK_SIZE		DPY_GENMASK(11, 0)
#define TCON0_CPU_TRI1			0x164
#define   TCON0_TRI1_BLOCK_NUM		DPY_GENMASK(15, 0)
#define TCON0_CPU_TRI2			0x168
#define   TCON0_TRI2_START_DELAY	DPY_GENMASK(31, 16)
#define   TCON0_TRI2_TRANS_START_MODE	DPY_BIT(15)
#define   TCON0_TRI2_SYNC_MODE		DPY_GENMASK(14, 13)
#define   TCON0_TRI2_TRANS_START_SET	DPY_GENMASK(12, 0)
#define TCON0_CPU_TRI3			0x16c
#define   TCON0_TRI3_INT_MODE		DPY_GENMASK(29, 28)
#define   TCON0_TRI3_COUNTER_N		DPY_GENMASK(23, 8)
#define   TCON0_TRI3_COUNTER_M		DPY_GENMASK(7, 0)
#define TCON0_CPU_TRI4			0x170
#define   TCON0_TRI4_EN			DPY_BIT(28)

#define TCON_CMAP_CTL			0x180
#define   TCON_CMAP_EN			DPY_BIT(31)

#define TCON_SAFE_PERIOD		0x1f0
#define   TCON_SAFE_PERIOD_FIFO_NUM	DPY_GENMASK(28, 16)
#define   TCON_SAFE_PERIOD_MODE		DPY_GENMASK(1, 0)

#define TCON0_LVDS_ANA(n)		(0x220 + (n) * 4)	/* n = 0, 1 */
#define   TCON0_LVDS_ANA_EN_MB		DPY_BIT(31)
#define   TCON0_LVDS_ANA_SRC_SEL	DPY_BIT(30)
#define   TCON0_LVDS_ANA_EN_LVDS	DPY_BIT(29)
#define   TCON0_LVDS_ANA_EN_24M		DPY_BIT(28)
#define   TCON0_LVDS_ANA_EN_DRVC	DPY_BIT(24)
#define   TCON0_LVDS_ANA_EN_DRVD	DPY_GENMASK(23, 20)
#define   TCON0_LVDS_ANA_C		DPY_GENMASK(19, 17)
#define   TCON0_LVDS_ANA_R		DPY_GENMASK(10, 8)

#define TCON_SYNC_CTL			0x230
#define   TCON_SYNC_DSI_NUM		DPY_BIT(8)
#define   TCON_SYNC_MASTER_SLAVE	DPY_BIT(4)
#define   TCON_SYNC_CTRL_MODE		DPY_BIT(0)

#define TCON_FSYNC_GEN_CTRL		0x23c
#define TCON_FSYNC_GEN_DLY		0x240

/* ------------------------------------------------------------------ */
/* TCON top (display interface top)                                    */
/* ------------------------------------------------------------------ */
#define TCON_TOP_TV_SETUP		0x000
#define   TCON_TOP_TV0_OUT		DPY_BIT(8)	/* 0: lcd0 drives pads */
#define   TCON_TOP_TV1_OUT		DPY_BIT(12)
#define TCON_TOP_DSI_SRC		0x004
#define   TCON_TOP_DSI_SRC_SEL(n)	DPY_BIT((n) * 4)
#define TCON_TOP_CLK_SRC		0x00c
#define   TCON_TOP_LCD_CLK_SRC(n)	DPY_BIT(n)	/* 1: from combo phy */
#define   TCON_TOP_PHY_CLK_SRC(n)	DPY_BIT(4 + (n))
#define TCON_TOP_DE_PERH		0x01c
#define   TCON_TOP_DE_PORT_PERH(p)	DPY_GENMASK(1 + (p) * 4, (p) * 4)
#define TCON_TOP_CLK_GATE		0x020
#define   TCON_TOP_DSI_CLK_GATE		DPY_BIT(16)

#endif /* __TCON_REGS_H__ */
