/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Interrupt registration for the archive: one line, the video engine's */

#include <stdint.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include "glue.h"

LOG_MODULE_DECLARE(vdec_sunxi_glue, CONFIG_VDEC_SUNXI_LOG_LEVEL);

#define VE_IRQN		DT_IRQN(VE_NODE)
#define VE_IRQ_PRIO	DT_IRQ(VE_NODE, priority)

typedef int (*glue_irq_handler_t)(void *data);

static glue_irq_handler_t ve_handler;
static void *ve_handler_data;
static bool ve_irq_connected;

static void ve_isr(const void *arg)
{
	ARG_UNUSED(arg);

	if (ve_handler != NULL) {
		(void)ve_handler(ve_handler_data);
	}
}

int32_t hal_request_irq(int32_t irq, glue_irq_handler_t handler, const char *name, void *data)
{
	ARG_UNUSED(name);

	/* The archive names the line by its own numbering; there is only one. */
	if (irq != 66) {
		LOG_WRN("unexpected video engine irq %d", irq);
	}
	if (!ve_irq_connected) {
		IRQ_CONNECT(VE_IRQN, VE_IRQ_PRIO, ve_isr, NULL, 0);
		ve_irq_connected = true;
	}
	ve_handler = handler;
	ve_handler_data = data;

	return 0;
}

void hal_free_irq(int32_t irq)
{
	ARG_UNUSED(irq);

	irq_disable(VE_IRQN);
	ve_handler = NULL;
	ve_handler_data = NULL;
}

int hal_enable_irq(int32_t irq)
{
	ARG_UNUSED(irq);

	irq_enable(VE_IRQN);
	return 0;
}

void hal_disable_irq(int32_t irq)
{
	ARG_UNUSED(irq);

	irq_disable(VE_IRQN);
}
