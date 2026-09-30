/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Frame buffer manager.
 *
 * A pool of pictures shared between the decoder (writes and references them)
 * and the consumer (reads them for display). A picture is always in one of
 * these places:
 *
 *   empty queue    free, nobody holds it
 *   decoder        taken with FbmRequestBuffer(), held until FbmReturnBuffer()
 *   valid queue    decoded and waiting for the consumer
 *   consumer       taken with FbmRequestPicture(), held until FbmReturnPicture()
 *
 * A reference picture can be held by the decoder and sit in the valid queue
 * or with the consumer at the same time; the per-frame flags track that and
 * the picture goes back to the empty queue only when the last holder lets go.
 *
 * Only pictures the manager allocates itself are supported (no externally
 * supplied buffers, no 3D pairs, no compressed or 10 bit formats).
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "ve_abi.h"
#include "ve_log.h"

VE_LOG_DECLARE();

#define FBM_MAX_WIDTH		7680
#define FBM_MAX_HEIGHT		4320
#define FBM_METADATA_SIZE	4096

BUILD_ASSERT(sizeof(struct k_mutex) <= sizeof(((struct ve_fbm *)0)->mutex),
	     "the mutex area of struct ve_fbm is too small");

static inline struct k_mutex *fbm_mutex(struct ve_fbm *fbm)
{
	return (struct k_mutex *)fbm->mutex;
}

static void node_enqueue(struct ve_fbm_node **head, struct ve_fbm_node *node)
{
	struct ve_fbm_node **tail = head;

	while (*tail != NULL) {
		tail = &(*tail)->next;
	}
	node->next = NULL;
	*tail = node;
}

static struct ve_fbm_node *node_dequeue(struct ve_fbm_node **head)
{
	struct ve_fbm_node *node = *head;

	if (node != NULL) {
		*head = node->next;
		node->next = NULL;
	}

	return node;
}

static void make_empty(struct ve_fbm *fbm, struct ve_fbm_node *node)
{
	node_enqueue(&fbm->empty_queue, node);
	fbm->empty_buffer_num++;
}

/* ---- picture memory ---- */

static int picture_alloc(struct ve_fbm *fbm, struct ve_picture *pic, int *align, int width,
			 int height)
{
	int fmt = pic->pixel_format;
	int stride, size_y, size_c, total;
	char *mem;

	pic->data0 = pic->data1 = pic->data2 = pic->data3 = NULL;

	switch (fmt) {
	case VE_PIX_YUV_PLANAR_420:
	case VE_PIX_YUV_PLANAR_422:
	case VE_PIX_YUV_PLANAR_444:
	case VE_PIX_YV12:
	case VE_PIX_NV21:
	case VE_PIX_NV12:
		if (*align == 0) {
			*align = 16;
		}
		stride = ROUND_UP(width, *align);
		pic->line_stride = stride;
		pic->width = stride;
		pic->height = ROUND_UP(height, *align);
		size_y = pic->width * pic->height;
		if (fmt == VE_PIX_YUV_PLANAR_422) {
			size_c = size_y >> 1;
		} else if (fmt == VE_PIX_YUV_PLANAR_444) {
			size_c = size_y;
		} else {
			size_c = size_y >> 2;
		}
		total = size_y + 2 * size_c;
		break;

	case VE_PIX_YUV_MB32_420:
	case VE_PIX_YUV_MB32_422:
	case VE_PIX_YUV_MB32_444: {
		int c_width = ROUND_UP(width >> 1, 16);
		int c_height = ROUND_UP(height >> 1, 32);

		*align = 32;
		pic->line_stride = ROUND_UP(width, 32);
		pic->width = pic->line_stride;
		pic->height = ROUND_UP(height, 32);
		size_y = pic->line_stride * ROUND_UP(height, 32);
		if (fmt == VE_PIX_YUV_MB32_420) {
			size_c = c_width * c_height;
		} else if (fmt == VE_PIX_YUV_MB32_422) {
			size_c = size_y / 2;
		} else {
			size_c = size_y;
		}
		total = size_y + 2 * size_c;
		break;
	}

	case VE_PIX_RGBA:
	case VE_PIX_ARGB:
	case VE_PIX_ABGR:
	case VE_PIX_BGRA:
		pic->line_stride = width * 4;
		pic->width = width;
		pic->height = height;
		size_y = size_c = 0;
		total = pic->line_stride * height;
		break;

	default:
		LOG_ERR("unsupported pixel format %d", fmt);
		return -1;
	}

	mem = fbm->memops->palloc(total, fbm->ve_ops, fbm->ve_ops_self);
	if (mem == NULL) {
		LOG_ERR("no memory for a %d byte picture", total);
		return -1;
	}
	memset(mem, fmt >= VE_PIX_RGBA ? 0xff : 0, total);
	fbm->memops->flush_cache(mem, total);

	pic->data0 = mem;
	if (fmt < VE_PIX_RGBA) {
		pic->data1 = mem + size_y;
		/* NV12/NV21 keep U and V interleaved in the second plane */
		pic->data2 = (fmt == VE_PIX_NV12 || fmt == VE_PIX_NV21) ? NULL :
								       mem + size_y + size_c;
	}
	pic->buf_size = total;

	pic->meta_data = calloc(1, FBM_METADATA_SIZE);
	if (pic->meta_data == NULL) {
		fbm->memops->pfree(mem, fbm->ve_ops, fbm->ve_ops_self);
		pic->data0 = pic->data1 = pic->data2 = NULL;
		return -1;
	}

	pic->phy_y_addr = (uintptr_t)fbm->memops->ve_get_phyaddr(pic->data0);
	if (pic->data1 != NULL) {
		pic->phy_c_addr = (uintptr_t)fbm->memops->ve_get_phyaddr(pic->data1);
	}

	return 0;
}

