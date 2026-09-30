// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline description generated from the devicetree.
 *
 * Every enabled node of the display pipeline (display engine, TCON top and
 * LCD, RGB/LVDS/DSI encoders, combo D-PHY, panels, backlights) becomes a
 * struct dpy_node of the graph the display core consumes:
 *
 *   reg / interrupts / clocks / resets  ->  struct dpy_res
 *   top / phy / backlight phandles      ->  struct dpy_ref
 *   ports/port@N/endpoint@M             ->  struct dpy_link (source side)
 *   the remaining properties            ->  the pdata of the matched driver
 *
 * Command sequences (panel power/init) are cell arrays in the devicetree;
 * dpy_dt_prepare() decodes them into struct dpy_cmd lists at boot.
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/dt-bindings/clock/sun252i-f101-ccu.h>
#include <zephyr/dt-bindings/pinctrl/allwinner-pinctrl.h>
#include <zephyr/sys/util.h>

#include <dpy/dpy_graph.h>
#include <dpy/dpy_os.h>
#include <dpy/dpy_pdata.h>

#include "dt_graph.h"

#define DPY_SYM(prefix, n)	UTIL_CAT(prefix, DT_DEP_ORD(n))

/* ------------------------------------------------------------------ */
/* Resources and references                                            */
/* ------------------------------------------------------------------ */
#define RES_REG(n) \
	COND_CODE_1(DT_NODE_HAS_PROP(n, reg), \
		    (DPY_RES_MMIO("reg", DT_REG_ADDR(n), DT_REG_SIZE(n)),), ())

#define RES_IRQ(n) \
	COND_CODE_1(DT_NODE_HAS_PROP(n, interrupts), \
		    (DPY_RES_IRQ("irq", DT_IRQN(n)),), ())

#define CLK_PARENT(n, idx) \
	COND_CODE_1(DT_NODE_HAS_PROP(n, clock_parents), \
		    (DT_PROP_BY_IDX(n, clock_parents, idx)), (DPY_CLK_NO_PARENT))

#define CLK_RATE(n, idx) \
	COND_CODE_1(DT_NODE_HAS_PROP(n, clock_rates), \
		    (DT_PROP_BY_IDX(n, clock_rates, idx)), (0))

#define RES_CLK_ONE(n, prop, idx) \
	DPY_RES_CLK(DT_PROP_BY_IDX(n, clock_names, idx), ALLWINNER_CCU_MAIN, \
		    DT_CLOCKS_CELL_BY_IDX(n, idx, clkid), CLK_PARENT(n, idx), \
		    CLK_RATE(n, idx)),

#define RES_CLKS(n) \
	COND_CODE_1(DT_NODE_HAS_PROP(n, clocks), \
		    (DT_FOREACH_PROP_ELEM(n, clock_names, RES_CLK_ONE)), ())

#define RES_RST_ONE(n, prop, idx) \
	DPY_RES_RST(DT_PROP_BY_IDX(n, reset_names, idx), ALLWINNER_CCU_MAIN, \
		    DT_PHA_BY_IDX(n, resets, idx, id)),

#define RES_RSTS(n) \
	COND_CODE_1(DT_NODE_HAS_PROP(n, resets), \
		    (DT_FOREACH_PROP_ELEM(n, reset_names, RES_RST_ONE)), ())

#define HAS_RES(n) \
	UTIL_OR(UTIL_OR(DT_NODE_HAS_PROP(n, reg), DT_NODE_HAS_PROP(n, interrupts)), \
		UTIL_OR(DT_NODE_HAS_PROP(n, clocks), DT_NODE_HAS_PROP(n, resets)))

