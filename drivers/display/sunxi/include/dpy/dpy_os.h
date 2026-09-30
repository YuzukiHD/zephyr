/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display pipeline framework - operating system abstraction.
 *
 * This is the only interface between the display stack and the host OS.
 * A port (osal/dpy_os_<os>.c) implements every function declared here;
 * nothing else in the stack calls into the RTOS directly.
 *
 * Porting to another RTOS (Zephyr, RT-Thread, ...) therefore means writing
 * one new osal file plus the build glue, see docs/display/porting.md.
 */
#ifndef __DPY_OS_H__
#define __DPY_OS_H__

#include <stdarg.h>
#include <dpy/dpy_types.h>

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */
void *dpy_os_zalloc(size_t size);
void dpy_os_free(void *ptr);

/*
 * Memory the display hardware reads through DMA (register command queue,
 * shadow register blocks). The memory may be cached; callers use
 * dpy_os_dcache_clean() before handing it to hardware.
 */
void *dpy_os_dma_alloc(size_t size, size_t align, dpy_dma_addr_t *dma);
void dpy_os_dma_free(void *ptr);
dpy_dma_addr_t dpy_os_virt_to_dma(const void *ptr);

void dpy_os_dcache_clean(const void *ptr, size_t size);
void dpy_os_dcache_invalidate(void *ptr, size_t size);

/* ------------------------------------------------------------------ */
/* Register access                                                     */
/* ------------------------------------------------------------------ */
/* Map a physical register window; returns the CPU address to use. */
uintptr_t dpy_os_ioremap(uintptr_t phys, size_t size);

#ifdef DPY_OS_MMIO_ACCESSORS
/*
 * The port provides the accessors, e.g. with explicit barriers, tracing,
 * or a register model (tools/host simulates the hardware this way).
 */
uint32_t dpy_readl(uintptr_t addr);
void dpy_writel(uint32_t val, uintptr_t addr);
#else
static inline uint32_t dpy_readl(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void dpy_writel(uint32_t val, uintptr_t addr)
{
	*(volatile uint32_t *)addr = val;
}
#endif

static inline void dpy_updatel(uintptr_t addr, uint32_t mask, uint32_t val)
{
	dpy_writel((dpy_readl(addr) & ~mask) | (val & mask), addr);
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */
void dpy_os_udelay(uint32_t us);
void dpy_os_msleep(uint32_t ms);
uint64_t dpy_os_time_us(void);

/* ------------------------------------------------------------------ */
/* Synchronisation                                                     */
/* ------------------------------------------------------------------ */
struct dpy_mutex;
struct dpy_sem;

struct dpy_mutex *dpy_os_mutex_create(void);
void dpy_os_mutex_destroy(struct dpy_mutex *m);
void dpy_os_mutex_lock(struct dpy_mutex *m);
void dpy_os_mutex_unlock(struct dpy_mutex *m);

/* counting semaphore; dpy_os_sem_post() must be callable from IRQ context */
struct dpy_sem *dpy_os_sem_create(uint32_t initial);
void dpy_os_sem_destroy(struct dpy_sem *s);
void dpy_os_sem_post(struct dpy_sem *s);
/*
 * returns 0 on success, -ETIMEDOUT on timeout; a timeout of 0 polls
 * without blocking, DPY_WAIT_FOREVER blocks
 */
int dpy_os_sem_wait(struct dpy_sem *s, uint32_t timeout_ms);
#define DPY_WAIT_FOREVER	0xffffffffU

/*
 * Interrupt-safe critical section protecting data shared with IRQ
 * handlers. Must not sleep while held.
 */
typedef struct {
	/* storage for the port's native lock object */
	unsigned long storage[4];
} dpy_spinlock_t;

void dpy_os_spin_init(dpy_spinlock_t *lock);
unsigned long dpy_os_spin_lock_irqsave(dpy_spinlock_t *lock);
void dpy_os_spin_unlock_irqrestore(dpy_spinlock_t *lock, unsigned long flags);

bool dpy_os_in_irq(void);

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */
typedef void (*dpy_irq_handler_t)(void *data);

int dpy_os_request_irq(int irq, dpy_irq_handler_t handler, const char *name,
		       void *data);
void dpy_os_free_irq(int irq, void *data);

/* ------------------------------------------------------------------ */
/* Clocks and resets                                                   */
/* ------------------------------------------------------------------ */
struct dpy_clk;
struct dpy_reset;

/*
 * Clock/reset identifiers are opaque SoC-specific numbers carried by the
 * board/SoC description (see struct dpy_res). The port translates them.
 */
struct dpy_clk *dpy_os_clk_get(uint32_t controller, uint32_t id);
void dpy_os_clk_put(struct dpy_clk *clk);
int dpy_os_clk_enable(struct dpy_clk *clk);
void dpy_os_clk_disable(struct dpy_clk *clk);
int dpy_os_clk_set_parent(struct dpy_clk *clk, struct dpy_clk *parent);
int dpy_os_clk_set_rate(struct dpy_clk *clk, uint32_t hz);
uint32_t dpy_os_clk_get_rate(struct dpy_clk *clk);
/* rate the clock would actually run at for @hz, 0 if unknown */
uint32_t dpy_os_clk_round_rate(struct dpy_clk *clk, uint32_t hz);

struct dpy_reset *dpy_os_reset_get(uint32_t controller, uint32_t id);
void dpy_os_reset_put(struct dpy_reset *rst);
int dpy_os_reset_assert(struct dpy_reset *rst);
int dpy_os_reset_deassert(struct dpy_reset *rst);

/* ------------------------------------------------------------------ */
/* Pins, GPIO, PWM, regulators                                          */
/* ------------------------------------------------------------------ */
/* @pin is a SoC global pin number (bank * 32 + index on sunxi) */
int dpy_os_pin_set_function(uint32_t pin, uint32_t function);
int dpy_os_pin_set_drive(uint32_t pin, uint32_t level);
int dpy_os_pin_set_pull(uint32_t pin, uint32_t pull);
int dpy_os_gpio_direction_output(uint32_t pin, int value);
int dpy_os_gpio_direction_input(uint32_t pin);
int dpy_os_gpio_set_value(uint32_t pin, int value);
int dpy_os_gpio_get_value(uint32_t pin);

/*
 * @ctrl is the PWM controller of the port (a struct device on Zephyr).
 * enable = false drives the inactive level (duty 0), the pin stays a PWM
 * output. Returns -ENOTSUP when no PWM driver is available.
 */
int dpy_os_pwm_apply(const void *ctrl, uint32_t channel,
		     uint32_t period_ns, uint32_t duty_ns, bool inverted,
		     bool enable);

/* returns -ENOTSUP when no regulator framework is available */
int dpy_os_regulator_set(uint32_t id, uint32_t microvolt, bool enable);

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
void dpy_os_vprintf(const char *fmt, va_list ap);
void dpy_os_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* __DPY_OS_H__ */
