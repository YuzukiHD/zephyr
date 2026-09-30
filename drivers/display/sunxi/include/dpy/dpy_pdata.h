/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display pipeline framework - platform data of the generic drivers.
 *
 * Board descriptions fill these structures and attach them to graph
 * nodes. A devicetree parser produces the same data.
 */
#ifndef __DPY_PDATA_H__
#define __DPY_PDATA_H__

#include <dpy/dpy_kms.h>

/* ------------------------------------------------------------------ */
/* Pins and GPIOs                                                      */
/* ------------------------------------------------------------------ */
/* sunxi global pin number: bank (A=0) * 32 + index */
#define DPY_SUNXI_PIN(bank, n)	((uint16_t)(((bank) - 'A') * 32 + (n)))
#define DPY_PIN_DEFAULT		0xff

#define DPY_PIN_FUNC_INPUT	0
#define DPY_PIN_FUNC_OUTPUT	1

struct dpy_pin {
	uint16_t pin;
	uint8_t function;
	uint8_t drive;		/* DPY_PIN_DEFAULT: leave untouched */
	uint8_t pull;		/* DPY_PIN_DEFAULT: leave untouched */
};

#define DPY_PIN(_pin, _func) \
	{ .pin = _pin, .function = _func, .drive = DPY_PIN_DEFAULT, .pull = 0 }
#define DPY_PIN_DRV(_pin, _func, _drv) \
	{ .pin = _pin, .function = _func, .drive = _drv, .pull = 0 }

struct dpy_pin_group {
	const struct dpy_pin *pins;
	uint8_t count;
};

#define DPY_PIN_GROUP(arr) { .pins = arr, .count = DPY_ARRAY_SIZE(arr) }

#define DPY_GPIO_VALID		(1U << 0)
#define DPY_GPIO_ACTIVE_LOW	(1U << 1)

struct dpy_gpio {
	uint16_t pin;
	uint8_t flags;
};

#define DPY_GPIO_NONE		{ 0, 0 }
#define DPY_GPIO_HIGH(_pin)	{ _pin, DPY_GPIO_VALID }
#define DPY_GPIO_LOW(_pin)	{ _pin, DPY_GPIO_VALID | DPY_GPIO_ACTIVE_LOW }

void dpy_pins_apply(const struct dpy_pin_group *group);
void dpy_pins_release(const struct dpy_pin_group *group);
/* drive @gpio to its active (@on) or inactive level; no-op if unset */
int dpy_gpio_set(const struct dpy_gpio *gpio, bool on);

/* ------------------------------------------------------------------ */
/* Command sequences (power, reset, panel init)                        */
/* ------------------------------------------------------------------ */
enum dpy_cmd_type {
	DPY_CMD_END = 0,
	DPY_CMD_DELAY,		/* arg = ms */
	DPY_CMD_GPIO,		/* arg = pin, len = output level (physical) */
	DPY_CMD_REGULATOR,	/* arg = id, len = on/off, data = uV (u32) */
	DPY_CMD_DCS,		/* data[0] = cmd, then params; arg = delay ms */
	DPY_CMD_GENERIC,	/* generic DSI write; arg = delay ms */
	DPY_CMD_SPI_CMD,	/* SPI command byte(s); arg = delay ms */
	DPY_CMD_SPI_DATA,	/* SPI data byte(s); arg = delay ms */
};

struct dpy_cmd {
	uint8_t type;
	uint8_t len;
	uint16_t arg;
	const uint8_t *data;
};

#define DPY_CMD_DELAY_MS(ms)	{ DPY_CMD_DELAY, 0, ms, NULL }
#define DPY_CMD_SET_GPIO(pin, level) { DPY_CMD_GPIO, level, pin, NULL }
#define DPY_CMD_BYTES(t, ms, ...) \
	{ t, sizeof((const uint8_t[]){ __VA_ARGS__ }), ms, \
	  (const uint8_t[]){ __VA_ARGS__ } }
#define DPY_CMD_DCS_SEQ(ms, ...)	DPY_CMD_BYTES(DPY_CMD_DCS, ms, __VA_ARGS__)
#define DPY_CMD_GEN_SEQ(ms, ...)	DPY_CMD_BYTES(DPY_CMD_GENERIC, ms, __VA_ARGS__)
#define DPY_CMD_SPI_C(ms, ...)		DPY_CMD_BYTES(DPY_CMD_SPI_CMD, ms, __VA_ARGS__)
#define DPY_CMD_SPI_D(ms, ...)		DPY_CMD_BYTES(DPY_CMD_SPI_DATA, ms, __VA_ARGS__)

