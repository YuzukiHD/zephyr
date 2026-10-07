/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT goodix_gt967

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_touch.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gt967, CONFIG_INPUT_LOG_LEVEL);

/* 16 bit register addresses, sent big endian */
#define REG_PRODUCT_ID	0x8140U
#define REG_STATUS	0x814EU
#define REG_CONFIG	0x8047U

/* the first three ASCII bytes of the product id, "967" */
#define PRODUCT_ID_967	0x00373639U

/* configuration block: 0x8047..0x8128, checksum at 0x8129 and the "updated" flag at 0x812A */
#define CONFIG_SIZE		228U
#define CONFIG_X_MAX		1U	/* 16 bit little endian, offsets from REG_CONFIG */
#define CONFIG_Y_MAX		3U
#define CONFIG_TOUCH_NUMBER	5U	/* low 4 bits */
#define CONFIG_MODULE_SWITCH1	6U
#define MODULE_SWITCH1_INT_MASK	0x03U	/* 0: rising edge */

#define STATUS_READY		BIT(7)
#define STATUS_POINTS_MASK	0x0FU

#define MAX_POINTS		5U
#define POINT_SIZE		8U

#define RESET_HOLD_MS		12
#define ADDRESS_SETUP_MS	2
#define ADDRESS_HOLD_MS		6
#define INT_SYNC_MS		50
#define BOOT_MS			60

struct gt967_config {
	struct input_touchscreen_common_config common;
	struct i2c_dt_spec bus;
	struct gpio_dt_spec irq_gpio;
	struct gpio_dt_spec reset_gpio;
};

struct gt967_data {
	const struct device *dev;
	struct k_work work;
#ifdef CONFIG_INPUT_GT967_INTERRUPT
	struct gpio_callback irq_cb;
#else
	struct k_timer timer;
#endif
	/* touch ids that were down in the previous frame, and where */
	uint8_t down;
	uint16_t last_x[MAX_POINTS];
	uint16_t last_y[MAX_POINTS];
};

static int gt967_read(const struct device *dev, uint16_t reg, void *buf, size_t len)
{
	const struct gt967_config *cfg = dev->config;
	uint8_t addr[2];

	sys_put_be16(reg, addr);
	return i2c_write_read_dt(&cfg->bus, addr, sizeof(addr), buf, len);
}

static int gt967_write(const struct device *dev, uint16_t reg, const void *buf, size_t len)
{
	const struct gt967_config *cfg = dev->config;
	uint8_t msg[2 + CONFIG_SIZE + 1];

	if (len > sizeof(msg) - 2U) {
		return -EINVAL;
	}
	sys_put_be16(reg, msg);
	memcpy(&msg[2], buf, len);
	return i2c_write_dt(&cfg->bus, msg, 2U + len);
}

static void gt967_report(const struct device *dev, uint8_t id, uint16_t x, uint16_t y, bool down)
{
	if (CONFIG_INPUT_GT967_MAX_TOUCH_POINTS > 1) {
		input_report_abs(dev, INPUT_ABS_MT_SLOT, id, true, K_FOREVER);
	}
	input_touchscreen_report_pos(dev, x, y, K_FOREVER);
	input_report_key(dev, INPUT_BTN_TOUCH, down ? 1 : 0, true, K_FOREVER);
}

