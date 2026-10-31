// SPDX-License-Identifier: GPL-2.0-only
/* Metalio FT6336U: portrait touchscreen, confirm key and two Enter keys. */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "metalio-ft6336u-core.h"

struct metalio_ft6336u {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset;
	struct delayed_work sample;
	struct metalio_touch_state state;
	signed char slot_id[METALIO_TOUCH_CONTACTS];
	ktime_t last_read;
	bool have_last_read;
	bool running;
	bool suspended;
};

static int metalio_touch_read(struct metalio_ft6336u *touch, u8 reg,
			     u8 *buffer, unsigned int length)
{
	struct i2c_client *client = touch->client;
	struct i2c_msg messages[] = {
		{ .addr = client->addr, .len = 1, .buf = &reg },
		{ .addr = client->addr, .flags = I2C_M_RD,
		  .len = length, .buf = buffer },
	};
	int ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));

	return ret == ARRAY_SIZE(messages) ? 0 : ret < 0 ? ret : -EIO;
}

static int metalio_touch_reset(struct metalio_ft6336u *touch)
{
	u8 chip_high, chip_low, firmware;
	int ret;

	ret = gpiod_set_value_cansleep(touch->reset, 1);
	if (ret)
		return ret;
	msleep(10);
	ret = gpiod_set_value_cansleep(touch->reset, 0);
	if (ret)
		return ret;
	msleep(300);
	ret = metalio_touch_read(touch, 0xa3, &chip_high, 1);
	if (ret)
		return ret;
	ret = metalio_touch_read(touch, 0x9f, &chip_low, 1);
	if (ret)
		return ret;
	ret = metalio_touch_read(touch, 0xa6, &firmware, 1);
	if (ret)
		return ret;
	if (chip_high != 0x64 || chip_low != 0x26 || firmware == 0xff) {
		dev_err(&touch->client->dev, "unexpected touch ID %02x/%02x firmware %02x\n",
			chip_high, chip_low, firmware);
		return -ENODEV;
	}
	dev_info(&touch->client->dev, "FT6336U %02x/%02x firmware %02x, portrait + confirm/Enter keys\n",
		 chip_high, chip_low, firmware);
	return 0;
}

static void metalio_touch_emit(struct metalio_ft6336u *touch,
			       const struct metalio_touch_frame *frame)
{
	unsigned int i, slot;

	/* Release missing IDs before reusing their slots for a new contact. */
	for (slot = 0; slot < METALIO_TOUCH_CONTACTS; slot++) {
		for (i = 0; i < frame->count; i++)
			if (touch->slot_id[slot] == frame->points[i].id)
				break;
		if (i == frame->count && touch->slot_id[slot] >= 0) {
			input_mt_slot(touch->input, slot);
			input_mt_report_slot_state(touch->input, MT_TOOL_FINGER, false);
			touch->slot_id[slot] = -1;
		}
	}
	for (i = 0; i < frame->count; i++) {
		const struct metalio_touch_point *point = &frame->points[i];

		for (slot = 0; slot < METALIO_TOUCH_CONTACTS; slot++)
			if (touch->slot_id[slot] == point->id)
				break;
		if (slot == METALIO_TOUCH_CONTACTS) {
			for (slot = 0; slot < METALIO_TOUCH_CONTACTS; slot++)
				if (touch->slot_id[slot] < 0)
					break;
			touch->slot_id[slot] = point->id;
		}
		input_mt_slot(touch->input, slot);
		input_mt_report_slot_state(touch->input, MT_TOOL_FINGER, true);
		input_report_abs(touch->input, ABS_MT_POSITION_X, point->x);
		input_report_abs(touch->input, ABS_MT_POSITION_Y, point->y);
	}
	input_mt_sync_frame(touch->input);
	input_report_key(touch->input, KEY_ENTER, frame->enter);
	input_report_key(touch->input, KEY_REFRESH, frame->confirm);
	input_sync(touch->input);
}

static void metalio_touch_sample(struct work_struct *work)
{
	struct metalio_ft6336u *touch = container_of(to_delayed_work(work),
						   struct metalio_ft6336u, sample);
	struct metalio_touch_report report;
	struct metalio_touch_frame frame;
	s64 elapsed;
	u8 raw[METALIO_TOUCH_REPORT_SIZE];
	int ret;

	if (!READ_ONCE(touch->running))
		return;
	elapsed = ktime_us_delta(ktime_get(), touch->last_read);
	if (touch->have_last_read && elapsed < 5000) {
		schedule_delayed_work(&touch->sample,
				      usecs_to_jiffies(5000 - elapsed));
		return;
	}
	touch->last_read = ktime_get();
	touch->have_last_read = true;
	ret = metalio_touch_read(touch, 0x02, raw, sizeof(raw));
	if (ret || !metalio_touch_decode(raw, sizeof(raw), &report)) {
		dev_warn_ratelimited(&touch->client->dev, "invalid touch sample: %d\n",
				    ret ?: -EPROTO);
		metalio_touch_fail(&touch->state, &frame);
	} else {
		metalio_touch_update(&touch->state, &report, &frame);
	}
	metalio_touch_emit(touch, &frame);
	if (READ_ONCE(touch->running))
		schedule_delayed_work(&touch->sample, msecs_to_jiffies(10));
}

