// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * PWM dimmed backlight with an optional enable GPIO.
 *
 * Without a PWM driver in the system (dpy_os_pwm_apply() returns
 * -ENOTSUP) only the enable GPIO is driven and the brightness level is
 * remembered for when dimming becomes available.
 */
#define DPY_LOG_TAG "bl-pwm"
#include <dpy/dpy_log.h>
#include <dpy/dpy_panel.h>
#include <dpy/dpy_pdata.h>


struct bl_pwm {
	struct dpy_backlight bl;
	const struct dpy_backlight_pwm_pdata *pd;
	bool no_pwm;
};

static uint32_t bl_pwm_duty(const struct bl_pwm *b, uint32_t level)
{
	const struct dpy_backlight_pwm_pdata *pd = b->pd;
	uint32_t max = b->bl.max_level;

	if (!level)
		return 0;
	/* map 1..max onto min_level..max */
	if (pd->min_level && pd->min_level < max)
		level = pd->min_level + (level - 1) * (max - pd->min_level) /
			(max - 1 ? max - 1 : 1);
	return (uint32_t)((uint64_t)pd->period_ns * level / max);
}

static int bl_pwm_update(struct dpy_backlight *bl, uint32_t level, bool on)
{
	struct bl_pwm *b = bl->priv;
	const struct dpy_backlight_pwm_pdata *pd = b->pd;
	bool lit = on && level;
	int ret = 0;

	if (!lit)
		dpy_gpio_set(&pd->enable, false);

	if (!b->no_pwm && pd->controller) {
		ret = dpy_os_pwm_apply(pd->controller, pd->channel,
				       pd->period_ns, bl_pwm_duty(b, level),
				       pd->inverted, lit);
		if (ret == -ENOTSUP) {
			dpy_warn("no PWM driver, backlight is on/off only\n");
			b->no_pwm = true;
			ret = 0;
		}
	}

	if (lit)
		dpy_gpio_set(&pd->enable, true);
	return ret;
}

static const struct dpy_backlight_ops bl_pwm_ops = {
	.update = bl_pwm_update,
};

static int bl_pwm_bind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct bl_pwm *b = dev->priv;

	(void)ddev;
	dpy_pins_apply(&b->pd->pins);
	return dpy_backlight_register(&b->bl);
}

static void bl_pwm_unbind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct bl_pwm *b = dev->priv;

	(void)ddev;
	dpy_backlight_disable(&b->bl);
	dpy_backlight_unregister(&b->bl);
}

static const struct dpy_component_ops bl_pwm_component_ops = {
	.bind = bl_pwm_bind,
	.unbind = bl_pwm_unbind,
};

static int bl_pwm_probe(struct dpy_dev *dev)
{
	const struct dpy_backlight_pwm_pdata *pd = dpy_dev_pdata(dev);
	struct bl_pwm *b;

	if (!pd || !pd->period_ns)
		return -EINVAL;
	b = dpy_os_zalloc(sizeof(*b));
	if (!b)
		return -ENOMEM;
	b->pd = pd;
	b->bl.node = dev->node;
	b->bl.ops = &bl_pwm_ops;
	b->bl.priv = b;
	b->bl.max_level = pd->max_level ? pd->max_level : DISPLAY_BACKLIGHT_MAX;
	b->bl.level = pd->default_level ? pd->default_level : b->bl.max_level;
	dev->priv = b;
	return dpy_component_add(dev);
}

static void bl_pwm_remove(struct dpy_dev *dev)
{
	dpy_component_del(dev);
	dpy_os_free(dev->priv);
	dev->priv = NULL;
}

static const struct dpy_match bl_pwm_match[] = {
	{ "pwm-backlight", NULL },
	{ NULL },
};

const struct dpy_driver dpy_backlight_pwm_driver = {
	.name = "pwm-backlight",
	.match = bl_pwm_match,
	.klass = DPY_COMP_BACKLIGHT,
	.probe = bl_pwm_probe,
	.remove = bl_pwm_remove,
	.ops = &bl_pwm_component_ops,
};