#define REF(n, prop) \
	COND_CODE_1(DT_NODE_HAS_PROP(n, prop), \
		    ({ #prop, DT_NODE_FULL_NAME(DT_PHANDLE(n, prop)) },), ())

#define HAS_REF(n) \
	UTIL_OR(DT_NODE_HAS_PROP(n, top), \
		UTIL_OR(DT_NODE_HAS_PROP(n, phy), DT_NODE_HAS_PROP(n, backlight)))

#define DEFINE_COMMON(n) \
	COND_CODE_1(HAS_RES(n), \
		    (static const struct dpy_res DPY_SYM(dt_res_, n)[] = \
			{ RES_REG(n) RES_IRQ(n) RES_CLKS(n) RES_RSTS(n) };), ()) \
	COND_CODE_1(HAS_REF(n), \
		    (static const struct dpy_ref DPY_SYM(dt_ref_, n)[] = \
			{ REF(n, top) REF(n, phy) REF(n, backlight) };), ())

/* struct dpy_node initialiser; @compat is a DPY_COMPATIBLE() list */
#define NODE(n, pd_, ...) \
	{ \
		.name = DT_NODE_FULL_NAME(n), \
		.compatible = DPY_COMPATIBLE(__VA_ARGS__), \
		.status = DPY_STATUS_OKAY, \
		.res = COND_CODE_1(HAS_RES(n), (DPY_SYM(dt_res_, n)), (NULL)), \
		.nres = COND_CODE_1(HAS_RES(n), (ARRAY_SIZE(DPY_SYM(dt_res_, n))), (0)), \
		.refs = COND_CODE_1(HAS_REF(n), (DPY_SYM(dt_ref_, n)), (NULL)), \
		.nrefs = COND_CODE_1(HAS_REF(n), (ARRAY_SIZE(DPY_SYM(dt_ref_, n))), (0)), \
		.pdata = pd_, \
	},

/* ------------------------------------------------------------------ */
/* Pins and GPIOs                                                      */
/* ------------------------------------------------------------------ */
#define PIN_ELEM(grp, prop, idx) \
	{ \
		.pin = ALLWINNER_PINMUX_PIN(DT_PROP_BY_IDX(grp, prop, idx)), \
		.function = ALLWINNER_PINMUX_MUXSEL(DT_PROP_BY_IDX(grp, prop, idx)), \
		.drive = DT_PROP_OR(grp, allwinner_drive_level, DPY_PIN_DEFAULT), \
		.pull = 0, \
	},

#define PIN_GROUP(grp) DT_FOREACH_PROP_ELEM(grp, pinmux, PIN_ELEM)

#define DEFINE_PINS(n) \
	COND_CODE_1(DT_NODE_HAS_PROP(n, pinctrl_0), \
		    (static const struct dpy_pin DPY_SYM(dt_pins_, n)[] = \
			{ DT_FOREACH_CHILD(DT_PHANDLE_BY_IDX(n, pinctrl_0, 0), PIN_GROUP) };), ())

#define PINS(n) \
	COND_CODE_1(DT_NODE_HAS_PROP(n, pinctrl_0), \
		    ({ .pins = DPY_SYM(dt_pins_, n), \
		       .count = ARRAY_SIZE(DPY_SYM(dt_pins_, n)) }), ({ 0 }))

/* sunxi global pin number of a GPIO cell: bank * 32 + pin */
#define GPIO_PIN(n, prop) \
	(((DT_REG_ADDR(DT_GPIO_CTLR_BY_IDX(n, prop, 0)) - \
	   DT_REG_ADDR(DT_NODELABEL(pio))) / 0x30) * 32 + DT_GPIO_PIN_BY_IDX(n, prop, 0))

#define DPY_GPIO_OF(n, prop) \
	COND_CODE_1(DT_NODE_HAS_PROP(n, prop), \
		    ({ .pin = GPIO_PIN(n, prop), \
		       .flags = DPY_GPIO_VALID | \
				((DT_GPIO_FLAGS_BY_IDX(n, prop, 0) & GPIO_ACTIVE_LOW) ? \
				 DPY_GPIO_ACTIVE_LOW : 0) }), \
		    (DPY_GPIO_NONE))

/* ------------------------------------------------------------------ */
/* Command sequences                                                   */
/* ------------------------------------------------------------------ */
struct dt_seq_src {
	const uint32_t *cells;
	uint16_t len;
	struct dpy_cmd_seq *dst;
};

#define SEQ_DEFINE(n, prop, field) \
	COND_CODE_1(DT_NODE_HAS_PROP(n, prop), \
		    (static const uint32_t UTIL_CAT(UTIL_CAT(dt_seq_, field), \
						    DT_DEP_ORD(n))[] = DT_PROP(n, prop);), ())

#define SEQ_SRC(n, prop, field) \
	COND_CODE_1(DT_NODE_HAS_PROP(n, prop), \
		    ({ .cells = UTIL_CAT(UTIL_CAT(dt_seq_, field), DT_DEP_ORD(n)), \
		       .len = ARRAY_SIZE(UTIL_CAT(UTIL_CAT(dt_seq_, field), DT_DEP_ORD(n))), \
		       .dst = &DPY_SYM(dt_pd_, n).field },), ())

#define SEQ_DEFINE_ALL(n) \
	SEQ_DEFINE(n, power_on_sequence, power_on) \
	SEQ_DEFINE(n, power_off_sequence, power_off) \
	SEQ_DEFINE(n, init_sequence, init) \
	SEQ_DEFINE(n, exit_sequence, exit)

#define SEQ_SRC_ALL(n) \
	SEQ_SRC(n, power_on_sequence, power_on) \
	SEQ_SRC(n, power_off_sequence, power_off) \
	SEQ_SRC(n, init_sequence, init) \
	SEQ_SRC(n, exit_sequence, exit)

/* ------------------------------------------------------------------ */
/* Panels                                                              */
/* ------------------------------------------------------------------ */
#define TIMING(n) DT_CHILD(n, display_timings)

#define MODE_ELEM(t, panel) \
	COND_CODE_1(DT_NODE_HAS_COMPAT(t, zephyr_panel_timing), \
		    ({ \
			.clock = DT_PROP(t, clock_frequency) / 1000, \
			.hdisplay = DT_PROP(panel, width), \
			.hsync_start = DT_PROP(panel, width) + DT_PROP(t, hfront_porch), \
			.hsync_end = DT_PROP(panel, width) + DT_PROP(t, hfront_porch) + \
				     DT_PROP(t, hsync_len), \
			.htotal = DT_PROP(panel, width) + DT_PROP(t, hfront_porch) + \
				  DT_PROP(t, hsync_len) + DT_PROP(t, hback_porch), \
			.vdisplay = DT_PROP(panel, height), \
			.vsync_start = DT_PROP(panel, height) + DT_PROP(t, vfront_porch), \
			.vsync_end = DT_PROP(panel, height) + DT_PROP(t, vfront_porch) + \
				     DT_PROP(t, vsync_len), \
			.vtotal = DT_PROP(panel, height) + DT_PROP(t, vfront_porch) + \
				  DT_PROP(t, vsync_len) + DT_PROP(t, vback_porch), \
			.flags = (DT_PROP(t, hsync_active) ? 0 : DISPLAY_MODE_FLAG_NHSYNC) | \
				 (DT_PROP(t, vsync_active) ? 0 : DISPLAY_MODE_FLAG_NVSYNC) | \
				 DISPLAY_MODE_FLAG_PREFERRED, \
		    },), ())

#define DEFINE_MODES(n) \
	static const struct dpy_display_mode DPY_SYM(dt_modes_, n)[] = \
		{ DT_FOREACH_CHILD_VARGS(n, MODE_ELEM, n) };

#define BUS_FLAGS(n) \
	((DT_PROP(TIMING(n), de_active) ? 0 : DPY_BUS_FLAG_DE_LOW) | \
	 (DT_PROP(TIMING(n), pixelclk_active) ? 0 : DPY_BUS_FLAG_PIXDATA_NEGEDGE))

#define PANEL_COMMON(n) \
	.name = DT_NODE_FULL_NAME(n), \
	.modes = DPY_SYM(dt_modes_, n), \
	.num_modes = ARRAY_SIZE(DPY_SYM(dt_modes_, n)), \
	.width_mm = DT_PROP_OR(n, width_mm, 0), \
	.height_mm = DT_PROP_OR(n, height_mm, 0), \
	.enable_delay_ms = DT_PROP(n, enable_delay_ms), \
	.disable_delay_ms = DT_PROP(n, disable_delay_ms)

#define DEFINE_PANEL_SIMPLE(n) \
	DEFINE_COMMON(n) DEFINE_MODES(n) SEQ_DEFINE_ALL(n) \
	static struct dpy_panel_simple_pdata DPY_SYM(dt_pd_, n) = { \
		PANEL_COMMON(n), \
		.bus_format = DT_ENUM_IDX(n, bus_format) + 1, \
		.bus_flags = BUS_FLAGS(n), \
		.spi = { \
			.mode = DT_ENUM_IDX(n, spi_mode), \
			.cs = DPY_GPIO_OF(n, spi_cs_gpios), \
			.sck = DPY_GPIO_OF(n, spi_sck_gpios), \
			.sda = DPY_GPIO_OF(n, spi_sda_gpios), \
			.dc = DPY_GPIO_OF(n, spi_dc_gpios), \
			.half_period_us = DT_PROP(n, spi_half_period_us), \
		}, \
	};

#define DEFINE_PANEL_DSI(n) \
	DEFINE_COMMON(n) DEFINE_MODES(n) SEQ_DEFINE_ALL(n) \
	static struct dpy_panel_dsi_pdata DPY_SYM(dt_pd_, n) = { \
		PANEL_COMMON(n), \
		.lanes = DT_PROP_LEN(n, data_lanes), \
		.format = DT_ENUM_IDX(n, mipi_dsi_format), \
		.mode_flags = DT_PROP(n, mode_flags), \
		.hs_trail = DT_PROP(n, hs_trail), \
		.clk_trail = DT_PROP(n, clk_trail), \
	};

/* ------------------------------------------------------------------ */
/* Backlights                                                          */
/* ------------------------------------------------------------------ */
#define DEFINE_BL_PWM(n) \
	DEFINE_COMMON(n) \
	static const struct dpy_backlight_pwm_pdata DPY_SYM(dt_pd_, n) = { \
		.controller = DEVICE_DT_GET(DT_PWMS_CTLR(n)), \
		.channel = DT_PWMS_CHANNEL(n), \
		.period_ns = DT_PWMS_PERIOD(n), \
		.inverted = !!(DT_PWMS_FLAGS(n) & PWM_POLARITY_INVERTED), \
		.enable = DPY_GPIO_OF(n, enable_gpios), \
		.max_level = DT_PROP(n, max_level), \
		.default_level = DT_PROP(n, default_level), \
		.min_level = DT_PROP(n, min_level), \
	};

#define DEFINE_BL_GPIO(n) \
	DEFINE_COMMON(n) \
	static const struct dpy_backlight_gpio_pdata DPY_SYM(dt_pd_, n) = { \
		.enable = DPY_GPIO_OF(n, enable_gpios), \
	};

/* ------------------------------------------------------------------ */
/* Engine and encoders                                                 */
/* ------------------------------------------------------------------ */
#define DEFINE_DE(n) \
	DEFINE_COMMON(n) \
	static const struct dpy_engine_pdata DPY_SYM(dt_pd_, n) = { \
		.adjust = { \
			.brightness = DT_PROP(n, brightness), \
			.contrast = DT_PROP(n, contrast), \
			.saturation = DT_PROP(n, saturation), \
			.hue = DT_PROP(n, hue), \
		}, \
		.background = DT_PROP(n, background_color), \
	};

#define DEFINE_RGB(n) \
	DEFINE_COMMON(n) DEFINE_PINS(n) \
	static const struct dpy_rgb_pdata DPY_SYM(dt_pd_, n) = { \
		.pins = PINS(n), \
		.hv_mode = DT_PROP(n, hv_mode), \
		.srgb_seq = DT_PROP(n, serial_rgb_sequence), \
		.syuv_seq = DT_PROP(n, serial_yuv_sequence), \
		.syuv_fdly = DT_PROP(n, serial_yuv_first_delay), \
		.rgb_swap = DT_PROP(n, rgb_swap), \
		.rb_swap = DT_PROP(n, rb_swap), \
		.clk_phase = DT_PROP(n, clk_phase), \
		.use_tcon_frm = DT_PROP(n, use_tcon_frm), \
		.io_adjust = DT_PROP(n, io_adjust), \
	};

#define DEFINE_LVDS(n) \
	DEFINE_COMMON(n) DEFINE_PINS(n) \
	static const struct dpy_lvds_pdata DPY_SYM(dt_pd_, n) = { \
		.pins = PINS(n), \
		.dual_link = DT_PROP(n, dual_link), \
		.use_tcon_frm = DT_PROP(n, use_tcon_frm), \
	};

#define DEFINE_DSI(n) \
	DEFINE_COMMON(n) DEFINE_PINS(n) \
	static const struct dpy_dsi_pdata DPY_SYM(dt_pd_, n) = { \
		.pins = PINS(n), \
	};

#define DEFINE_PLAIN(n) DEFINE_COMMON(n)

/* ------------------------------------------------------------------ */
/* Static definitions, one pass per node class                         */
/* ------------------------------------------------------------------ */
DT_FOREACH_STATUS_OKAY(allwinner_sun252iw2_display_engine, DEFINE_DE)
DT_FOREACH_STATUS_OKAY(allwinner_sun252iw2_tcon_top, DEFINE_PLAIN)
DT_FOREACH_STATUS_OKAY(allwinner_sun252iw2_tcon_lcd, DEFINE_PLAIN)
DT_FOREACH_STATUS_OKAY(allwinner_sunxi_rgb, DEFINE_RGB)
DT_FOREACH_STATUS_OKAY(allwinner_sunxi_lvds, DEFINE_LVDS)
DT_FOREACH_STATUS_OKAY(allwinner_sun252iw2_mipi_dsi, DEFINE_DSI)
DT_FOREACH_STATUS_OKAY(allwinner_sun252iw2_combo_dphy, DEFINE_PLAIN)
DT_FOREACH_STATUS_OKAY(panel_simple, DEFINE_PANEL_SIMPLE)
DT_FOREACH_STATUS_OKAY(panel_dsi, DEFINE_PANEL_DSI)
DT_FOREACH_STATUS_OKAY(pwm_backlight, DEFINE_BL_PWM)
DT_FOREACH_STATUS_OKAY(gpio_backlight, DEFINE_BL_GPIO)

/* ------------------------------------------------------------------ */
/* Node table                                                          */
/* ------------------------------------------------------------------ */
#define NODE_DE(n) \
	NODE(n, &DPY_SYM(dt_pd_, n), "allwinner,sun252iw2-display-engine")
#define NODE_TCON_TOP(n) \
	NODE(n, NULL, "allwinner,sun252iw2-tcon-top", "allwinner,sunxi-tcon-top")
#define NODE_TCON_LCD(n) \
	NODE(n, NULL, "allwinner,sun252iw2-tcon-lcd", "allwinner,sunxi-tcon-lcd")
#define NODE_RGB(n)	NODE(n, &DPY_SYM(dt_pd_, n), "allwinner,sunxi-rgb")
#define NODE_LVDS(n)	NODE(n, &DPY_SYM(dt_pd_, n), "allwinner,sunxi-lvds")
#define NODE_DSI(n) \
	NODE(n, &DPY_SYM(dt_pd_, n), "allwinner,sun252iw2-mipi-dsi", "allwinner,sunxi-mipi-dsi")
#define NODE_DPHY(n) \
	NODE(n, &dpy_sun252iw2_dphy_pdata, "allwinner,sun252iw2-combo-dphy", \
	     "allwinner,sunxi-combo-dphy")
#define NODE_PANEL_SIMPLE(n)	NODE(n, &DPY_SYM(dt_pd_, n), "panel-simple")
#define NODE_PANEL_DSI(n)	NODE(n, &DPY_SYM(dt_pd_, n), "panel-dsi")
#define NODE_BL_PWM(n)		NODE(n, &DPY_SYM(dt_pd_, n), "pwm-backlight")
#define NODE_BL_GPIO(n)		NODE(n, &DPY_SYM(dt_pd_, n), "gpio-backlight")

static const struct dpy_node dt_nodes[] = {
	DT_FOREACH_STATUS_OKAY(allwinner_sun252iw2_display_engine, NODE_DE)
	DT_FOREACH_STATUS_OKAY(allwinner_sun252iw2_tcon_top, NODE_TCON_TOP)
	DT_FOREACH_STATUS_OKAY(allwinner_sun252iw2_tcon_lcd, NODE_TCON_LCD)
	DT_FOREACH_STATUS_OKAY(allwinner_sunxi_rgb, NODE_RGB)
	DT_FOREACH_STATUS_OKAY(allwinner_sunxi_lvds, NODE_LVDS)
	DT_FOREACH_STATUS_OKAY(allwinner_sun252iw2_mipi_dsi, NODE_DSI)
	DT_FOREACH_STATUS_OKAY(allwinner_sun252iw2_combo_dphy, NODE_DPHY)
	DT_FOREACH_STATUS_OKAY(panel_simple, NODE_PANEL_SIMPLE)
	DT_FOREACH_STATUS_OKAY(panel_dsi, NODE_PANEL_DSI)
	DT_FOREACH_STATUS_OKAY(pwm_backlight, NODE_BL_PWM)
	DT_FOREACH_STATUS_OKAY(gpio_backlight, NODE_BL_GPIO)
};

/* ------------------------------------------------------------------ */
/* Links: ports/port@N/endpoint@M { remote-endpoint = <&sink>; }       */
/* ------------------------------------------------------------------ */
#define LINK_EP(ep, port, owner) \
	COND_CODE_1(DT_NODE_HAS_STATUS_OKAY(DT_PHANDLE(ep, remote_endpoint)), \
		    ({ \
			DT_NODE_FULL_NAME(owner), DT_REG_ADDR(port), DT_REG_ADDR(ep), \
			DT_NODE_FULL_NAME(DT_PHANDLE(ep, remote_endpoint)), \
			DT_PROP(ep, remote_port), DT_PROP(ep, remote_ep), \
		    },), ())

#define LINK_PORT(port, owner) \
	DT_FOREACH_CHILD_STATUS_OKAY_VARGS(port, LINK_EP, port, owner)

#define NODE_LINKS(n) \
	COND_CODE_1(DT_NODE_EXISTS(DT_CHILD(n, ports)), \
		    (DT_FOREACH_CHILD_VARGS(DT_CHILD(n, ports), LINK_PORT, n)), ())

static const struct dpy_link dt_links[] = {
	DT_FOREACH_STATUS_OKAY(allwinner_sun252iw2_display_engine, NODE_LINKS)
	DT_FOREACH_STATUS_OKAY(allwinner_sun252iw2_tcon_lcd, NODE_LINKS)
	DT_FOREACH_STATUS_OKAY(allwinner_sunxi_rgb, NODE_LINKS)
	DT_FOREACH_STATUS_OKAY(allwinner_sunxi_lvds, NODE_LINKS)
	DT_FOREACH_STATUS_OKAY(allwinner_sun252iw2_mipi_dsi, NODE_LINKS)
};

const struct dpy_board_desc dpy_board = {
	.name = "devicetree",
	.nodes = dt_nodes,
	.nnodes = ARRAY_SIZE(dt_nodes),
	.links = dt_links,
	.nlinks = ARRAY_SIZE(dt_links),
};

/* ------------------------------------------------------------------ */
/* Command sequence decoding                                           */
/* ------------------------------------------------------------------ */
static const struct dt_seq_src dt_seq_srcs[] = {
	DT_FOREACH_STATUS_OKAY(panel_simple, SEQ_SRC_ALL)
	DT_FOREACH_STATUS_OKAY(panel_dsi, SEQ_SRC_ALL)
	{ NULL, 0, NULL },
};

static size_t seq_data_len(uint32_t type, uint32_t len)
{
	switch (type) {
	case DPY_CMD_DCS:
	case DPY_CMD_GENERIC:
	case DPY_CMD_SPI_CMD:
	case DPY_CMD_SPI_DATA:
		return len;
	default:
		return 0;
	}
}

static int seq_decode(const struct dt_seq_src *src)
{
	const uint32_t *c = src->cells;
	size_t n = src->len, i, count = 0, bytes = 0, k = 0;
	struct dpy_cmd *cmds;
	uint8_t *data;

	for (i = 0; i + 3 <= n; count++) {
		size_t dl = seq_data_len(c[i], c[i + 1]);

		if (i + 3 + dl > n) {
			return -EINVAL;
		}
		bytes += dl;
		i += 3 + dl;
	}
	if (i != n) {
		return -EINVAL;
	}

	cmds = dpy_os_zalloc(count * sizeof(*cmds));
	data = dpy_os_zalloc(bytes ? bytes : 1);
	if (!cmds || !data) {
		dpy_os_free(cmds);
		dpy_os_free(data);
		return -ENOMEM;
	}

	for (i = 0, count = 0; i < n; count++) {
		size_t dl = seq_data_len(c[i], c[i + 1]), j;

		cmds[count].type = c[i];
		cmds[count].len = c[i + 1];
		cmds[count].arg = c[i + 2];
		if (dl) {
			cmds[count].data = &data[k];
			for (j = 0; j < dl; j++) {
				data[k++] = c[i + 3 + j];
			}
		}
		i += 3 + dl;
	}

	src->dst->cmds = cmds;
	src->dst->count = count;
	return 0;
}

int dpy_dt_prepare(void)
{
	size_t i;
	int ret;

	for (i = 0; dt_seq_srcs[i].cells; i++) {
		ret = seq_decode(&dt_seq_srcs[i]);
		if (ret) {
			return ret;
		}
	}
	return 0;
}
