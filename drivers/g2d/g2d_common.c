/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/g2d.h>
#include <zephyr/sys/atomic.h>

/*
 * The context of a synchronous run lives on the heap: when the caller gives
 * up waiting, the operation may still complete later and its callback must
 * not touch a stack frame that is gone.
 */
enum run_state {
	RUN_WAITING,
	RUN_DONE,
	RUN_ABANDONED,
};

struct run_ctx {
	struct k_sem done;
	atomic_t state;
	int status;
	g2d_callback_t callback;
	void *user_data;
};

static void run_callback(const struct device *dev, const struct g2d_op *op, int status,
			 void *user_data)
{
	struct run_ctx *ctx = user_data;

	if (ctx->callback != NULL) {
		ctx->callback(dev, op, status, ctx->user_data);
	}
	ctx->status = status;
	if (atomic_cas(&ctx->state, RUN_WAITING, RUN_DONE)) {
		k_sem_give(&ctx->done);
	} else {
		k_free(ctx);
	}
}

int g2d_run(const struct device *dev, const struct g2d_op *op, k_timeout_t timeout)
{
	struct g2d_op copy = *op;
	struct run_ctx *ctx;
	int ret;

	ctx = k_malloc(sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	k_sem_init(&ctx->done, 0, 1);
	atomic_set(&ctx->state, RUN_WAITING);
	ctx->callback = op->callback;
	ctx->user_data = op->user_data;
	copy.callback = run_callback;
	copy.user_data = ctx;

	ret = g2d_submit(dev, &copy);
	if (ret) {
		k_free(ctx);
		return ret;
	}

	if (k_sem_take(&ctx->done, timeout) != 0) {
		if (atomic_cas(&ctx->state, RUN_WAITING, RUN_ABANDONED)) {
			/* the callback frees the context when it finally runs */
			return -EAGAIN;
		}
		/* the callback got there first */
		k_sem_take(&ctx->done, K_FOREVER);
	}
	ret = ctx->status;
	k_free(ctx);
	return ret;
}

int g2d_fill(const struct device *dev, const struct g2d_surface *dst,
	     const struct g2d_rect *dst_rect, uint32_t color)
{
	struct g2d_op op = {
		.type = G2D_OP_FILL,
		.dst = *dst,
		.dst_rect = *dst_rect,
		.color = color,
	};

	return g2d_run(dev, &op, K_FOREVER);
}

int g2d_blit(const struct device *dev, const struct g2d_surface *src,
	     const struct g2d_rect *src_rect, const struct g2d_surface *dst,
	     const struct g2d_rect *dst_rect, enum g2d_rotation rotation, uint32_t flags)
{
	struct g2d_op op = {
		.type = G2D_OP_BLIT,
		.flags = flags,
		.src = *src,
		.src_rect = *src_rect,
		.dst = *dst,
		.dst_rect = *dst_rect,
		.rotation = rotation,
	};

	return g2d_run(dev, &op, K_FOREVER);
}

int g2d_blend(const struct device *dev, const struct g2d_surface *fg,
	      const struct g2d_rect *fg_rect, const struct g2d_surface *bg,
	      const struct g2d_rect *bg_rect, const struct g2d_surface *dst,
	      const struct g2d_rect *dst_rect, const struct g2d_blend *blend, uint32_t flags)
{
	struct g2d_op op = {
		.type = G2D_OP_BLEND,
		.flags = flags,
		.src = *fg,
		.src_rect = *fg_rect,
		.bg = *bg,
		.bg_rect = *bg_rect,
		.dst = *dst,
		.dst_rect = *dst_rect,
		.blend = *blend,
	};

	return g2d_run(dev, &op, K_FOREVER);
}
