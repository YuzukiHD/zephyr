/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "ve_hw.h"
#include "ve_log.h"
#include "ve_mem.h"
#include "ve_pool.h"

VE_LOG_DECLARE();

#define POOL_MAX_WIDTH	7680
#define POOL_MAX_HEIGHT	4320

struct node {
	struct ve_picture pic;
	struct node *next;
	bool by_decoder;
	bool ready;		/* in the ready queue */
	bool by_consumer;
	bool shown;		/* the consumer had it while the decoder still referenced it */
	struct node *linear;	/* the engine's own linear copy of this tiled picture, if any */
};

struct ve_pool {
	struct k_mutex lock;
	struct node *nodes;
	int total;
	enum ve_pixel format;	/* of the pictures when the decoder gets them */
	int free_count;
	int ready_count;
	struct node *free_list;
	struct node *ready_list;
};

static void enqueue(struct node **head, struct node *n)
{
	struct node **tail = head;

	while (*tail != NULL) {
		tail = &(*tail)->next;
	}
	n->next = NULL;
	*tail = n;
}

static struct node *dequeue(struct node **head)
{
	struct node *n = *head;

	if (n != NULL) {
		*head = n->next;
		n->next = NULL;
	}

	return n;
}

static void make_free(struct ve_pool *pool, struct node *n)
{
	enqueue(&pool->free_list, n);
	pool->free_count++;
}

/* ---- picture memory ---- */

static int picture_alloc(struct ve_picture *pic, const struct ve_pool_config *cfg)
{
	size_t size_y, size_c;
	uint8_t *mem;

	pic->format = cfg->format;
	if (cfg->format == VE_PIX_RGBA) {
		pic->width = cfg->width;
		pic->height = cfg->height;
		pic->stride = cfg->width * 4;
		size_y = (size_t)pic->stride * cfg->height;
		size_c = 0;
	} else {
		/* odd sizes have a chroma sample more, and a 1 pixel picture still has a macroblock */
		int c_width = ROUND_UP((cfg->width + 1) >> 1, 16);
		int c_height = ROUND_UP((cfg->height + 1) >> 1, 32);

		pic->stride = ROUND_UP(cfg->width, 32);
		pic->width = pic->stride;
		pic->height = ROUND_UP(cfg->height, 32);
		size_y = (size_t)pic->stride * pic->height;
		size_c = 2 * (size_t)c_width * c_height;
	}
	pic->size = size_y + size_c;

	mem = ve_mem_alloc(pic->size);
	if (mem == NULL) {
		return -ENOMEM;
	}
	if (cfg->format == VE_PIX_RGBA) {
		memset(mem, 0xff, pic->size);
	}
	ve_mem_flush(mem, pic->size);
	pic->plane[0] = mem;
	pic->plane[1] = size_c != 0 ? mem + size_y : NULL;
	pic->right = cfg->width;
	pic->bottom = cfg->height;

	return 0;
}

struct ve_pool *ve_pool_create(const struct ve_pool_config *cfg)
{
	struct ve_pool *pool;
	int i;

	if (cfg->width >= POOL_MAX_WIDTH || cfg->height >= POOL_MAX_HEIGHT || cfg->frames <= 0) {
		LOG_ERR("cannot make a picture pool for %dx%d, %d frames", cfg->width, cfg->height,
			cfg->frames);
		return NULL;
	}
	pool = calloc(1, sizeof(*pool));
	if (pool == NULL) {
		return NULL;
	}
	pool->nodes = calloc(cfg->frames, sizeof(*pool->nodes));
	if (pool->nodes == NULL) {
		free(pool);
		return NULL;
	}
	k_mutex_init(&pool->lock);
	pool->total = cfg->frames;
	pool->format = cfg->format;

	for (i = 0; i < cfg->frames; i++) {
		pool->nodes[i].pic.id = i;
		if (picture_alloc(&pool->nodes[i].pic, cfg) != 0) {
			LOG_ERR("only %d of %d pictures could be allocated", i, cfg->frames);
			while (i-- > 0) {
				ve_mem_free(pool->nodes[i].pic.plane[0]);
			}
			free(pool->nodes);
			free(pool);
			return NULL;
		}
	}
	for (i = 0; i < cfg->frames; i++) {
		enqueue(&pool->free_list, &pool->nodes[i]);
	}
	pool->free_count = cfg->frames;

