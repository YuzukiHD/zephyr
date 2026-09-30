// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - pin control and GPIO helpers for board data.
 */
#define DPY_LOG_TAG "pins"
#include <dpy/dpy_log.h>
#include <dpy/dpy_pdata.h>

void dpy_pins_apply(const struct dpy_pin_group *group)
{
	unsigned int i;

	for (i = 0; group && i < group->count; i++) {
		const struct dpy_pin *p = &group->pins[i];

		if (dpy_os_pin_set_function(p->pin, p->function))
			dpy_warn("pin %u: cannot select function %u\n", p->pin,
				 p->function);
		if (p->drive != DPY_PIN_DEFAULT)
			dpy_os_pin_set_drive(p->pin, p->drive);
		if (p->pull != DPY_PIN_DEFAULT)
			dpy_os_pin_set_pull(p->pin, p->pull);
	}
}

void dpy_pins_release(const struct dpy_pin_group *group)
{
	unsigned int i;

	/* park the pads as inputs so the panel sees no stray levels */
	for (i = 0; group && i < group->count; i++)
		dpy_os_pin_set_function(group->pins[i].pin, DPY_PIN_FUNC_INPUT);
}

int dpy_gpio_set(const struct dpy_gpio *gpio, bool on)
{
	int level;

	if (!gpio || !(gpio->flags & DPY_GPIO_VALID))
		return 0;
	level = on ? 1 : 0;
	if (gpio->flags & DPY_GPIO_ACTIVE_LOW)
		level = !level;
	return dpy_os_gpio_direction_output(gpio->pin, level);
}