static void picture_free(struct ve_fbm *fbm, struct ve_picture *pic)
{
	if (pic->data0 != NULL) {
		fbm->memops->pfree(pic->data0, fbm->ve_ops, fbm->ve_ops_self);
	}
	free(pic->meta_data);
	pic->data0 = pic->data1 = pic->data2 = pic->data3 = NULL;
	pic->meta_data = NULL;
}

/* ---- creation ---- */

static struct ve_fbm *fbm_create_one(struct ve_fbm_create_info *ci, struct ve_fbm_info *info)
{
	int width = ci->width, height = ci->height;
	int align = ci->align_stride;
	struct ve_fbm_node *node;
	struct ve_fbm *fbm;
	int i;

	if (width >= FBM_MAX_WIDTH || height >= FBM_MAX_HEIGHT || ci->frame_num <= 0) {
		LOG_ERR("cannot create a frame buffer manager for %dx%d, %d frames", width, height,
			ci->frame_num);
		return NULL;
	}
	if (ci->gpu_buf_valid) {
		LOG_ERR("externally supplied picture buffers are not supported");
		return NULL;
	}

	fbm = calloc(1, sizeof(*fbm));
	if (fbm == NULL) {
		return NULL;
	}
	fbm->frames = calloc(ci->frame_num, sizeof(*fbm->frames));
	if (fbm->frames == NULL) {
		free(fbm);
		return NULL;
	}

	fbm->memops = ci->memops;
	fbm->max_frame_num = ci->frame_num;
	fbm->ve_ops = ci->ve_ops;
	fbm->ve_ops_self = ci->ve_ops_self;
	fbm->thumbnail_mode = ci->thumbnail_mode;
	fbm->fbm_info = info;
	k_mutex_init(fbm_mutex(fbm));

	info->is_soft_decoder = ci->is_soft_decoder;
	if (info->buf_info.right_offset == 0 || info->buf_info.bottom_offset == 0) {
		info->buf_info.right_offset = width;
		info->buf_info.bottom_offset = height;
		info->buf_info.top_offset = 0;
		info->buf_info.left_offset = 0;
	}

	for (i = 0, node = fbm->frames; i < fbm->max_frame_num; i++, node++) {
		struct ve_picture *pic = &node->picture;

		pic->id = i;
		pic->buf_id = 0xdd; /* not known to the display yet */
		pic->pixel_format = ci->pixel_format;
		pic->is_progressive = ci->progressive;
		pic->left_offset = pic->top_offset = 0;
		pic->right_offset = info->buf_info.right_offset;
		pic->bottom_offset = info->buf_info.bottom_offset;
		pic->is_10bit = ci->is_10bit_stream;
		pic->color_primary = -1;
		if (align != 0) {
			pic->width = ROUND_UP(width, align);
			pic->height = ROUND_UP(height, align);
		} else {
			pic->width = width;
			pic->height = height;
		}
		pic->line_stride = pic->width;

		/* A thumbnail only ever needs the first picture(s); the rest share their memory */
		if (!ci->thumbnail_mode || i < 2) {
			if (picture_alloc(fbm, pic, &align, width, height) != 0) {
				break;
			}
		} else {
			struct ve_picture *first = &fbm->frames[0].picture;

			pic->data0 = first->data0;
			pic->data1 = first->data1;
			pic->data2 = first->data2;
			pic->data3 = first->data3;
			pic->phy_y_addr = first->phy_y_addr;
			pic->phy_c_addr = first->phy_c_addr;
		}
	}
	fbm->align_value = align;

