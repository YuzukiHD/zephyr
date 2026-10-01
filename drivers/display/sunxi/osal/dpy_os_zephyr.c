// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - OS port for Zephyr on the Allwinner F101.
 *
 * Implements every function of include/dpy/dpy_os.h. Zephyr's clock control
 * driver only knows the UART clocks of this SoC, so the display clocks and
 * resets (PLL_VIDEO0, DE, TCON, DPSS_TOP, DSI, combo PHY) are driven here
 * directly through the CCU registers.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/pinctrl/pinctrl_sunxi.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/irq.h>
#include <zephyr/irq_multilevel.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <dpy/dpy_os.h>

#include "dpy_os_zephyr.h"

#include <zephyr/dt-bindings/clock/sun252i-f101-ccu.h>

#define DPY_CACHE_LINE		64
#define DPY_CCU_BASE		0x02001000UL
#define DPY_PIO_BASE		0x02000000UL
#define DPY_HOSC_HZ		24000000U
#define DPY_IRQ_PRIO		1

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */
void *dpy_os_zalloc(size_t size)
{
	void *p = k_malloc(size);

	if (p) {
		memset(p, 0, size);
	}
	return p;
}

void dpy_os_free(void *ptr)
{
	if (ptr) {
		k_free(ptr);
	}
}

void *dpy_os_dma_alloc(size_t size, size_t align, dpy_dma_addr_t *dma)
{
	void *p;

	if (align < DPY_CACHE_LINE) {
		align = DPY_CACHE_LINE;
	}
	/* round the size up so cache maintenance never touches neighbours */
	size = ROUND_UP(size, DPY_CACHE_LINE);
	p = k_aligned_alloc(align, size);
	if (!p) {
		return NULL;
	}
	memset(p, 0, size);
	sys_cache_data_flush_range(p, size);
	if (dma) {
		*dma = (dpy_dma_addr_t)(uintptr_t)p;
	}
	return p;
}

void dpy_os_dma_free(void *ptr)
{
	if (ptr) {
		k_free(ptr);
	}
}

dpy_dma_addr_t dpy_os_virt_to_dma(const void *ptr)
{
	/* the SoC has no MMU: virtual == physical */
	return (dpy_dma_addr_t)(uintptr_t)ptr;
}

static void dpy_cache_range(const void *ptr, size_t size, void **start,
			    size_t *len)
{
	uintptr_t s = (uintptr_t)ptr & ~(uintptr_t)(DPY_CACHE_LINE - 1);
	uintptr_t e = ROUND_UP((uintptr_t)ptr + size, DPY_CACHE_LINE);

	*start = (void *)s;
	*len = e - s;
}

void dpy_os_dcache_clean(const void *ptr, size_t size)
{
	void *start;
	size_t len;

	if (!size) {
		return;
	}
	dpy_cache_range(ptr, size, &start, &len);
	sys_cache_data_flush_range(start, len);
}

void dpy_os_dcache_invalidate(void *ptr, size_t size)
{
	void *start;
	size_t len;

	if (!size) {
		return;
	}
	dpy_cache_range(ptr, size, &start, &len);
	sys_cache_data_invd_range(start, len);
}

