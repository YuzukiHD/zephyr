/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Allwinner G2D driver.
 *
 * submit() validates an operation and builds its register command list into
 * a job taken from a fixed pool; a worker thread feeds the jobs to the
 * hardware one at a time, the task-end interrupt completes them.
 */

#define DT_DRV_COMPAT allwinner_sunxi_g2d

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/g2d.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

#include "sunxi/g2d_sunxi_hw.h"
#include "sunxi/g2d_sunxi_regs.h"

LOG_MODULE_REGISTER(g2d_sunxi, CONFIG_G2D_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1, "one G2D only");

#define QUEUE_DEPTH		DT_INST_PROP(0, queue_depth)
#define MAX_SIZE		8192U
#define THREAD_ID		0
#define CMDLIST_BYTES		2560U

#define PLL_PERI_REG		0x0020U
#define HOSC_RATE		24000000U
#define MOD_CLK_MUX_MASK	GENMASK(26, 24)
#define MOD_CLK_DIV_MASK	GENMASK(4, 0)
#define MOD_CLK_GATE		BIT(31)

/* internal cache and bandwidth limiter, values of the vendor driver */
#define CACHE_SIZE		0x40U
#define DDR_LIMIT		0x90U

struct g2d_sunxi_config {
	uintptr_t base;
	uintptr_t ccu;
	uint16_t mod_reg;
	uint32_t rate;
	const struct device *clock_dev;
	uint32_t bus_clock_id;
	uint32_t mbus_clock_id;
	struct reset_dt_spec reset;
	void (*irq_config)(void);
};

struct g2d_sunxi_data {
	struct k_thread thread;
	struct k_sem hw_done;
	/* interrupt status of the last finished command list */
	uint32_t hw_status;
};

/* one queued operation with its command list */
struct g2d_job {
	struct g2d_op op;
	struct g2d_cmdlist cl;
	size_t head_len;
	uint8_t cmd[CMDLIST_BYTES] __aligned(64);
};

K_MEM_SLAB_DEFINE_STATIC(g2d_jobs, sizeof(struct g2d_job), QUEUE_DEPTH, 64);
K_MSGQ_DEFINE(g2d_queue, sizeof(struct g2d_job *), QUEUE_DEPTH, sizeof(void *));
static K_THREAD_STACK_DEFINE(g2d_stack, CONFIG_G2D_SUNXI_THREAD_STACK_SIZE);

/* ---- register access ----------------------------------------------------- */

static inline uint32_t g2d_rd(const struct device *dev, uint32_t off)
{
	const struct g2d_sunxi_config *cfg = dev->config;

	return sys_read32(cfg->base + off);
}

static inline void g2d_wr(const struct device *dev, uint32_t off, uint32_t val)
{
	const struct g2d_sunxi_config *cfg = dev->config;

	sys_write32(val, cfg->base + off);
}

static void g2d_upd(const struct device *dev, uint32_t off, uint32_t mask, uint32_t val)
{
	g2d_wr(dev, off, (g2d_rd(dev, off) & ~mask) | (val & mask));
}

/* ---- hardware control ------------------------------------------------------ */

/* pulse a reset bit: the hardware is out of reset when the bit is set */
static void g2d_pulse_reset(const struct device *dev, uint32_t bits)
{
	g2d_upd(dev, G2D_RESET, bits, 0);
	g2d_upd(dev, G2D_RESET, bits, bits);
}

