/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/arch/cpu.h>
#include <stdint.h>
#include <string.h>



#include "ve_hw.h"
#include "ve_jpeg_hw.h"

#ifndef BIT
#define BIT(n) (1U << (n))
#endif


/*
 * Decoder block, offsets from VE_HW_JPEG_REGS. The JPEG decoder uses the block of the MPEG family;
 * the names in the comments are those of the register guide, where the same registers sit 0x100
 * higher: https://linux-sunxi.org/VE_Register_guide
 */
#define R_CTRL		0x14	/* MACC_MPEG_CTRL: mode, interrupt enables */
#define R_START		0x18	/* MACC_MPEG_TRIG, byte 0: start command */
#define R_FORMAT	0x1b	/* MACC_MPEG_TRIG, byte 3: (chroma sampling << 3) | 3 */
#define R_STATUS	0x1c	/* MACC_MPEG_STATUS: write 1 to clear */
#define R_STREAM_ADDR	0x28	/* MACC_MPEG_VLD_ADDR: bits 31:28 of the buffer address in 3:0, 27:4 in place, 0x7 flags */
#define R_STREAM_OFF	0x2c	/* MACC_MPEG_VLD_OFFSET: start of the data within the buffer, in bits */
#define R_STREAM_LEN	0x30	/* MACC_MPEG_VLD_LEN: size of the data, in bits */
#define R_STREAM_END	0x34	/* MACC_MPEG_VBV_END: last byte of the buffer */
#define R_QTAB		0x80	/* MACC_MPEG_IQ_MIN_INPUT, the quantisation table port: (index << 8) | value */
#define R_QTAB_FIRST	BIT(14)	/* set on the first word of the luma table */
#define R_MCU_SIZE	0xb8	/* MACC_MPEG_JPEG_SIZE: ((mcu rows - 1) << 16) | (mcu columns - 1) */
#define R_RESTART	0xc0	/* MACC_MPEG_JPEG_RES_INT: restart interval */
#define R_RECON_Y	0xcc	/* MACC_MPEG_ROT_LUMA in the guide: the buffer the picture is written to, luma */
#define R_RECON_C	0xd0	/* MACC_MPEG_ROT_CHROMA: the same, chroma */
#define R_SCALE		0xd4	/* MACC_MPEG_ROTSCALE_CTRL: ((vertical << 2) | horizontal) << 8 */
#define R_HUFF_ADDR	0xe0	/* MACC_MPEG_SRAM_RW_OFFSET: address in the table memory */
#define R_HUFF_DATA	0xe4	/* MACC_MPEG_SRAM_RW_DATA: data port of the table memory (Huffman tables) */

#define START_DECODE	14
#define CTRL_IRQ_EN	0x7c
#define CTRL_MODE_BIG	0x40180	/* 4:2:2 and the engine versions that need it */
#define CTRL_MODE	0x180

/* top block, output side */
#define T_REF_C_SIZE	0xc4
#define T_REF_Y_STRIDE	0xc8	/* half word */
#define T_REF_C_STRIDE	0xca	/* half word */
#define T_DISP_Y_STRIDE	0xcc	/* half word */
#define T_DISP_C_STRIDE	0xce	/* half word */
#define T_OUT_CTRL	0xe8	/* bits 27:0 chroma buffer size, 31:28 format flags */
#define T_OUT_FORMAT	0xec	/* bits 6:4 and 2:0: display pixel format */

static void load_qtab(volatile uint8_t *r, const uint16_t *q0, const uint16_t *q1)
{
	for (int i = 0; i < 64; i++) {
		sys_write32(((uint32_t)i << 8) | q0[i] | R_QTAB_FIRST, (mem_addr_t)(r) + (R_QTAB));
	}
	for (int i = 0; i < 64; i++) {
		sys_write32(((uint32_t)i << 8) | q1[i], (mem_addr_t)(r) + (R_QTAB));
	}
}