/* ------------------------------------------------------------------ */
/* Register access                                                     */
/* ------------------------------------------------------------------ */
uintptr_t dpy_os_ioremap(uintptr_t phys, size_t size)
{
	ARG_UNUSED(size);
	/* registers are identity mapped on this SoC */
	return phys;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */
void dpy_os_udelay(uint32_t us)
{
	k_busy_wait(us);
}

void dpy_os_msleep(uint32_t ms)
{
	if (dpy_os_in_irq() || k_is_pre_kernel()) {
		k_busy_wait(ms * 1000U);
		return;
	}
	k_msleep(ms);
}

uint64_t dpy_os_time_us(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

/* ------------------------------------------------------------------ */
/* Synchronisation                                                     */
/* ------------------------------------------------------------------ */
struct dpy_mutex {
	struct k_mutex m;
};

struct dpy_sem {
	struct k_sem s;
};

struct dpy_mutex *dpy_os_mutex_create(void)
{
	struct dpy_mutex *m = k_malloc(sizeof(*m));

	if (m) {
		k_mutex_init(&m->m);
	}
	return m;
}

void dpy_os_mutex_destroy(struct dpy_mutex *m)
{
	if (m) {
		k_free(m);
	}
}

void dpy_os_mutex_lock(struct dpy_mutex *m)
{
	k_mutex_lock(&m->m, K_FOREVER);
}

void dpy_os_mutex_unlock(struct dpy_mutex *m)
{
	k_mutex_unlock(&m->m);
}

struct dpy_sem *dpy_os_sem_create(uint32_t initial)
{
	struct dpy_sem *s = k_malloc(sizeof(*s));

	if (s) {
		k_sem_init(&s->s, initial, K_SEM_MAX_LIMIT);
	}
	return s;
}

void dpy_os_sem_destroy(struct dpy_sem *s)
{
	if (s) {
		k_free(s);
	}
}

void dpy_os_sem_post(struct dpy_sem *s)
{
	k_sem_give(&s->s);
}

int dpy_os_sem_wait(struct dpy_sem *s, uint32_t timeout_ms)
{
	k_timeout_t to;

	if (timeout_ms == DPY_WAIT_FOREVER) {
		to = K_FOREVER;
	} else if (!timeout_ms) {
		to = K_NO_WAIT;
	} else {
		to = K_MSEC(timeout_ms);
	}
	return k_sem_take(&s->s, to) ? -ETIMEDOUT : 0;
}

void dpy_os_spin_init(dpy_spinlock_t *lock)
{
	memset(lock, 0, sizeof(*lock));
}

unsigned long dpy_os_spin_lock_irqsave(dpy_spinlock_t *lock)
{
	ARG_UNUSED(lock);
	/* single core: masking interrupts is the whole critical section */
	return (unsigned long)irq_lock();
}

void dpy_os_spin_unlock_irqrestore(dpy_spinlock_t *lock, unsigned long flags)
{
	ARG_UNUSED(lock);
	irq_unlock((unsigned int)flags);
}

bool dpy_os_in_irq(void)
{
	return k_is_in_isr();
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */
#define DPY_OS_MAX_IRQS 4

static struct {
	int irq;
	dpy_irq_handler_t handler;
	void *data;
} dpy_os_irqs[DPY_OS_MAX_IRQS];

static void dpy_os_irq_trampoline(const void *arg)
{
	uintptr_t slot = (uintptr_t)arg;

	dpy_os_irqs[slot].handler(dpy_os_irqs[slot].data);
}

/* @irq is the Zephyr (multilevel encoded) interrupt number from the DT */
static unsigned int dpy_irqn(int irq)
{
	return (unsigned int)irq;
}

int dpy_os_request_irq(int irq, dpy_irq_handler_t handler, const char *name,
		       void *data)
{
	uintptr_t i;
	int ret;

	ARG_UNUSED(name);

	for (i = 0; i < DPY_OS_MAX_IRQS; i++) {
		if (!dpy_os_irqs[i].handler) {
			break;
		}
	}
	if (i == DPY_OS_MAX_IRQS) {
		return -ENOSPC;
	}

	dpy_os_irqs[i].irq = irq;
	dpy_os_irqs[i].data = data;
	dpy_os_irqs[i].handler = handler;

	ret = irq_connect_dynamic(dpy_irqn(irq), DPY_IRQ_PRIO,
				  dpy_os_irq_trampoline, (const void *)i, 0);
	if (ret < 0) {
		dpy_os_irqs[i].handler = NULL;
		return -EIO;
	}
	irq_enable(dpy_irqn(irq));
	return 0;
}

void dpy_os_free_irq(int irq, void *data)
{
	int i;

	for (i = 0; i < DPY_OS_MAX_IRQS; i++) {
		if (dpy_os_irqs[i].handler && dpy_os_irqs[i].irq == irq &&
		    dpy_os_irqs[i].data == data) {
			irq_disable(dpy_irqn(irq));
			dpy_os_irqs[i].handler = NULL;
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Clocks and resets                                                   */
/* ------------------------------------------------------------------ */
struct dpy_clk {
	uint32_t id;
};

struct dpy_reset {
	uint32_t id;
};

/* PLL_PERI (0x20): N[15:8], P0[18:16], input /2 bit 1 (see cctl driver) */
#define PLL_PERI_REG		0x0020
/* PLL_VIDEO0 (0x40): N[15:8], LDO bit 30, enable 31, output gate 27, lock 29/28 */
#define PLL_VIDEO0_REG		0x0040
#define PLL_VIDEO0_N_SHIFT	8
#define PLL_VIDEO0_N_MASK	(0xffU << PLL_VIDEO0_N_SHIFT)
#define PLL_VIDEO0_INPUT_DIV2	BIT(1)	/* reference divided by two */
#define PLL_VIDEO0_LDO		BIT(30)
#define PLL_VIDEO0_EN		BIT(31)
#define PLL_VIDEO0_OUT		BIT(27)
#define PLL_VIDEO0_LOCK_EN	BIT(29)
#define PLL_VIDEO0_LOCKED	BIT(28)
#define PLL_VIDEO0_MIN_HZ	288000000U
#define PLL_VIDEO0_MAX_HZ	2400000000U

#define MAX_PARENTS 4

/* module clock kinds */
enum dpy_clk_kind {
	CLK_KIND_PLL_PERI_2X,
	CLK_KIND_PLL_PERI_1X,
	CLK_KIND_PLL_VIDEO0_4X,
	CLK_KIND_MUXDIV,	/* gate 31, mux 26:24, linear div at bit 0 */
	CLK_KIND_MP,		/* gate 31, mux 26:24, P 9:8, M 3:0 (TCON) */
	CLK_KIND_BUS,		/* gate at bit 0 */
};

struct dpy_clk_desc {
	uint32_t id;
	uint8_t kind;
	uint8_t div_width;
	uint16_t reg;
	/* mux index -> parent clock id (0 = invalid) */
	uint32_t parents[MAX_PARENTS];
};

static const struct dpy_clk_desc dpy_clks[] = {
	{ CLK_PLL_PERI_2X, CLK_KIND_PLL_PERI_2X },
	{ CLK_PLL_PERI_1X, CLK_KIND_PLL_PERI_1X },
	{ CLK_PLL_VIDEO0_4X, CLK_KIND_PLL_VIDEO0_4X },
	{ CLK_DE, CLK_KIND_MUXDIV, 5, 0x0600,
	  { CLK_PLL_PERI_2X, CLK_PLL_VIDEO0_4X } },
	{ CLK_BUS_DE, CLK_KIND_BUS, 0, 0x060c },
	{ CLK_COMBOPHY0, CLK_KIND_MUXDIV, 5, 0x0aa0,
	  { CLK_PLL_VIDEO0_4X, CLK_PLL_PERI_2X } },
	{ CLK_BUS_COMBOPHY0, CLK_KIND_BUS, 0, 0x0aa4 },
	{ CLK_BUS_DPSS_TOP, CLK_KIND_BUS, 0, 0x0abc },
	/* DSI: 0 = HOSC, 1 = PERI_1X, 2 = VIDEO0_2X (not modelled: unused) */
	{ CLK_DSI, CLK_KIND_MUXDIV, 4, 0x0b24,
	  { 0, CLK_PLL_PERI_1X } },
	{ CLK_BUS_DSI, CLK_KIND_BUS, 0, 0x0b4c },
	/* TCON: 0 = VIDEO0_1X (4X / 4), 1 = VIDEO0_4X, 2 = PERI_2X */
	{ CLK_TCONLCD, CLK_KIND_MP, 0, 0x0b60,
	  { 0, CLK_PLL_VIDEO0_4X, CLK_PLL_PERI_2X } },
	{ CLK_BUS_TCONLCD, CLK_KIND_BUS, 0, 0x0b7c },
};

static struct dpy_clk dpy_clk_pool[ARRAY_SIZE(dpy_clks)];

struct dpy_reset_desc {
	uint32_t id;
	uint16_t reg;
};

static const struct dpy_reset_desc dpy_resets[] = {
	{ RST_BUS_DE, 0x060c },
	{ RST_BUS_DPSS_TOP, 0x0abc },
	{ RST_BUS_DSI, 0x0b4c },
	{ RST_BUS_TCONLCD, 0x0b7c },
	{ RST_BUS_LVDS0, 0x0bac },
};

static struct dpy_reset dpy_reset_pool[ARRAY_SIZE(dpy_resets)];

static uint32_t ccu_rd(uint32_t off)
{
	return sys_read32(DPY_CCU_BASE + off);
}

static void ccu_wr(uint32_t off, uint32_t val)
{
	sys_write32(val, DPY_CCU_BASE + off);
}

static void ccu_upd(uint32_t off, uint32_t mask, uint32_t val)
{
	ccu_wr(off, (ccu_rd(off) & ~mask) | (val & mask));
}

static const struct dpy_clk_desc *clk_desc(const struct dpy_clk *clk)
{
	return clk ? &dpy_clks[clk - dpy_clk_pool] : NULL;
}

static struct dpy_clk *clk_by_id(uint32_t id)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(dpy_clks); i++) {
		if (dpy_clks[i].id == id) {
			return &dpy_clk_pool[i];
		}
	}
	return NULL;
}

static uint32_t pll_peri_rate(void)
{
	uint32_t pll = ccu_rd(PLL_PERI_REG);
	uint32_t n = ((pll >> 8) & 0xff) + 1;
	uint32_t p0 = ((pll >> 16) & 0x7) + 1;
	uint32_t m = (pll & BIT(1)) ? 2 : 1;

	return DPY_HOSC_HZ / m / p0 * n;
}

static uint32_t pll_video0_rate(void)
{
	uint32_t reg = ccu_rd(PLL_VIDEO0_REG);
	uint32_t n = ((reg & PLL_VIDEO0_N_MASK) >> PLL_VIDEO0_N_SHIFT) + 1;

	return DPY_HOSC_HZ * n / ((reg & PLL_VIDEO0_INPUT_DIV2) ? 2 : 1);
}

static uint32_t clk_parent_rate(const struct dpy_clk_desc *d, uint8_t mux)
{
	uint32_t pid = mux < MAX_PARENTS ? d->parents[mux] : 0;

	if (d->id == CLK_TCONLCD && mux == 0) {
		return pll_video0_rate() / 4;	/* PLL_VIDEO0_1X */
	}
	switch (pid) {
	case CLK_PLL_PERI_2X:
		return pll_peri_rate();
	case CLK_PLL_PERI_1X:
		return pll_peri_rate() / 2;
	case CLK_PLL_VIDEO0_4X:
		return pll_video0_rate();
	default:
		return d->id == CLK_DSI && mux == 0 ? DPY_HOSC_HZ : 0;
	}
}

struct dpy_clk *dpy_os_clk_get(uint32_t controller, uint32_t id)
{
	if (controller != ALLWINNER_CCU_MAIN) {
		return NULL;
	}
	return clk_by_id(id);
}

void dpy_os_clk_put(struct dpy_clk *clk)
{
	ARG_UNUSED(clk);
}

static void pll_video0_enable(void)
{
	uint32_t reg = ccu_rd(PLL_VIDEO0_REG);
	int tries = 10000;

	if ((reg & PLL_VIDEO0_EN) && (reg & PLL_VIDEO0_OUT)) {
		return;
	}
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_LDO, PLL_VIDEO0_LDO);
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_EN, PLL_VIDEO0_EN);
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_LOCK_EN, PLL_VIDEO0_LOCK_EN);
	while (tries-- && !(ccu_rd(PLL_VIDEO0_REG) & PLL_VIDEO0_LOCKED)) {
		k_busy_wait(10);
	}
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_OUT, PLL_VIDEO0_OUT);
}