	return pool;
}

void ve_pool_destroy(struct ve_pool *pool)
{
	if (pool == NULL) {
		return;
	}
	for (int i = 0; i < pool->total; i++) {
		ve_mem_free(pool->nodes[i].pic.plane[0]);
	}
	free(pool->nodes);
	free(pool);
}

/* ---- decoder side ---- */

static struct ve_picture *acquire_locked(struct ve_pool *pool)
{
	struct node *n = dequeue(&pool->free_list);

	if (n == NULL) {
		return NULL;
	}
	if (n->by_decoder || n->ready || n->by_consumer || n->shown) {
		LOG_ERR("picture %d taken from the free list is not free", n->pic.id);
		return NULL;
	}
	pool->free_count--;
	n->by_decoder = true;
	/* a picture that served as a conversion target comes back as a plain buffer */
	n->pic.format = pool->format;

	return &n->pic;
}

struct ve_picture *ve_pool_acquire(struct ve_pool *pool)
{
	struct ve_picture *pic;

	k_mutex_lock(&pool->lock, K_FOREVER);
	pic = acquire_locked(pool);
	k_mutex_unlock(&pool->lock);

	return pic;
}

static struct node *node_of(struct ve_pool *pool, struct ve_picture *pic, const char *who)
{
	if (pic->id < 0 || pic->id >= pool->total || pic != &pool->nodes[pic->id].pic) {
		LOG_ERR("%s: picture %p has an invalid id %d", who, pic, pic->id);
		return NULL;
	}

	return &pool->nodes[pic->id];
}

static void queue_ready(struct ve_pool *pool, struct node *n)
{
	enqueue(&pool->ready_list, n);
	pool->ready_count++;
	n->ready = true;
}

static void return_locked(struct ve_pool *pool, struct ve_picture *pic, bool valid, bool convert);

/*
 * A tiled picture about to be shown is converted by the engine into a second, linear picture and
 * that one goes to the consumer instead. Returns the converted picture (already queued), or NULL
 * when the picture needs no conversion or none could be made.
 */
static struct ve_picture *convert_for_display(struct ve_pool *pool, struct ve_picture *pic)
{
	struct ve_picture *out;

	if (pic->format != VE_PIX_TILED_420) {
		return NULL;
	}
	if (pool->nodes[pic->id].linear != NULL) {
		struct node *l = pool->nodes[pic->id].linear;

		pool->nodes[pic->id].linear = NULL;
		out = &l->pic;
		out->pts = pic->pts;
		out->format = VE_PIX_NV12;
		out->width = pic->width;
		out->height = pic->height;
		out->left = pic->left;
		out->top = pic->top;
		out->right = pic->right;
		out->bottom = pic->bottom;
		return_locked(pool, out, true, false);

		return out;
	}
	out = acquire_locked(pool);
	if (out == NULL) {
		LOG_WRN("no spare picture for the conversion, showing the tiled picture");
		return NULL;
	}
	out->pts = pic->pts;
	out->format = VE_PIX_NV12;
	out->width = pic->width;
	out->height = pic->height;
	out->left = pic->left;
	out->top = pic->top;
	out->right = pic->right;
	out->bottom = pic->bottom;

	if (ve_hw_convert((uint32_t)(uintptr_t)pic->plane[0], (uint32_t)(uintptr_t)pic->plane[1],
			  (uint32_t)(uintptr_t)out->plane[0], pic->width, pic->height) != 0) {
		LOG_ERR("picture conversion failed");
		return_locked(pool, out, false, false);
		return NULL;
	}
	return_locked(pool, out, true, false);

	return out;
}

static void return_locked(struct ve_pool *pool, struct ve_picture *pic, bool valid, bool convert)
{
	struct node *n = node_of(pool, pic, "return");

	if (n == NULL) {
		return;
	}
	if (!n->by_decoder) {
		LOG_ERR("picture %d returned by the decoder was not taken", pic->id);
		return;
	}
	n->by_decoder = false;

	if (!valid && n->linear != NULL) {
		struct node *l = n->linear;

		n->linear = NULL;
		return_locked(pool, &l->pic, false, false);
	}

	if (n->ready) {
		/* queued for the consumer while the decoder still referenced it: nothing to do */
		if (n->by_consumer || n->shown) {
			LOG_ERR("picture %d is queued but also shown", pic->id);
		}
	} else if (n->by_consumer) {
		/* the consumer holds it; it is freed when the consumer returns it */
	} else if (n->shown) {
		n->shown = false;
		make_free(pool, n);
	} else if (valid) {
		if (convert && convert_for_display(pool, pic) != NULL) {
			/* the converted copy is what the consumer gets, this one is free again */
			make_free(pool, n);
		} else {
			queue_ready(pool, n);
		}
	} else {
		make_free(pool, n);
	}
}