static void g2d_hw_open(const struct device *dev)
{
	g2d_upd(dev, G2D_CLK_GATE, G2D_CLK_GATE_CORE, G2D_CLK_GATE_CORE);
	g2d_upd(dev, G2D_MBUS_GATE, G2D_MBUS_GATE_CLK | G2D_MBUS_GATE_RESET,
		G2D_MBUS_GATE_CLK | G2D_MBUS_GATE_RESET);
	g2d_upd(dev, G2D_RESET, G2D_RESET_CORE, G2D_RESET_CORE);

	g2d_upd(dev, G2D_CLK_GATE, G2D_CLK_GATE_THREAD(THREAD_ID), G2D_CLK_GATE_THREAD(THREAD_ID));
	g2d_upd(dev, G2D_RESET, G2D_RESET_THREAD(THREAD_ID), G2D_RESET_THREAD(THREAD_ID));

	g2d_upd(dev, G2D_CACHE_CTRL, G2D_CACHE_CTRL_EN, G2D_CACHE_CTRL_EN);
	g2d_upd(dev, G2D_CACHE_SIZE0, G2D_CACHE_SIZE_MASK, CACHE_SIZE);
	g2d_upd(dev, G2D_CACHE_SIZE1, G2D_CACHE_SIZE_MASK, CACHE_SIZE);

	/* keep the G2D from starving the display engine and the CPU of memory bandwidth */
	g2d_upd(dev, G2D_DDR_LIMIT, G2D_DDR_LIMIT_EN | G2D_DDR_LIMIT_MASK,
		G2D_DDR_LIMIT_EN | DDR_LIMIT);
}

static void g2d_hw_recover(const struct device *dev)
{
	g2d_pulse_reset(dev, G2D_RESET_CORE);
	g2d_pulse_reset(dev, G2D_RESET_THREAD(THREAD_ID));
}

static void g2d_hw_start(const struct device *dev, const struct g2d_job *job)
{
	const uint32_t thread = G2D_THREAD(THREAD_ID);

	g2d_upd(dev, G2D_MODE, G2D_MODE_MASTER, G2D_MODE_MASTER);
	g2d_wr(dev, thread + G2D_THREAD_IRQ_EN, G2D_THREAD_IRQ_TASK_END | G2D_THREAD_IRQ_TIMEOUT);
	g2d_wr(dev, thread + G2D_THREAD_HEAD_LOW, (uint32_t)(uintptr_t)job->cmd);
	g2d_wr(dev, thread + G2D_THREAD_HEAD_HIGH_LEN,
	       FIELD_PREP(G2D_THREAD_HEAD_LEN_MASK, job->head_len));
	g2d_wr(dev, thread + G2D_THREAD_ATTR,
	       FIELD_PREP(G2D_THREAD_ATTR_CMD_NUM_MASK, 0) | G2D_THREAD_ATTR_END_IRQ);
	g2d_wr(dev, thread + G2D_THREAD_UPDATE, 1);
}

static void g2d_dump(const struct device *dev)
{
	uint32_t off;

	for (off = 0; off < 0x60; off += 0x10) {
		LOG_ERR("%03x: %08x %08x %08x %08x", off, g2d_rd(dev, off), g2d_rd(dev, off + 4),
			g2d_rd(dev, off + 8), g2d_rd(dev, off + 12));
	}
	LOG_ERR("thread: %08x %08x", g2d_rd(dev, G2D_THREAD(THREAD_ID) + G2D_THREAD_IRQ_EN),
		g2d_rd(dev, G2D_THREAD(THREAD_ID) + G2D_THREAD_IRQ_STATUS));
}

static void g2d_sunxi_isr(const struct device *dev)
{
	struct g2d_sunxi_data *data = dev->data;
	const uint32_t status_reg = G2D_THREAD(THREAD_ID) + G2D_THREAD_IRQ_STATUS;
	uint32_t status = g2d_rd(dev, status_reg);

	g2d_wr(dev, status_reg, status);	/* write 1 to clear */
	if (status & (G2D_THREAD_IRQ_TASK_END | G2D_THREAD_IRQ_TIMEOUT)) {
		g2d_pulse_reset(dev, G2D_RESET_THREAD(THREAD_ID));
		data->hw_status = status;
		k_sem_give(&data->hw_done);
	}
}

/* ---- cache maintenance --------------------------------------------------------- */

enum cache_op {
	CACHE_CLEAN,
	CACHE_CLEAN_INVALIDATE,
	CACHE_INVALIDATE,
};

static void cache_rect(const struct g2d_surface *s, const struct g2d_rect *rect, enum cache_op op)
{
	const struct g2d_fmt_info *fmt = g2d_fmt_get(s->format);
	unsigned int p;

	for (p = 0; fmt != NULL && p < fmt->planes; p++) {
		void *start;
		size_t len;

		if (g2d_surface_rect_span(s, rect, p, &start, &len)) {
			continue;
		}
		switch (op) {
		case CACHE_CLEAN:
			sys_cache_data_flush_range(start, len);
			break;
		case CACHE_CLEAN_INVALIDATE:
			sys_cache_data_flush_and_invd_range(start, len);
			break;
		default:
			sys_cache_data_invd_range(start, len);
			break;
		}
	}
}