static void gt967_work_handler(struct k_work *work)
{
	struct gt967_data *data = CONTAINER_OF(work, struct gt967_data, work);
	const struct device *dev = data->dev;
	uint8_t buf[1 + MAX_POINTS * POINT_SIZE];
	uint8_t clear[1] = {0};
	uint8_t points, now = 0;
	int ret;

	ret = gt967_read(dev, REG_STATUS, buf, 1);
	if (ret < 0) {
		LOG_ERR("read failed: %d", ret);
		return;
	}
	if (!(buf[0] & STATUS_READY)) {
		return;
	}
	points = MIN(buf[0] & STATUS_POINTS_MASK, CONFIG_INPUT_GT967_MAX_TOUCH_POINTS);

	/* only the points that are down, a slow bus time is spent on every byte */
	if (points > 0) {
		ret = gt967_read(dev, REG_STATUS + 1, &buf[1], points * POINT_SIZE);
		if (ret < 0) {
			LOG_ERR("read failed: %d", ret);
			return;
		}
	}

	/* the controller keeps the frame until the status is cleared */
	ret = gt967_write(dev, REG_STATUS, clear, sizeof(clear));
	if (ret < 0) {
		LOG_ERR("clearing the status failed: %d", ret);
		return;
	}

	for (uint8_t i = 0; i < points; i++) {
		const uint8_t *p = &buf[1 + i * POINT_SIZE];
		uint8_t id = p[0] % MAX_POINTS;
		uint16_t x = sys_get_le16(&p[1]);
		uint16_t y = sys_get_le16(&p[3]);

		now |= BIT(id);
		/* the controller repeats a resting finger in every frame, report what changed */
		if ((data->down & BIT(id)) && data->last_x[id] == x && data->last_y[id] == y) {
			continue;
		}
		data->last_x[id] = x;
		data->last_y[id] = y;
		LOG_DBG("down id %u at %u,%u", id, x, y);
		gt967_report(dev, id, x, y, true);
	}

	for (uint8_t id = 0; id < MAX_POINTS; id++) {
		if ((data->down & BIT(id)) && !(now & BIT(id))) {
			LOG_DBG("up id %u", id);
			gt967_report(dev, id, data->last_x[id], data->last_y[id], false);
		}
	}
	data->down = now;
}

#ifdef CONFIG_INPUT_GT967_INTERRUPT
static void gt967_irq_handler(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct gt967_data *data = CONTAINER_OF(cb, struct gt967_data, irq_cb);

	k_work_submit(&data->work);
}
#else
static void gt967_timer_handler(struct k_timer *timer)
{
	struct gt967_data *data = CONTAINER_OF(timer, struct gt967_data, timer);

	k_work_submit(&data->work);
}
#endif

/*
 * Reset with the INT line at the level that selects the I2C address, then drive INT low for a
 * while so that the controller starts out with the interrupt line in a known state.
 */
static int gt967_reset(const struct device *dev)
{
	const struct gt967_config *cfg = dev->config;
	int high = cfg->bus.addr == 0x14U;
	int ret;

	ret = gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return ret;
	}
	k_msleep(RESET_HOLD_MS);
	ret = gpio_pin_configure_dt(&cfg->irq_gpio, high ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}
	k_msleep(ADDRESS_SETUP_MS);
	gpio_pin_set_dt(&cfg->reset_gpio, 0);
	k_msleep(ADDRESS_HOLD_MS);

	/* INT low for 50 ms, then released to the controller */
	gpio_pin_configure_dt(&cfg->irq_gpio, GPIO_OUTPUT_INACTIVE);
	k_msleep(INT_SYNC_MS);
	return gpio_pin_configure_dt(&cfg->irq_gpio, GPIO_INPUT);
}

static uint8_t gt967_checksum(const uint8_t *config)
{
	uint8_t sum = 0;

	for (unsigned int i = 0; i < CONFIG_SIZE - 2U; i++) {
		sum += config[i];
	}
	return (uint8_t)(~sum + 1U);
}

/*
 * Bring the resolution, the number of touch points and the interrupt edge in line with the
 * devicetree and Kconfig; a controller that already has them keeps its configuration untouched.
 */