static void pll_video0_disable(void)
{
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_OUT, 0);
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_EN, 0);
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_LDO, 0);
}

int dpy_os_clk_enable(struct dpy_clk *clk)
{
	const struct dpy_clk_desc *d = clk_desc(clk);

	if (!d) {
		return 0;
	}
	switch (d->kind) {
	case CLK_KIND_PLL_VIDEO0_4X:
		pll_video0_enable();
		break;
	case CLK_KIND_MUXDIV:
	case CLK_KIND_MP:
		{
			uint32_t mux = (ccu_rd(d->reg) >> 24) & 0x7;

			/* make sure the selected PLL runs before ungating */
			if (mux < MAX_PARENTS &&
			    (d->parents[mux] == CLK_PLL_VIDEO0_4X ||
			     (d->id == CLK_TCONLCD && mux == 0))) {
				pll_video0_enable();
			}
		}
		ccu_upd(d->reg, BIT(31), BIT(31));
		break;
	case CLK_KIND_BUS:
		ccu_upd(d->reg, BIT(0), BIT(0));
		break;
	default:
		break;
	}
	return 0;
}

void dpy_os_clk_disable(struct dpy_clk *clk)
{
	const struct dpy_clk_desc *d = clk_desc(clk);

	if (!d) {
		return;
	}
	switch (d->kind) {
	case CLK_KIND_MUXDIV:
	case CLK_KIND_MP:
		ccu_upd(d->reg, BIT(31), 0);
		break;
	case CLK_KIND_BUS:
		ccu_upd(d->reg, BIT(0), 0);
		break;
	default:
		/* the PLLs are shared, leave them running */
		break;
	}
}

