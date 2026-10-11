/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The video engine of the chip: power, clock, reset, interrupt, the lock that
 * serialises the decoders and the tiled to linear picture conversion. There is
 * one engine; every session shares it.
 */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_HW_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_HW_H_

#include <stdint.h>

/* Decoder blocks of the engine (PNG is decoded by the CPU) */
enum ve_hw_decoder {
	VE_HW_NONE,
	VE_HW_JPEG,
	VE_HW_H264,
};

/*
 * Register window of the engine, as offsets from its base. A common block at the start is shared
 * by all decoders; every decoder has a block of its own that the decoder number in the top mode
 * register switches in, so only one decoder is usable at a time and the lock of this file
 * protects the choice.
 *
 *   0x000  common block: decoder select, reset, busy status, placement of the line buffers,
 *          output strides and format, version
 *   0x100  the block of the MPEG family, which the baseline JPEG decoder uses: control 0x114,
 *          trigger 0x118, status 0x11c, picture size in MCUs 0x1b8, restart interval 0x1c0,
 *          port to the table memory (quantisation, Huffman) 0x1e0/0x1e4
 *   0x200  the H.264 block: control 0x220, trigger 0x224, status 0x228, bit stream window
 *          0x230..0x23c, reconstruction and reference picture addresses from 0x2ac
 *   0xa00  picture conversion block (tiled to linear), see ve_hw_convert()
 *
 * Earlier generations of the engine have blocks for more codecs behind these (0x300 and up) and
 * count the memory traffic of the engine in the common block (0x20 reads, 0x24 writes, in 64 bit
 * words, bit 31 enables); this driver uses neither.
 *
 * The register guide of the linux-sunxi community describes the layout of these earlier
 * generations: https://linux-sunxi.org/VE_Register_guide. It matches this chip in structure
 * (base, common block, one block per decoder) but not in every encoding: here the decoder numbers
 * have bit 3 inverted, and 0xe8 and 0xec also return the version of the engine.
 */
#define VE_HW_TOP	0x000
#define VE_HW_JPEG_REGS	0x100
#define VE_HW_H264_REGS	0x200

/* Powers and resets the engine for the first user, counts the others */
int ve_hw_get(void);
void ve_hw_put(void);

/* Serialise the use of the registers; the lock may be taken again by its owner */
void ve_hw_lock(void);
void ve_hw_unlock(void);

/* Route the registers to a decoder block (the lock is taken inside) */
void ve_hw_select(enum ve_hw_decoder decoder);
/* Decoder blocks off */
void ve_hw_idle(void);
/* Reset the decoder blocks and select @p decoder; @p width sets the memory mode */
void ve_hw_reset(enum ve_hw_decoder decoder, int width);

/* Sleep until the engine interrupts, 0 or -ETIMEDOUT */
int ve_hw_wait(void);

int ve_hw_set_clock(unsigned int mhz);

/* Revision words the chip reports: high word in bits 63..32 */
uint64_t ve_hw_version(void);

/* First byte of a register block */
volatile uint8_t *ve_hw_regs(uint32_t offset);

/*
 * Convert a picture the engine left in its tiled layout to linear NV12. The
 * luma plane is written at @p out_y and the chroma plane follows it.
 */
int ve_hw_convert(uint32_t in_y, uint32_t in_c, uint32_t out_y, int width, int height);

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_HW_H_ */