	if (i < fbm->max_frame_num) {
		LOG_ERR("only %d of %d pictures could be allocated", i, fbm->max_frame_num);
		while (i-- > 0) {
			if (!ci->thumbnail_mode || i < 2) {
				picture_free(fbm, &fbm->frames[i].picture);
			}
		}
		free(fbm->frames);
		free(fbm);
		return NULL;
	}

	info->buf_info.buf_num = ci->thumbnail_mode ? 1 : ci->frame_num;
	if (align != 0) {
		info->buf_info.buf_width = ROUND_UP(width, align);
		info->buf_info.buf_height = ROUND_UP(height, align);
	} else {
		info->buf_info.buf_width = width;
		info->buf_info.buf_height = height;
	}
	info->buf_info.align_value = align;
	info->buf_info.pixel_format = ci->pixel_format;
	info->buf_info.progressive = ci->progressive;
	info->buf_info.is_soft_decoder = ci->is_soft_decoder;
	info->buf_info.is_10bit_video = ci->is_10bit_stream;

	for (i = 0; i < ci->frame_num; i++) {
		node_enqueue(&fbm->empty_queue, &fbm->frames[i]);
	}
	fbm->empty_buffer_num = ci->frame_num;

	return fbm;
}

/* The first call makes the main pool, a second call the pool of the second output */
struct ve_fbm *FbmCreate(struct ve_fbm_create_info *ci, struct ve_fbm_info *info)
{
	if (info->fbm_first == NULL) {
		info->fbm_first = fbm_create_one(ci, info);
		return info->fbm_first;
	}
	if (info->fbm_second == NULL) {
		info->fbm_second = fbm_create_one(ci, info);
		info->minor_y_buf_offset = 0;
		info->minor_c_buf_offset = 0;
		return info->fbm_second;
	}

	return NULL;
}

void FbmDestroy(struct ve_fbm *fbm)
{
	if (fbm == NULL) {
		return;
	}
	for (int i = 0; i < fbm->max_frame_num; i++) {
		/* in thumbnail mode only the first pictures own memory */
		if (!fbm->thumbnail_mode || i < 2) {
			picture_free(fbm, &fbm->frames[i].picture);
		}
	}
	free(fbm->frames);
	free(fbm);
}

/* ---- decoder side ---- */

static struct ve_picture *request_buffer_locked(struct ve_fbm *fbm)
{
	struct ve_fbm_node *node = node_dequeue(&fbm->empty_queue);

	if (node == NULL) {
		return NULL;
	}
	if (node->flag.used_by_decoder || node->flag.in_valid_picture_queue ||
	    node->flag.used_by_render || node->flag.already_displayed) {
		LOG_ERR("picture %d taken from the empty queue is not free", node->picture.id);
		return NULL;
	}
	fbm->empty_buffer_num--;
	fbm->decoder_holding_num++;
	node->flag.used_by_decoder = 1;

	return &node->picture;
}

struct ve_picture *FbmRequestBuffer(struct ve_fbm *fbm)
{
	struct ve_picture *pic;

	if (fbm == NULL || fbm->fbm_info == NULL) {
		return NULL;
	}
	k_mutex_lock(fbm_mutex(fbm), K_FOREVER);
	pic = request_buffer_locked(fbm);
	k_mutex_unlock(fbm_mutex(fbm));

	return pic;
}

static struct ve_fbm_node *node_of(struct ve_fbm *fbm, struct ve_picture *pic, const char *who)
{
	int index = pic->id;

	if (index < 0 || index >= fbm->max_frame_num || pic != &fbm->frames[index].picture) {
		LOG_ERR("%s: picture %p has an invalid id %d", who, pic, index);
		return NULL;
	}

	return &fbm->frames[index];
}