int dpy_os_clk_set_parent(struct dpy_clk *clk, struct dpy_clk *parent)
{
	const struct dpy_clk_desc *d = clk_desc(clk);
	const struct dpy_clk_desc *p = clk_desc(parent);
	uint32_t mux;

	if (!d || !p) {
		return 0;
	}
	if (d->kind != CLK_KIND_MUXDIV && d->kind != CLK_KIND_MP) {
		return -EINVAL;
	}
	for (mux = 0; mux < MAX_PARENTS; mux++) {
		if (d->parents[mux] == p->id) {
			ccu_upd(d->reg, 0x7U << 24, mux << 24);
			return 0;
		}
	}
	return -EINVAL;
}

/* choose N so that 24 MHz * N is closest to @hz within the PLL range */
static uint32_t pll_video0_pick(uint32_t hz)
{
	uint32_t n;

	hz = CLAMP(hz, PLL_VIDEO0_MIN_HZ, PLL_VIDEO0_MAX_HZ);
	n = DIV_ROUND_CLOSEST(hz, DPY_HOSC_HZ);
	return CLAMP(n, 1U, 256U);
}

static int pll_video0_set_rate(uint32_t hz)
{
	uint32_t n = pll_video0_pick(hz);
	bool was_on = ccu_rd(PLL_VIDEO0_REG) & PLL_VIDEO0_EN;

	if (was_on) {
		pll_video0_disable();
	}
	/*
	 * The input divider must be cleared so that the PLL
	 * runs at 24 MHz * N; left set it halves the output and the pixel
	 * clock, and with it the refresh rate.
	 */
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_INPUT_DIV2, 0);
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_N_MASK,
		(n - 1) << PLL_VIDEO0_N_SHIFT);
	if (was_on) {
		pll_video0_enable();
	}
	return 0;
}