static irqreturn_t metalio_touch_irq(int irq, void *context)
{
	struct metalio_ft6336u *touch = context;

	if (READ_ONCE(touch->running))
		mod_delayed_work(system_wq, &touch->sample, 0);
	return IRQ_HANDLED;
}

static int metalio_touch_start(struct input_dev *input)
{
	struct metalio_ft6336u *touch = input_get_drvdata(input);

	if (touch->suspended)
		return -EHOSTDOWN;
	metalio_touch_init(&touch->state);
	touch->have_last_read = false;
	WRITE_ONCE(touch->running, true);
	enable_irq(touch->client->irq);
	schedule_delayed_work(&touch->sample, 0);
	return 0;
}

static void metalio_touch_stop(struct input_dev *input)
{
	struct metalio_ft6336u *touch = input_get_drvdata(input);
	struct metalio_touch_frame frame;

	if (!touch->running)
		return;
	WRITE_ONCE(touch->running, false);
	disable_irq(touch->client->irq);
	cancel_delayed_work_sync(&touch->sample);
	metalio_touch_fail(&touch->state, &frame);
	metalio_touch_emit(touch, &frame);
}

static int metalio_touch_suspend(struct device *dev)
{
	struct metalio_ft6336u *touch = dev_get_drvdata(dev);

	mutex_lock(&touch->input->mutex);
	touch->suspended = true;
	metalio_touch_stop(touch->input);
	mutex_unlock(&touch->input->mutex);
	return 0;
}

static int metalio_touch_resume(struct device *dev)
{
	struct metalio_ft6336u *touch = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&touch->input->mutex);
	if (input_device_enabled(touch->input))
		ret = metalio_touch_reset(touch);
	if (!ret) {
		touch->suspended = false;
		if (input_device_enabled(touch->input))
			ret = metalio_touch_start(touch->input);
	}
	mutex_unlock(&touch->input->mutex);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(metalio_touch_pm, metalio_touch_suspend,
			      metalio_touch_resume);

static int metalio_touch_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct metalio_ft6336u *touch;
	struct input_dev *input;
	int ret;

	if (client->addr != 0x38 || client->irq <= 0)
		return dev_err_probe(dev, -EINVAL, "requires address 0x38 and IRQ\n");
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	touch = devm_kzalloc(dev, sizeof(*touch), GFP_KERNEL);
	if (!touch)
		return -ENOMEM;
	touch->client = client;
	i2c_set_clientdata(client, touch);
	INIT_DELAYED_WORK(&touch->sample, metalio_touch_sample);
	touch->slot_id[0] = touch->slot_id[1] = -1;
	touch->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(touch->reset))
		return dev_err_probe(dev, PTR_ERR(touch->reset), "reset GPIO\n");
	ret = metalio_touch_reset(touch);
	if (ret)
		return dev_err_probe(dev, ret, "touch controller reset/identity\n");
	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	touch->input = input;
	input->name = "Metalio FT6336U touchscreen and keys";
	input->id.bustype = BUS_I2C;
	input->open = metalio_touch_start;
	input->close = metalio_touch_stop;
	input_set_drvdata(input, touch);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, 683, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, 1215, 0, 0);
	input_set_capability(input, EV_KEY, KEY_ENTER);
	input_set_capability(input, EV_KEY, KEY_REFRESH);
	ret = input_mt_init_slots(input, METALIO_TOUCH_CONTACTS, INPUT_MT_DIRECT);
	if (ret)
		return ret;
	ret = devm_request_irq(dev, client->irq, metalio_touch_irq,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_NO_AUTOEN,
			       dev_name(dev), touch);
	if (ret)
		return dev_err_probe(dev, ret, "touch IRQ\n");
	return input_register_device(input);
}

static void metalio_touch_shutdown(struct i2c_client *client)
{
	struct metalio_ft6336u *touch = i2c_get_clientdata(client);

	mutex_lock(&touch->input->mutex);
	touch->suspended = true;
	metalio_touch_stop(touch->input);
	mutex_unlock(&touch->input->mutex);
}

static const struct of_device_id metalio_touch_of_match[] = {
	{ .compatible = "metalio,ft6336u" },
	{ }
};
MODULE_DEVICE_TABLE(of, metalio_touch_of_match);

static struct i2c_driver metalio_touch_driver = {
	.driver = {
		.name = "metalio-ft6336u",
		.of_match_table = metalio_touch_of_match,
		.pm = pm_sleep_ptr(&metalio_touch_pm),
	},
	.probe = metalio_touch_probe,
	.remove = metalio_touch_shutdown,
	.shutdown = metalio_touch_shutdown,
};
module_i2c_driver(metalio_touch_driver);

MODULE_DESCRIPTION("Metalio E-Ink4 Plus FT6336U touchscreen and confirm/Enter keys");
MODULE_LICENSE("GPL");
