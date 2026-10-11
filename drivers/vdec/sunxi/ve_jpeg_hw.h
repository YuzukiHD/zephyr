/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Baseline JPEG decoding on the video engine. The engine parses the entropy
 * coded data itself; software reads the marker segments (ve_jpeg_parse), the
 * tables are loaded into the engine (ve_jpeg_hw_setup) and a run decodes one
 * picture into memory (ve_jpeg_hw_run).
 */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_JPEG_HW_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_JPEG_HW_H_

#include <stddef.h>
#include <stdint.h>

/* Chroma sampling of a frame, the values are the ones the engine takes */
enum ve_jpeg_format {
	VE_JPEG_420 = 0,
	VE_JPEG_411 = 1,
	VE_JPEG_422 = 2,
	VE_JPEG_444 = 3,
	VE_JPEG_422T = 4,
	VE_JPEG_400 = 5,
	VE_JPEG_ERR = 6,
};

/* A Huffman table in the layout the engine wants */
struct ve_jpeg_huff {
	uint16_t start_code[16];	/* first code of each length, 0xffff when unused */
	uint8_t offset[16];		/* symbols of shorter codes */
	uint8_t symbol[256];
};

struct ve_jpeg_comp {
	int id;
	int h, v;			/* sampling factors */
	int tq;				/* quantisation table */
	int td, ta;			/* DC / AC Huffman table */
};

/* Everything a decode needs from the marker segments of one picture */
struct ve_jpeg_frame {
	int width;
	int height;
	int ncomp;
	struct ve_jpeg_comp comp[3];
	int scan_order[3];		/* component ids in scan order */
	int max_h, max_v;
	int restart_interval;
	enum ve_jpeg_format format;
	uint16_t qtab[4][64];
	struct ve_jpeg_huff dc[2];
	struct ve_jpeg_huff ac[2];
	int has_dht;
	size_t data_off;		/* first byte of the entropy coded data */
	size_t data_size;		/* up to the end marker */
};

/* Sampling format from the components, VE_JPEG_ERR if the engine cannot do it */
enum ve_jpeg_format ve_jpeg_format_of(const struct ve_jpeg_comp *c, int ncomp);

/* Read the segments of one picture; 0, -EINVAL (damaged) or -ENOTSUP (not baseline) */
int ve_jpeg_parse(const uint8_t *buf, size_t len, struct ve_jpeg_frame *f);

/* Words of the engine's table memory (1024 half words) for a frame */
#define VE_JPEG_HUFF_WORDS 512
void ve_jpeg_huff_image(const struct ve_jpeg_frame *f, uint32_t out[VE_JPEG_HUFF_WORDS]);
/* The image used when a stream has no DHT segment (standard tables) */
const uint32_t *ve_jpeg_default_huff_image(void);

/* ---- hardware ---- */

/* Where the engine writes and where the compressed data is */
struct ve_jpeg_job {
	const struct ve_jpeg_frame *frame;
	int direct;			/* decode straight into the picture (the only mode used) */
	uint32_t ref_y, ref_c;		/* physical addresses of the picture planes */
	/* stream buffer holding the data */
	uint32_t vbv_phys;		/* physical address of the buffer start */
	uint32_t vbv_size;
	uint32_t data_offset;		/* of the entropy coded data in the buffer */
	uint32_t data_size;
};

/* Decode one picture; blocks until the engine is done. 0 or a negative errno. */
int ve_jpeg_hw_decode(const struct ve_jpeg_job *job);

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_JPEG_HW_H_ */