int dpy_os_clk_set_rate(struct dpy_clk *clk, uint32_t hz)
{
	const struct dpy_clk_desc *d = clk_desc(clk);
	uint32_t mux, parent, div, max_div, shift;

	if (!d) {
		return -ENODEV;
	}
	if (d->kind == CLK_KIND_PLL_VIDEO0_4X) {
		return pll_video0_set_rate(hz);
	}
	if (d->kind != CLK_KIND_MUXDIV && d->kind != CLK_KIND_MP) {
		return -ENOTSUP;
	}

	mux = (ccu_rd(d->reg) >> 24) & 0x7;
	if (d->kind == CLK_KIND_MP &&
	    d->parents[mux < MAX_PARENTS ? mux : 0] == CLK_PLL_VIDEO0_4X) {
		/* the pixel clock PLL is dedicated to us: retune it, M = P = 0 */
		int ret = pll_video0_set_rate(hz);

		ccu_upd(d->reg, 0xf | (0x3U << 8), 0);
		return ret;
	}

	parent = clk_parent_rate(d, mux);
	if (!parent || !hz) {
		return -EINVAL;
	}
	if (d->kind == CLK_KIND_MP) {
		/* shared parent: pick the M divider only */
		div = CLAMP(DIV_ROUND_CLOSEST(parent, hz), 1U, 16U);
		ccu_upd(d->reg, 0xf | (0x3U << 8), div - 1);
		return 0;
	}
	max_div = BIT(d->div_width);
	shift = 0;
	div = CLAMP(DIV_ROUND_CLOSEST(parent, hz), 1U, max_div);
	ccu_upd(d->reg, (max_div - 1) << shift, (div - 1) << shift);
	return 0;
}