struct dpy_cmd_seq {
	const struct dpy_cmd *cmds;
	uint16_t count;
};

#define DPY_CMD_SEQ(arr) { .cmds = arr, .count = DPY_ARRAY_SIZE(arr) }

/* GPIO bit-banged SPI used by panel init sequences */
enum dpy_spi_mode {
	DPY_SPI_NONE = 0,
	DPY_SPI_3WIRE_9BIT,	/* D/C bit sent before each byte */
	DPY_SPI_4WIRE_8BIT,	/* separate D/C line */
};

struct dpy_spi_gpio {
	uint8_t mode;		/* enum dpy_spi_mode */
	struct dpy_gpio cs;
	struct dpy_gpio sck;
	struct dpy_gpio sda;
	struct dpy_gpio dc;
	uint16_t half_period_us;
};

/* ------------------------------------------------------------------ */
/* Display engine and phy                                              */
/* ------------------------------------------------------------------ */
/* board defaults applied to the CRTCs of a display engine */
struct dpy_engine_pdata {
	struct dpy_color_adjust adjust;
	uint32_t background;		/* 0xRRGGBB */
};

/* SoC hooks for the combo D-PHY (SoC description data) */
struct dpy_combo_dphy_soc_pdata {
	/* trim the termination resistors from fuse data, may be NULL */
	void (*calibrate)(void);
};

/* ------------------------------------------------------------------ */
/* Encoders                                                            */
/* ------------------------------------------------------------------ */
struct dpy_rgb_pdata {
	struct dpy_pin_group pins;
	uint8_t hv_mode;	/* enum dpy_hv_mode */
	uint8_t srgb_seq;
	uint8_t syuv_seq;
	uint8_t syuv_fdly;
	uint8_t rgb_swap;
	uint8_t rb_swap;
	uint8_t clk_phase;
	/* use the TCON FRM instead of the display engine dither */
	bool use_tcon_frm;
	uint32_t io_adjust;
};

struct dpy_lvds_pdata {
	struct dpy_pin_group pins;
	bool dual_link;
	bool use_tcon_frm;
};

struct dpy_dsi_pdata {
	struct dpy_pin_group pins;
};

/* ------------------------------------------------------------------ */
/* Panels                                                              */
/* ------------------------------------------------------------------ */
struct dpy_panel_simple_pdata {
	const char *name;
	const struct dpy_display_mode *modes;
	uint8_t num_modes;
	uint32_t width_mm;
	uint32_t height_mm;
	uint32_t bus_format;		/* enum dpy_bus_format */
	uint32_t bus_flags;		/* DPY_BUS_FLAG_* */
	struct dpy_cmd_seq power_on;	/* before the link starts */
	struct dpy_cmd_seq power_off;
	struct dpy_cmd_seq init;	/* register setup (SPI) */
	struct dpy_cmd_seq exit;
	struct dpy_spi_gpio spi;
	uint16_t enable_delay_ms;	/* video running -> display on */
	uint16_t disable_delay_ms;
};

struct dpy_panel_dsi_pdata {
	const char *name;
	const struct dpy_display_mode *modes;
	uint8_t num_modes;
	uint32_t width_mm;
	uint32_t height_mm;
	uint8_t lanes;
	uint8_t format;			/* enum dpy_dsi_format */
	uint32_t mode_flags;		/* DPY_DSI_MODE_* */
	uint8_t hs_trail;
	uint8_t clk_trail;
	struct dpy_cmd_seq power_on;
	struct dpy_cmd_seq power_off;
	struct dpy_cmd_seq init;	/* DCS/generic commands, link in LP */
	struct dpy_cmd_seq exit;
	uint16_t enable_delay_ms;
	uint16_t disable_delay_ms;
};

/* ------------------------------------------------------------------ */
/* Backlights                                                          */
/* ------------------------------------------------------------------ */
struct dpy_backlight_gpio_pdata {
	struct dpy_gpio enable;
};

struct dpy_backlight_pwm_pdata {
	const void *controller;	/* PWM controller handle for dpy_os_pwm_apply() */
	uint8_t channel;
	struct dpy_pin_group pins;
	uint32_t period_ns;
	bool inverted;
	struct dpy_gpio enable;
	uint32_t max_level;
	uint32_t default_level;
	/* optional minimum duty (level 1) in 1/max_level units */
	uint32_t min_level;
};

#endif /* __DPY_PDATA_H__ */
