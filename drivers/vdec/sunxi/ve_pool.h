/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Picture pool: a fixed set of pictures shared between the decoder (which
 * writes and references them) and the consumer (which reads them for display).
 * A picture is always in one of these places:
 *
 *   free        nobody holds it
 *   decoder     taken with ve_pool_acquire(), held until submitted or discarded
 *   ready       decoded and waiting for the consumer
 *   consumer    taken with ve_pool_take(), held until ve_pool_give_back()
 *
 * A reference picture can be held by the decoder and wait in the ready queue or
 * be with the consumer at the same time; it goes back to the free list when
 * the last holder lets go.
 *
 * The engine writes tiled pictures (32x32 macroblocks). A tiled picture that
 * becomes ready is converted by the engine into a second, linear NV12
 * picture of the pool and that one goes to the consumer, so decoding a tiled
 * picture needs two free pictures.
 */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_POOL_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_POOL_H_

#include <stddef.h>
#include <stdint.h>

/* Layout of the memory of a picture */
enum ve_pixel {
	VE_PIX_TILED_420,	/* what the engine writes: luma and chroma planes in 32x32 macroblocks */
	VE_PIX_NV12,		/* luma plane, then an interleaved Cb/Cr plane at half resolution */
	VE_PIX_RGBA,		/* 32 bit words, the bytes in memory are A, B, G, R */
};

struct ve_picture {
	int id;
	enum ve_pixel format;
	/* size of the buffer in pixels (aligned) and bytes from one row to the next */
	int width;
	int height;
	int stride;
	/* the visible part: edges counted from the top left corner, right and bottom exclusive */
	int left;
	int top;
	int right;
	int bottom;
	int64_t pts;
	/* the second plane is NULL for RGBA */
	uint8_t *plane[2];
	size_t size;
};

struct ve_pool;

struct ve_pool_config {
	int frames;		/* pictures to allocate */
	int width;		/* of the visible part */
	int height;
	/* VE_PIX_TILED_420 (then converted for the consumer) or VE_PIX_RGBA (no conversion) */
	enum ve_pixel format;
	int align;		/* alignment of the width and height of a linear picture, 0 for 16 */
};

struct ve_pool *ve_pool_create(const struct ve_pool_config *config);
void ve_pool_destroy(struct ve_pool *pool);

/* Decoder side: a free picture, NULL when there is none */
struct ve_picture *ve_pool_acquire(struct ve_pool *pool);
/* The decoder is done with the picture; it is shown, and freed when nobody references it */
void ve_pool_submit(struct ve_pool *pool, struct ve_picture *pic);
/* The decoder is done with the picture and it is not to be shown */
void ve_pool_discard(struct ve_pool *pool, struct ve_picture *pic);
/* The picture is finished but the decoder keeps it as a reference: show it as well */
void ve_pool_share(struct ve_pool *pool, struct ve_picture *pic);

/*
 * The engine writes the linear (NV12) copy of a tiled picture itself: @p out, taken with
 * ve_pool_acquire(), is the copy of @p pic. When @p pic becomes visible, @p out is queued for the
 * consumer instead of a converted picture being made; when @p pic is discarded unseen, @p out is
 * freed with it.
 */
int ve_pool_attach_linear(struct ve_pool *pool, struct ve_picture *pic, struct ve_picture *out);

/* Consumer side */
struct ve_picture *ve_pool_take(struct ve_pool *pool);
int ve_pool_give_back(struct ve_pool *pool, struct ve_picture *pic);

/* Drop the pictures that wait for the consumer */
void ve_pool_flush(struct ve_pool *pool);

int ve_pool_total(const struct ve_pool *pool);
int ve_pool_free(const struct ve_pool *pool);
int ve_pool_ready(const struct ve_pool *pool);

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_POOL_H_ */