uint32_t dpy_os_clk_get_rate(struct dpy_clk *clk)
{
	const struct dpy_clk_desc *d = clk_desc(clk);
	uint32_t reg, mux, rate;

	if (!d) {
		return 0;
	}
	switch (d->kind) {
	case CLK_KIND_PLL_PERI_2X:
		return pll_peri_rate();
	case CLK_KIND_PLL_PERI_1X:
		return pll_peri_rate() / 2;
	case CLK_KIND_PLL_VIDEO0_4X:
		return pll_video0_rate();
	case CLK_KIND_BUS:
		return DPY_HOSC_HZ;
	default:
		break;
	}
	reg = ccu_rd(d->reg);
	mux = (reg >> 24) & 0x7;
	rate = clk_parent_rate(d, mux);
	if (d->kind == CLK_KIND_MP) {
		return rate / ((reg & 0xf) + 1) / BIT((reg >> 8) & 0x3);
	}
	return rate / ((reg & (BIT(d->div_width) - 1)) + 1);
}

uint32_t dpy_os_clk_round_rate(struct dpy_clk *clk, uint32_t hz)
{
	const struct dpy_clk_desc *d = clk_desc(clk);

	if (!d) {
		return 0;
	}
	if (d->kind == CLK_KIND_PLL_VIDEO0_4X ||
	    (d->kind == CLK_KIND_MP &&
	     d->parents[((ccu_rd(d->reg) >> 24) & 0x7) < MAX_PARENTS ?
			((ccu_rd(d->reg) >> 24) & 0x7) : 0] ==
		     CLK_PLL_VIDEO0_4X)) {
		return pll_video0_pick(hz) * DPY_HOSC_HZ;
	}
	return 0;	/* unknown: callers use the request as is */
}

struct dpy_reset *dpy_os_reset_get(uint32_t controller, uint32_t id)
{
	size_t i;

	if (controller != ALLWINNER_CCU_MAIN) {
		return NULL;
	}
	for (i = 0; i < ARRAY_SIZE(dpy_resets); i++) {
		if (dpy_resets[i].id == id) {
			dpy_reset_pool[i].id = id;
			return &dpy_reset_pool[i];
		}
	}
	return NULL;
}

void dpy_os_reset_put(struct dpy_reset *rst)
{
	ARG_UNUSED(rst);
}

static uint32_t reset_reg(const struct dpy_reset *rst)
{
	return dpy_resets[rst - dpy_reset_pool].reg;
}

int dpy_os_reset_assert(struct dpy_reset *rst)
{
	if (rst) {
		ccu_upd(reset_reg(rst), BIT(16), 0);
	}
	return 0;
}

