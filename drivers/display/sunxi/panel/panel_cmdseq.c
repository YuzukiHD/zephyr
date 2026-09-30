// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Panel command sequence interpreter.
 *
 * Board descriptions express power-up, reset and register initialisation
 * of a panel as a table of struct dpy_cmd; this file executes them.
 */
#define DPY_LOG_TAG "cmdseq"
#include <dpy/dpy_log.h>

#include "panel_cmdseq.h"

/* ------------------------------------------------------------------ */
/* GPIO bit-banged SPI                                                 */
/* ------------------------------------------------------------------ */
#ifdef CONFIG_DISPLAY_PANEL_SPI_INIT
static void spi_level(const struct dpy_gpio *g, int level)
{
	if (g->flags & DPY_GPIO_VALID)
		dpy_os_gpio_set_value(g->pin, level);
}

void dpy_spi_gpio_setup(const struct dpy_spi_gpio *spi)
{
	if (!spi || spi->mode == DPY_SPI_NONE)
		return;
	/* idle: CS high, clock high, data high */
	if (spi->cs.flags & DPY_GPIO_VALID)
		dpy_os_gpio_direction_output(spi->cs.pin, 1);
	if (spi->sck.flags & DPY_GPIO_VALID)
		dpy_os_gpio_direction_output(spi->sck.pin, 1);
	if (spi->sda.flags & DPY_GPIO_VALID)
		dpy_os_gpio_direction_output(spi->sda.pin, 1);
	if (spi->dc.flags & DPY_GPIO_VALID)
		dpy_os_gpio_direction_output(spi->dc.pin, 1);
}

static void spi_bits(const struct dpy_spi_gpio *spi, uint32_t word,
		     unsigned int bits)
{
	uint32_t hp = spi->half_period_us ? spi->half_period_us : 1;

	while (bits--) {
		spi_level(&spi->sck, 0);
		spi_level(&spi->sda, (word >> bits) & 1);
		dpy_os_udelay(hp);
		/* the panel samples on the rising edge */
		spi_level(&spi->sck, 1);
		dpy_os_udelay(hp);
	}
}

static void spi_byte(const struct dpy_spi_gpio *spi, uint8_t byte, bool data)
{
	if (spi->mode == DPY_SPI_3WIRE_9BIT) {
		spi_bits(spi, ((uint32_t)data << 8) | byte, 9);
	} else {
		spi_level(&spi->dc, data);
		spi_bits(spi, byte, 8);
	}
}

static int spi_send(const struct dpy_spi_gpio *spi, const struct dpy_cmd *c)
{
	uint8_t i;

	if (!spi || spi->mode == DPY_SPI_NONE)
		return -ENODEV;
	spi_level(&spi->cs, 0);
	for (i = 0; i < c->len; i++)
		spi_byte(spi, c->data[i],
			 c->type == DPY_CMD_SPI_DATA || i > 0);
	spi_level(&spi->cs, 1);
	return 0;
}
#else
void dpy_spi_gpio_setup(const struct dpy_spi_gpio *spi)
{
}

static int spi_send(const struct dpy_spi_gpio *spi, const struct dpy_cmd *c)
{
	dpy_err("SPI init commands need CONFIG_DISPLAY_PANEL_SPI_INIT\n");
	return -ENOTSUP;
}
#endif

/* ------------------------------------------------------------------ */
/* Sequence                                                            */
/* ------------------------------------------------------------------ */
int dpy_cmdseq_run(const struct dpy_cmd_seq *seq, const struct dpy_cmd_ctx *ctx)
{
	unsigned int i;
	int ret = 0;

	for (i = 0; seq && i < seq->count; i++) {
		const struct dpy_cmd *c = &seq->cmds[i];

		switch (c->type) {
		case DPY_CMD_END:
			return 0;
		case DPY_CMD_DELAY:
			dpy_os_msleep(c->arg);
			continue;
		case DPY_CMD_GPIO:
			/* a missing pin should not stop the rest of the sequence */
			ret = dpy_os_gpio_direction_output(c->arg, c->len);
			if (ret)
				dpy_warn("command %u: gpio %u: %d\n", i, c->arg,
					 ret);
			ret = 0;
			continue;
		case DPY_CMD_REGULATOR: {
			uint32_t uv = 0;

			if (c->data)
				memcpy(&uv, c->data, sizeof(uv));
			ret = dpy_os_regulator_set(c->arg, uv, c->len != 0);
			if (ret == -ENOTSUP)
				ret = 0;
			continue;
		}
		case DPY_CMD_DCS:
			if (!ctx || !ctx->dsi || !c->len)
				return -ENODEV;
			ret = dpy_dsi_dcs_write(ctx->dsi, c->data[0],
						c->data + 1, c->len - 1u);
			break;
		case DPY_CMD_GENERIC:
			if (!ctx || !ctx->dsi)
				return -ENODEV;
			ret = dpy_dsi_generic_write(ctx->dsi, c->data, c->len);
			break;
		case DPY_CMD_SPI_CMD:
		case DPY_CMD_SPI_DATA:
			ret = spi_send(ctx ? ctx->spi : NULL, c);
			break;
		default:
			return -EINVAL;
		}
		if (ret) {
			dpy_err("command %u failed: %d\n", i, ret);
			return ret;
		}
		/* bus commands carry their post delay in arg */
		if (c->arg)
			dpy_os_msleep(c->arg);
	}
	return ret;
}