static void return_buffer_locked(struct ve_fbm *fbm, struct ve_picture *pic, int valid)
{
	struct ve_fbm_node *node = node_of(fbm, pic, "FbmReturnBuffer");

	if (node == NULL) {
		return;
	}
	if (!node->flag.used_by_decoder) {
		LOG_ERR("picture %d returned by the decoder was not taken", pic->id);
		return;
	}
	node->flag.used_by_decoder = 0;

	if (node->flag.in_valid_picture_queue) {
		/* queued for the consumer while the decoder still referenced it: nothing to do */
		if (node->flag.used_by_render || node->flag.already_displayed) {
			LOG_ERR("picture %d is queued but also shown", pic->id);
			return;
		}
	} else if (node->flag.used_by_render) {
		/* the consumer holds it; it is freed when the consumer returns it */
	} else if (node->flag.already_displayed) {
		node->flag.already_displayed = 0;
		make_empty(fbm, node);
	} else if (valid) {
		node_enqueue(&fbm->valid_queue, node);
		fbm->valid_picture_num++;
		fbm->wait_for_disp_num++;
		node->flag.in_valid_picture_queue = 1;
	} else {
		make_empty(fbm, node);
	}
	fbm->decoder_holding_num--;
}

void FbmReturnBuffer(struct ve_fbm *fbm, struct ve_picture *pic, int valid)
{
	if (fbm == NULL || fbm->fbm_info == NULL || pic == NULL) {
		return;
	}
	k_mutex_lock(fbm_mutex(fbm), K_FOREVER);
	return_buffer_locked(fbm, pic, valid);
	k_mutex_unlock(fbm_mutex(fbm));
}

/*
 * The decoder finished a picture and keeps it as a reference: make it visible
 * to the consumer too. The engine writes tiled (32x32 macroblock) output, so
 * unless tiled output is wanted the picture is first converted by the engine
 * into a second, linear, picture and that one is queued instead.
 */
void FbmShareBuffer(struct ve_fbm *fbm, struct ve_picture *pic)
{
	struct ve_fbm_node *node;

	if (fbm == NULL || fbm->fbm_info == NULL || pic == NULL) {
		return;
	}
	node = node_of(fbm, pic, "FbmShareBuffer");
	if (node == NULL) {
		return;
	}

	k_mutex_lock(fbm_mutex(fbm), K_FOREVER);
	if (!node->flag.used_by_decoder || node->flag.in_valid_picture_queue ||
	    node->flag.already_displayed) {
		LOG_ERR("picture %d is shared in the wrong state", pic->id);
		k_mutex_unlock(fbm_mutex(fbm));
		return;
	}

	pic->output_mb32 = 0;
	if (pic->pixel_format == VE_PIX_YUV_MB32_420 || pic->pixel_format == VE_PIX_NV12) {
		struct ve_picture *out = request_buffer_locked(fbm);

		if (out != NULL) {
			out->pts = pic->pts;
			out->pixel_format = VE_PIX_NV12;
			out->aspect_ratio = pic->aspect_ratio;
			out->width = pic->width;
			out->height = pic->height;
			out->left_offset = pic->left_offset;
			out->top_offset = pic->top_offset;
			out->right_offset = pic->right_offset;
			out->bottom_offset = pic->bottom_offset;
			if (VideoEngineConvert(fbm->ve_ops, fbm->ve_ops_self, pic, out) != 0) {
				LOG_ERR("picture conversion failed");
				return_buffer_locked(fbm, out, 0);
				k_mutex_unlock(fbm_mutex(fbm));
				return;
			}
			return_buffer_locked(fbm, out, 1);
			node->flag.already_displayed = 1;
			k_mutex_unlock(fbm_mutex(fbm));
			return;
		}
		LOG_WRN("no spare picture for the conversion, queueing the tiled picture");
	}

	node->flag.in_valid_picture_queue = 1;
	node_enqueue(&fbm->valid_queue, node);
	fbm->valid_picture_num++;
	fbm->wait_for_disp_num++;
	k_mutex_unlock(fbm_mutex(fbm));
}

/* ---- consumer side ---- */

struct ve_picture *FbmRequestPicture(struct ve_fbm *fbm)
{
	struct ve_fbm_node *node;

	if (fbm == NULL || fbm->fbm_info == NULL) {
		return NULL;
	}
	k_mutex_lock(fbm_mutex(fbm), K_FOREVER);
	node = node_dequeue(&fbm->valid_queue);
	if (node != NULL) {
		if (!node->flag.in_valid_picture_queue || node->flag.used_by_render ||
		    node->flag.already_displayed) {
			LOG_ERR("picture %d taken from the valid queue is in the wrong state",
				node->picture.id);
			k_mutex_unlock(fbm_mutex(fbm));
			return NULL;
		}
		fbm->valid_picture_num--;
		fbm->wait_for_disp_num--;
		fbm->render_holding_num++;
		node->flag.in_valid_picture_queue = 0;
		node->flag.used_by_render = 1;
	}
	k_mutex_unlock(fbm_mutex(fbm));

	return node != NULL ? &node->picture : NULL;
}