static void job_cache_before(const struct g2d_op *op)
{
	if (op->flags & G2D_FLAG_NO_CACHE_OPS) {
		return;
	}
	/* dirty lines of the destination must not be written back over the result */
	cache_rect(&op->dst, &op->dst_rect, CACHE_CLEAN_INVALIDATE);
	if (op->type != G2D_OP_FILL) {
		cache_rect(&op->src, &op->src_rect, CACHE_CLEAN);
	}
	if (op->type == G2D_OP_BLEND) {
		cache_rect(&op->bg, &op->bg_rect, CACHE_CLEAN);
	}
}

static void job_cache_after(const struct g2d_op *op)
{
	if (!(op->flags & G2D_FLAG_NO_CACHE_OPS)) {
		cache_rect(&op->dst, &op->dst_rect, CACHE_INVALIDATE);
	}
}

/* ---- worker --------------------------------------------------------------------- */

static int g2d_sunxi_run(const struct device *dev, struct g2d_job *job)
{
	struct g2d_sunxi_data *data = dev->data;
	int ret = 0;

	job_cache_before(&job->op);

	k_sem_reset(&data->hw_done);
	g2d_hw_start(dev, job);
	if (k_sem_take(&data->hw_done, K_MSEC(CONFIG_G2D_SUNXI_TIMEOUT_MS)) != 0) {
		LOG_ERR("operation %d timed out", job->op.type);
		g2d_dump(dev);
		g2d_hw_recover(dev);
		ret = -ETIMEDOUT;
	} else if (data->hw_status & G2D_THREAD_IRQ_TIMEOUT) {
		LOG_ERR("hardware timeout in operation %d", job->op.type);
		g2d_hw_recover(dev);
		ret = -EIO;
	}

	job_cache_after(&job->op);
	return ret;
}

static void g2d_sunxi_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct g2d_job *job;
	int status;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_msgq_get(&g2d_queue, &job, K_FOREVER);
		status = g2d_sunxi_run(dev, job);
		if (job->op.callback != NULL) {
			job->op.callback(dev, &job->op, status, job->op.user_data);
		}
		k_mem_slab_free(&g2d_jobs, job);
	}
}

/* ---- driver API ------------------------------------------------------------------- */

static int g2d_sunxi_submit(const struct device *dev, const struct g2d_op *op)
{
	struct g2d_job *job;
	int ret;

	ARG_UNUSED(dev);

	if (k_mem_slab_alloc(&g2d_jobs, (void **)&job, K_NO_WAIT) != 0) {
		return -ENOMEM;
	}

	job->op = *op;
	g2d_cmdlist_init(&job->cl, job->cmd, sizeof(job->cmd));
	ret = g2d_compose(&job->cl, &job->op);
	if (ret) {
		k_mem_slab_free(&g2d_jobs, job);
		return ret;
	}
	job->head_len = g2d_cmdlist_finish(&job->cl);

	/* a job holds one queue entry, so the queue cannot overflow */
	k_msgq_put(&g2d_queue, &job, K_NO_WAIT);
	return 0;
}

static int g2d_sunxi_get_capabilities(const struct device *dev, struct g2d_capabilities *caps)
{
	ARG_UNUSED(dev);

	caps->src_formats = BIT(G2D_PIXFMT_MAX) - 1U;
	caps->dst_formats = BIT(G2D_PIXFMT_NV12) - 1U;
	caps->ops = BIT(G2D_OP_FILL) | BIT(G2D_OP_BLIT) | BIT(G2D_OP_BLEND);
	caps->max_width = MAX_SIZE;
	caps->max_height = MAX_SIZE;
	caps->rotate_addr_align = 4;
	caps->rotate_pitch_align = 8;
	caps->queue_depth = QUEUE_DEPTH;
	return 0;
}