static int gt967_configure(const struct device *dev)
{
	const struct gt967_config *cfg = dev->config;
	uint8_t config[CONFIG_SIZE];
	uint8_t want[CONFIG_SIZE];
	int ret;

	ret = gt967_read(dev, REG_CONFIG, config, CONFIG_SIZE);
	if (ret < 0) {
		return ret;
	}
	if (config[0] == 0U || gt967_checksum(config) != config[CONFIG_SIZE - 2U]) {
		LOG_ERR("the configuration of the controller is not valid");
		return -ENODEV;
	}

	LOG_INF("configuration 0x%02x: %u x %u, up to %u point(s), module switch 0x%02x", config[0],
		sys_get_le16(&config[CONFIG_X_MAX]), sys_get_le16(&config[CONFIG_Y_MAX]),
		config[CONFIG_TOUCH_NUMBER] & 0x0fU, config[CONFIG_MODULE_SWITCH1]);
	memcpy(want, config, sizeof(want));
	if (cfg->common.screen_width != 0U && cfg->common.screen_height != 0U) {
		sys_put_le16(cfg->common.screen_width, &want[CONFIG_X_MAX]);
		sys_put_le16(cfg->common.screen_height, &want[CONFIG_Y_MAX]);
	}
	want[CONFIG_TOUCH_NUMBER] = (want[CONFIG_TOUCH_NUMBER] & 0xf0U) |
				    CONFIG_INPUT_GT967_MAX_TOUCH_POINTS;
	want[CONFIG_MODULE_SWITCH1] &= ~MODULE_SWITCH1_INT_MASK;

	if (memcmp(config, want, CONFIG_SIZE - 2U) == 0) {
		return 0;
	}
	want[CONFIG_SIZE - 2U] = gt967_checksum(want);
	want[CONFIG_SIZE - 1U] = 1U;	/* configuration updated */
	ret = gt967_write(dev, REG_CONFIG, want, CONFIG_SIZE);
	if (ret < 0) {
		return ret;
	}
	LOG_INF("configuration updated: %ux%u, %u point(s)", sys_get_le16(&want[CONFIG_X_MAX]),
		sys_get_le16(&want[CONFIG_Y_MAX]), want[CONFIG_TOUCH_NUMBER] & 0x0fU);
	k_msleep(BOOT_MS);
	return 0;
}

static int gt967_init(const struct device *dev)
{
	const struct gt967_config *cfg = dev->config;
	struct gt967_data *data = dev->data;
	uint8_t id[4], info[6];
	int ret;

	data->dev = dev;
	k_work_init(&data->work, gt967_work_handler);

	if (!i2c_is_ready_dt(&cfg->bus) || !gpio_is_ready_dt(&cfg->irq_gpio) ||
	    !gpio_is_ready_dt(&cfg->reset_gpio)) {
		LOG_ERR("bus or gpio not ready");
		return -ENODEV;
	}

	ret = gt967_reset(dev);
	if (ret < 0) {
		return ret;
	}

	ret = gt967_read(dev, REG_PRODUCT_ID, id, sizeof(id));
	if (ret < 0) {
		LOG_ERR("no answer at address 0x%02x: %d", cfg->bus.addr, ret);
		return ret;
	}
	if ((sys_get_le32(id) & 0x00FFFFFFU) != PRODUCT_ID_967) {
		LOG_ERR("unexpected product id %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);
		return -ENODEV;
	}
	ret = gt967_read(dev, REG_PRODUCT_ID + 4U, info, sizeof(info));
	if (ret == 0) {
		LOG_INF("GT%c%c%c firmware 0x%04x, %ux%u", id[0], id[1], id[2],
			sys_get_le16(&info[0]), sys_get_le16(&info[2]), sys_get_le16(&info[4]));
	}

	ret = gt967_configure(dev);
	if (ret < 0) {
		return ret;
	}

#ifdef CONFIG_INPUT_GT967_INTERRUPT
	gpio_init_callback(&data->irq_cb, gt967_irq_handler, BIT(cfg->irq_gpio.pin));
	ret = gpio_add_callback(cfg->irq_gpio.port, &data->irq_cb);
	if (ret < 0) {
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&cfg->irq_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		return ret;
	}
#else
	k_timer_init(&data->timer, gt967_timer_handler, NULL);
	k_timer_start(&data->timer, K_MSEC(CONFIG_INPUT_GT967_PERIOD_MS),
		      K_MSEC(CONFIG_INPUT_GT967_PERIOD_MS));
#endif

	/* drop a frame that is already waiting */
	k_work_submit(&data->work);
	return 0;
}

#define GT967_INIT(inst)							\
	static const struct gt967_config gt967_config_##inst = {		\
		.common = INPUT_TOUCH_DT_INST_COMMON_CONFIG_INIT(inst),		\
		.bus = I2C_DT_SPEC_INST_GET(inst),				\
		.irq_gpio = GPIO_DT_SPEC_INST_GET(inst, irq_gpios),		\
		.reset_gpio = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),		\
	};									\
	static struct gt967_data gt967_data_##inst;				\
	INPUT_TOUCH_STRUCT_CHECK(struct gt967_config);				\
	DEVICE_DT_INST_DEFINE(inst, gt967_init, NULL, &gt967_data_##inst,	\
			      &gt967_config_##inst, POST_KERNEL,		\
			      CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(GT967_INIT)
