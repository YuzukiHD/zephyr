// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * On/off backlight switched by a GPIO.
 */
#define DPY_LOG_TAG "bl-gpio"
#include <dpy/dpy_log.h>
#include <dpy/dpy_panel.h>
#include <dpy/dpy_pdata.h>

struct bl_gpio {
	struct dpy_backlight bl;
	const struct dpy_backlight_gpio_pdata *pd;
};

static int bl_gpio_update(struct dpy_backlight *bl, uint32_t level, bool on)
{
	struct bl_gpio *b = bl->priv;
	int value = on && level;

	if (b->pd->enable.flags & DPY_GPIO_ACTIVE_LOW)
		value = !value;
	return dpy_os_gpio_direction_output(b->pd->enable.pin, value);
}

static const struct dpy_backlight_ops bl_gpio_ops = {
	.update = bl_gpio_update,
};

static int bl_gpio_bind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct bl_gpio *b = dev->priv;

	(void)ddev;
	return dpy_backlight_register(&b->bl);
}

static void bl_gpio_unbind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct bl_gpio *b = dev->priv;

	(void)ddev;
	dpy_backlight_disable(&b->bl);
	dpy_backlight_unregister(&b->bl);
}

static const struct dpy_component_ops bl_gpio_component_ops = {
	.bind = bl_gpio_bind,
	.unbind = bl_gpio_unbind,
};

static int bl_gpio_probe(struct dpy_dev *dev)
{
	const struct dpy_backlight_gpio_pdata *pd = dpy_dev_pdata(dev);
	struct bl_gpio *b;

	if (!pd || !(pd->enable.flags & DPY_GPIO_VALID))
		return -EINVAL;
	b = dpy_os_zalloc(sizeof(*b));
	if (!b)
		return -ENOMEM;
	b->pd = pd;
	b->bl.node = dev->node;
	b->bl.ops = &bl_gpio_ops;
	b->bl.priv = b;
	b->bl.max_level = 1;
	b->bl.level = 1;
	dev->priv = b;
	return dpy_component_add(dev);
}

static void bl_gpio_remove(struct dpy_dev *dev)
{
	dpy_component_del(dev);
	dpy_os_free(dev->priv);
	dev->priv = NULL;
}

static const struct dpy_match bl_gpio_match[] = {
	{ "gpio-backlight", NULL },
	{ NULL },
};

const struct dpy_driver dpy_backlight_gpio_driver = {
	.name = "gpio-backlight",
	.match = bl_gpio_match,
	.klass = DPY_COMP_BACKLIGHT,
	.probe = bl_gpio_probe,
	.remove = bl_gpio_remove,
	.ops = &bl_gpio_component_ops,
};