static DEVICE_API(g2d, g2d_sunxi_api) = {
	.submit = g2d_sunxi_submit,
	.get_capabilities = g2d_sunxi_get_capabilities,
};

/* ---- initialisation ------------------------------------------------------------------- */

static uint32_t pll_peri_rate(const struct g2d_sunxi_config *cfg)
{
	uint32_t pll = sys_read32(cfg->ccu + PLL_PERI_REG);
	uint32_t n = ((pll >> 8) & 0xff) + 1;
	uint32_t p0 = ((pll >> 16) & 0x7) + 1;
	uint32_t m = (pll & BIT(1)) ? 2 : 1;

	return HOSC_RATE / m / p0 * n;
}

static int g2d_sunxi_clock_init(const struct g2d_sunxi_config *cfg)
{
	uint32_t src = pll_peri_rate(cfg);
	uint32_t div = CLAMP(DIV_ROUND_UP(src, cfg->rate), 1U, 32U);
	uint32_t reg;
	int ret;

	ret = clock_control_on(cfg->clock_dev, (clock_control_subsys_t)(uintptr_t)cfg->bus_clock_id);
	if (ret == 0) {
		ret = clock_control_on(cfg->clock_dev,
				       (clock_control_subsys_t)(uintptr_t)cfg->mbus_clock_id);
	}
	if (ret == 0) {
		ret = reset_line_toggle_dt(&cfg->reset);
	}
	if (ret) {
		return ret;
	}

	/* module clock: PLL_PERI_2X / div */
	reg = sys_read32(cfg->ccu + cfg->mod_reg);
	reg &= ~(MOD_CLK_GATE | MOD_CLK_MUX_MASK | MOD_CLK_DIV_MASK);
	reg |= div - 1;
	sys_write32(reg, cfg->ccu + cfg->mod_reg);
	sys_write32(reg | MOD_CLK_GATE, cfg->ccu + cfg->mod_reg);
	LOG_INF("module clock %u Hz (PLL_PERI_2X %u Hz / %u)", src / div, src, div);
	return 0;
}

static int g2d_sunxi_init(const struct device *dev)
{
	const struct g2d_sunxi_config *cfg = dev->config;
	struct g2d_sunxi_data *data = dev->data;
	int ret;

	k_sem_init(&data->hw_done, 0, 1);

	ret = g2d_sunxi_clock_init(cfg);
	if (ret) {
		LOG_ERR("clock setup failed: %d", ret);
		return ret;
	}

	g2d_hw_open(dev);
	cfg->irq_config();

	k_thread_create(&data->thread, g2d_stack, K_THREAD_STACK_SIZEOF(g2d_stack),
			g2d_sunxi_thread, (void *)dev, NULL, NULL,
			K_PRIO_PREEMPT(CONFIG_G2D_SUNXI_THREAD_PRIORITY), 0, K_NO_WAIT);
	k_thread_name_set(&data->thread, "g2d");

	LOG_INF("ready, queue depth %u", QUEUE_DEPTH);
	return 0;
}

static void g2d_sunxi_irq_config(void)
{
	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority), g2d_sunxi_isr,
		    DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));
}

static const struct g2d_sunxi_config g2d_sunxi_config0 = {
	.base = DT_INST_REG_ADDR(0),
	.ccu = DT_INST_PROP(0, ccu_base),
	.mod_reg = DT_INST_PROP(0, mod_clk_reg),
	.rate = DT_INST_PROP(0, clock_frequency),
	.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(0, bus)),
	.bus_clock_id = DT_INST_CLOCKS_CELL_BY_NAME(0, bus, clkid),
	.mbus_clock_id = DT_INST_CLOCKS_CELL_BY_NAME(0, mbus, clkid),
	.reset = RESET_DT_SPEC_INST_GET(0),
	.irq_config = g2d_sunxi_irq_config,
};

static struct g2d_sunxi_data g2d_sunxi_data0;

DEVICE_DT_INST_DEFINE(0, g2d_sunxi_init, NULL, &g2d_sunxi_data0, &g2d_sunxi_config0, POST_KERNEL,
		      CONFIG_G2D_INIT_PRIORITY, &g2d_sunxi_api);