static void load_huff(volatile uint8_t *r, const struct ve_jpeg_frame *f)
{
	uint32_t image[VE_JPEG_HUFF_WORDS];
	const uint32_t *src = image;

	if (f->has_dht) {
		ve_jpeg_huff_image(f, image);
	} else {
		src = ve_jpeg_default_huff_image();
	}
	sys_write32(0, (mem_addr_t)(r) + (R_HUFF_ADDR));
	for (int i = 0; i < VE_JPEG_HUFF_WORDS; i++) {
		sys_write32(src[i], (mem_addr_t)(r) + (R_HUFF_DATA));
	}
}

/* Quantisation table used by the n-th component of the scan */
static const uint16_t *qtab_of(const struct ve_jpeg_frame *f, int n)
{
	int id = f->scan_order[n < f->ncomp ? n : 0];

	for (int i = 0; i < f->ncomp; i++) {
		if (f->comp[i].id == id) {
			return f->qtab[f->comp[i].tq];
		}
	}

	return f->qtab[0];
}

static uint32_t stream_addr_word(uint32_t phys)
{
	return (phys >> 28) | (phys & 0x0ffffff0U) | 0x70000000U;
}

static uint32_t ctrl_mode(const struct ve_jpeg_job *job)
{
	if (job->direct) {
		return 0;
	}

	uint64_t version = ve_hw_version();
	uint32_t ver_lo = (uint32_t)version;
	uint32_t ver_hi = (uint32_t)(version >> 32);

	/* the old engine revisions have the plain mode only; 4:2:2 needs the wide one */
	if (ver_hi == 0 && ver_lo <= 0x1638) {
		return CTRL_MODE;
	}

	return job->frame->format == VE_JPEG_422 ? CTRL_MODE_BIG : CTRL_MODE;
}

int ve_jpeg_hw_decode(const struct ve_jpeg_job *job)
{
	const struct ve_jpeg_frame *f = job->frame;
	volatile uint8_t *r = ve_hw_regs(VE_HW_JPEG_REGS);
	volatile uint8_t *t = ve_hw_regs(VE_HW_TOP);
	int mcu_w, mcu_h;

	if (r == NULL || t == NULL || f->format == VE_JPEG_ERR) {
		return -EINVAL;
	}

	/* tables and geometry */
	sys_write32(ctrl_mode(job), (mem_addr_t)(r) + (R_CTRL));
	sys_write32((uint32_t)f->restart_interval, (mem_addr_t)(r) + (R_RESTART));
	sys_write8((uint8_t)(((unsigned int)f->format << 3) | 3), (mem_addr_t)(r) + R_FORMAT);
	load_qtab(r, qtab_of(f, 0), qtab_of(f, 1));
	sys_write32(job->ref_y, (mem_addr_t)(r) + (R_RECON_Y));
	sys_write32(job->ref_c, (mem_addr_t)(r) + (R_RECON_C));
	mcu_w = (f->width + f->max_h * 8 - 1) / (f->max_h * 8);
	mcu_h = (f->height + f->max_v * 8 - 1) / (f->max_v * 8);
	sys_write32(((uint32_t)(mcu_h - 1) << 16) | (uint32_t)((mcu_w - 1) & 0xffff), (mem_addr_t)(r) + (R_MCU_SIZE));
	sys_write32(0, (mem_addr_t)(r) + (R_SCALE));
	load_huff(r, f);
	sys_write32(job->vbv_phys + job->vbv_size - 1, (mem_addr_t)(r) + (R_STREAM_END));

	/* start and wait */
	sys_write32(sys_read32((mem_addr_t)(r) + (R_CTRL)) | CTRL_IRQ_EN, (mem_addr_t)(r) + (R_CTRL));
	sys_write32(job->data_offset << 3, (mem_addr_t)(r) + (R_STREAM_OFF));
	sys_write32(job->data_size << 3, (mem_addr_t)(r) + (R_STREAM_LEN));
	sys_write32(stream_addr_word(job->vbv_phys), (mem_addr_t)(r) + (R_STREAM_ADDR));
	sys_write8(START_DECODE, (mem_addr_t)(r) + R_START);
	ve_hw_wait();
	sys_write32(sys_read32((mem_addr_t)(r) + (R_STATUS)) | 0x0f, (mem_addr_t)(r) + (R_STATUS));

	return 0;
}