int dpy_os_reset_deassert(struct dpy_reset *rst)
{
	if (rst) {
		ccu_upd(reset_reg(rst), BIT(16), BIT(16));
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Pins, GPIO, PWM, regulators                                          */
/* ------------------------------------------------------------------ */
#define PIO_HW		(&sunxi_pio_hw_type0_info)

static uintptr_t pio_bank(uint32_t pin)
{
	return DPY_PIO_BASE + (pin / SUNXI_PIO_PINS_PER_BANK) * PIO_HW->bank_mem_size;
}

/* update a @bits wide field of pin @pin inside a packed register block */
static void pio_field(uint32_t pin, uint32_t blk_off, uint32_t per_reg,
		      uint32_t bits, uint32_t val)
{
	uint32_t num = pin % SUNXI_PIO_PINS_PER_BANK;
	uintptr_t reg = pio_bank(pin) + blk_off + (num / per_reg) * 4;
	uint32_t shift = (num % per_reg) * bits;
	uint32_t mask = (BIT(bits) - 1) << shift;
	k_spinlock_key_t key;
	static struct k_spinlock lock;

	key = k_spin_lock(&lock);
	sys_write32((sys_read32(reg) & ~mask) | ((val << shift) & mask), reg);
	k_spin_unlock(&lock, key);
}

int dpy_os_pin_set_function(uint32_t pin, uint32_t function)
{
	pio_field(pin, PIO_HW->mux_regs_offset, PIO_HW->mux_pins_per_reg,
		  PIO_HW->mux_pins_bits, function);
	return 0;
}

int dpy_os_pin_set_drive(uint32_t pin, uint32_t level)
{
	pio_field(pin, PIO_HW->drv_regs_offset, PIO_HW->drv_pins_per_reg,
		  PIO_HW->drv_pins_bits, level);
	return 0;
}

int dpy_os_pin_set_pull(uint32_t pin, uint32_t pull)
{
	pio_field(pin, PIO_HW->pull_regs_offset, PIO_HW->pull_pins_per_reg,
		  PIO_HW->pull_pins_bits, pull);
	return 0;
}

int dpy_os_gpio_set_value(uint32_t pin, int value)
{
	pio_field(pin, PIO_HW->data_regs_offset, PIO_HW->data_pins_per_reg,
		  PIO_HW->data_pins_bits, value ? 1 : 0);
	return 0;
}

int dpy_os_gpio_direction_output(uint32_t pin, int value)
{
	/* latch the level first so the pin does not glitch when it turns output */
	dpy_os_gpio_set_value(pin, value);
	return dpy_os_pin_set_function(pin, 1);	/* muxsel 1 = output */
}

int dpy_os_gpio_direction_input(uint32_t pin)
{
	return dpy_os_pin_set_function(pin, 0);	/* muxsel 0 = input */
}

int dpy_os_gpio_get_value(uint32_t pin)
{
	uint32_t num = pin % SUNXI_PIO_PINS_PER_BANK;

	return (sys_read32(pio_bank(pin) + PIO_HW->data_regs_offset) >> num) & 1;
}

int dpy_os_pwm_apply(const void *ctrl, uint32_t channel,
		     uint32_t period_ns, uint32_t duty_ns, bool inverted,
		     bool enable)
{
	const struct device *pwm = ctrl;
	uint64_t rate;
	uint32_t period, pulse;
	int ret;

	/* a PWM block marked zephyr,deferred-init is brought up with the backlight that uses it */
	if (!device_is_ready(pwm) && device_init(pwm) != 0) {
		return -ENODEV;
	}
	ret = pwm_get_cycles_per_sec(pwm, channel, &rate);
	if (ret) {
		return ret;
	}
	period = (uint32_t)(rate * period_ns / NSEC_PER_SEC);
	pulse = enable ? (uint32_t)(rate * duty_ns / NSEC_PER_SEC) : 0;
	/*
	 * Off keeps the channel running at duty 0: releasing it would mux
	 * the pin back to an input and leave the backlight to the board pulls.
	 */
	return pwm_set_cycles(pwm, channel, period, pulse,
			      inverted ? PWM_POLARITY_INVERTED : PWM_POLARITY_NORMAL);
}

int dpy_os_regulator_set(uint32_t id, uint32_t microvolt, bool enable)
{
	ARG_UNUSED(id);
	ARG_UNUSED(microvolt);
	ARG_UNUSED(enable);
	return -ENOTSUP;
}

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
#ifdef CONFIG_DISPLAY_SHELL_CMDS
#include <zephyr/shell/shell.h>

/* shell commands run one at a time in the shell thread */
static const struct shell *dpy_out_sh;

void dpy_os_shell_bind(const struct shell *sh)
{
	dpy_out_sh = sh;
}
#endif

void dpy_os_vprintf(const char *fmt, va_list ap)
{
#ifdef CONFIG_DISPLAY_SHELL_CMDS
	if (dpy_out_sh) {
		shell_vfprintf(dpy_out_sh, SHELL_NORMAL, fmt, ap);
		return;
	}
#endif
	vprintk(fmt, ap);
}

void dpy_os_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	dpy_os_vprintf(fmt, ap);
	va_end(ap);
}