int ve_pool_attach_linear(struct ve_pool *pool, struct ve_picture *pic, struct ve_picture *out)
{
	struct node *n = node_of(pool, pic, "attach");
	struct node *l = node_of(pool, out, "attach");
	int ret = -EINVAL;

	if (n == NULL || l == NULL) {
		return ret;
	}
	k_mutex_lock(&pool->lock, K_FOREVER);
	if (n->by_decoder && l->by_decoder && n->linear == NULL && pic->format == VE_PIX_TILED_420) {
		n->linear = l;
		ret = 0;
	}
	k_mutex_unlock(&pool->lock);

	return ret;
}

void ve_pool_submit(struct ve_pool *pool, struct ve_picture *pic)
{
	k_mutex_lock(&pool->lock, K_FOREVER);
	return_locked(pool, pic, true, true);
	k_mutex_unlock(&pool->lock);
}

void ve_pool_discard(struct ve_pool *pool, struct ve_picture *pic)
{
	k_mutex_lock(&pool->lock, K_FOREVER);
	return_locked(pool, pic, false, true);
	k_mutex_unlock(&pool->lock);
}

void ve_pool_share(struct ve_pool *pool, struct ve_picture *pic)
{
	struct node *n = node_of(pool, pic, "share");

	if (n == NULL) {
		return;
	}
	k_mutex_lock(&pool->lock, K_FOREVER);
	if (!n->by_decoder || n->ready || n->shown) {
		LOG_ERR("picture %d is shared in the wrong state", pic->id);
	} else if (convert_for_display(pool, pic) != NULL) {
		/* the decoder keeps referencing this picture; it is freed when it lets go */
		n->shown = true;
	} else {
		queue_ready(pool, n);
	}
	k_mutex_unlock(&pool->lock);
}

/* ---- consumer side ---- */

struct ve_picture *ve_pool_take(struct ve_pool *pool)
{
	struct node *n;

	k_mutex_lock(&pool->lock, K_FOREVER);
	n = dequeue(&pool->ready_list);
	if (n != NULL) {
		if (!n->ready || n->by_consumer || n->shown) {
			LOG_ERR("picture %d taken from the ready queue is in the wrong state",
				n->pic.id);
			k_mutex_unlock(&pool->lock);
			return NULL;
		}
		pool->ready_count--;
		n->ready = false;
		n->by_consumer = true;
	}
	k_mutex_unlock(&pool->lock);

	return n != NULL ? &n->pic : NULL;
}

int ve_pool_give_back(struct ve_pool *pool, struct ve_picture *pic)
{
	struct node *n = node_of(pool, pic, "give back");

	if (n == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&pool->lock, K_FOREVER);
	if (!n->by_consumer || n->ready || n->shown) {
		LOG_ERR("picture %d returned by the consumer is in the wrong state", pic->id);
		k_mutex_unlock(&pool->lock);
		return -EINVAL;
	}
	n->by_consumer = false;
	if (n->by_decoder) {
		n->shown = true;
	} else {
		make_free(pool, n);
	}
	k_mutex_unlock(&pool->lock);

	return 0;
}

void ve_pool_flush(struct ve_pool *pool)
{
	struct node *n;

	k_mutex_lock(&pool->lock, K_FOREVER);
	while ((n = dequeue(&pool->ready_list)) != NULL) {
		pool->ready_count--;
		n->ready = false;
		if (!n->by_decoder) {
			make_free(pool, n);
		} else {
			n->shown = true;
		}
	}
	k_mutex_unlock(&pool->lock);
}

int ve_pool_total(const struct ve_pool *pool)
{
	return pool->total;
}

int ve_pool_free(const struct ve_pool *pool)
{
	return pool->free_count;
}

int ve_pool_ready(const struct ve_pool *pool)
{
	return pool->ready_count;
}