int FbmReturnPicture(struct ve_fbm *fbm, struct ve_picture *pic)
{
	struct ve_fbm_node *node = NULL;
	int i;

	if (fbm == NULL || fbm->fbm_info == NULL || pic == NULL) {
		return 0;
	}

	if (pic->id >= 0 && pic->id < fbm->max_frame_num &&
	    pic == &fbm->frames[pic->id].picture) {
		node = &fbm->frames[pic->id];
	} else {
		/* a copy of a picture: find the original by its buffer */
		for (i = 0; i < fbm->max_frame_num; i++) {
			if (fbm->frames[i].picture.phy_y_addr == pic->phy_y_addr &&
			    fbm->frames[i].flag.used_by_render) {
				node = &fbm->frames[i];
				break;
			}
		}
	}
	if (node == NULL) {
		LOG_WRN("FbmReturnPicture: unknown picture");
		return -1;
	}

	k_mutex_lock(fbm_mutex(fbm), K_FOREVER);
	if (!node->flag.used_by_render || node->flag.in_valid_picture_queue ||
	    node->flag.already_displayed) {
		LOG_ERR("picture %d returned by the consumer is in the wrong state",
			node->picture.id);
		k_mutex_unlock(fbm_mutex(fbm));
		return -1;
	}
	node->flag.used_by_render = 0;
	fbm->render_holding_num--;
	if (node->flag.used_by_decoder) {
		node->flag.already_displayed = 1;
	} else {
		make_empty(fbm, node);
	}
	k_mutex_unlock(fbm_mutex(fbm));

	return 0;
}

struct ve_picture *FbmNextPictureInfo(struct ve_fbm *fbm)
{
	if (fbm == NULL || fbm->fbm_info == NULL || fbm->valid_queue == NULL) {
		return NULL;
	}

	return &fbm->valid_queue->picture;
}

/* Drop every picture still waiting for the consumer */
void FbmFlush(struct ve_fbm *fbm)
{
	struct ve_fbm_node *node;

	if (fbm == NULL || fbm->fbm_info == NULL) {
		return;
	}
	k_mutex_lock(fbm_mutex(fbm), K_FOREVER);
	while ((node = node_dequeue(&fbm->valid_queue)) != NULL) {
		fbm->valid_picture_num--;
		node->flag.in_valid_picture_queue = 0;
		if (!node->flag.used_by_decoder) {
			make_empty(fbm, node);
		} else {
			node->flag.already_displayed = 1;
		}
	}
	fbm->wait_for_disp_num = 0;
	k_mutex_unlock(fbm_mutex(fbm));
}

/* ---- queries ---- */

int FbmTotalBufferNum(struct ve_fbm *fbm)
{
	return fbm != NULL ? fbm->max_frame_num : 0;
}

int FbmEmptyBufferNum(struct ve_fbm *fbm)
{
	return fbm != NULL ? fbm->empty_buffer_num : 0;
}

int FbmValidPictureNum(struct ve_fbm *fbm)
{
	return fbm != NULL ? fbm->valid_picture_num : 0;
}

int FbmGetDisplayBufferNum(struct ve_fbm *fbm)
{
	return fbm != NULL ? fbm->render_holding_num + fbm->valid_picture_num : 0;
}

int FbmGetAlignValue(struct ve_fbm *fbm)
{
	return fbm != NULL ? fbm->align_value : 0;
}

/* Offset of the second output's planes inside a shared buffer (0: separate buffers) */
unsigned int FbmGetBufferOffset(struct ve_fbm *fbm, int is_y_buf)
{
	struct ve_fbm_info *info;

	if (fbm == NULL || fbm->fbm_info == NULL) {
		return 0;
	}
	info = fbm->fbm_info;
	if (fbm == info->fbm_first) {
		return 0;
	}

	return is_y_buf ? info->minor_y_buf_offset : info->minor_c_buf_offset;
}

/* Describe the pictures of a manager (the first one is representative) */
int FbmGetBufferInfo(struct ve_fbm *fbm, struct ve_picture *pic)
{
	const struct ve_picture *first;

	if (fbm == NULL || fbm->fbm_info == NULL) {
		return 0;
	}
	first = &fbm->frames[0].picture;
	pic->pixel_format = first->pixel_format;
	pic->width = first->width;
	pic->height = first->height;
	pic->line_stride = first->line_stride;
	pic->top_offset = first->top_offset;
	pic->left_offset = first->left_offset;
	pic->frame_rate = first->frame_rate;
	pic->aspect_ratio = first->aspect_ratio;
	pic->is_progressive = first->is_progressive;

	return 0;
}
