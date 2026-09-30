// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - driver table.
 *
 * The RTOS has no initcalls, so the available drivers are listed here,
 * selected by Kconfig. Graph nodes are matched against this table by their
 * compatible string.
 */
#include <dpy/dpy_device.h>

extern const struct dpy_driver dpy_de_driver;
extern const struct dpy_driver dpy_tcon_top_driver;
extern const struct dpy_driver dpy_tcon_lcd_driver;
extern const struct dpy_driver dpy_combo_dphy_driver;
extern const struct dpy_driver dpy_rgb_driver;
extern const struct dpy_driver dpy_lvds_driver;
extern const struct dpy_driver dpy_dsi_driver;
extern const struct dpy_driver dpy_panel_simple_driver;
extern const struct dpy_driver dpy_panel_dsi_driver;
extern const struct dpy_driver dpy_backlight_gpio_driver;
extern const struct dpy_driver dpy_backlight_pwm_driver;

const struct dpy_driver *const dpy_driver_table[] = {
#ifdef CONFIG_DISPLAY_DE
	&dpy_de_driver,
#endif
#ifdef CONFIG_DISPLAY_TCON_LCD
	&dpy_tcon_top_driver,
	&dpy_tcon_lcd_driver,
#endif
#ifdef CONFIG_DISPLAY_COMBO_DPHY
	&dpy_combo_dphy_driver,
#endif
#ifdef CONFIG_DISPLAY_ENCODER_RGB
	&dpy_rgb_driver,
#endif
#ifdef CONFIG_DISPLAY_ENCODER_LVDS
	&dpy_lvds_driver,
#endif
#ifdef CONFIG_DISPLAY_ENCODER_DSI
	&dpy_dsi_driver,
#endif
#ifdef CONFIG_DISPLAY_PANEL_SIMPLE
	&dpy_panel_simple_driver,
#endif
#ifdef CONFIG_DISPLAY_PANEL_DSI
	&dpy_panel_dsi_driver,
#endif
#ifdef CONFIG_DISPLAY_BACKLIGHT_GPIO
	&dpy_backlight_gpio_driver,
#endif
#ifdef CONFIG_DISPLAY_BACKLIGHT_PWM
	&dpy_backlight_pwm_driver,
#endif
	NULL,
};

const unsigned int dpy_driver_table_size = DPY_ARRAY_SIZE(dpy_driver_table) - 1;
