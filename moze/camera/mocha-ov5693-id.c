// SPDX-License-Identifier: GPL-2.0-only
/* Mocha front camera identification and opt-in one-shot capture. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include "ov5693-mocha-720p.h"
#include "mocha-vi2-capture.h"

static bool check_mode;
static bool video;
module_param(video, bool, 0400);

static bool capture_frame;
module_param(capture_frame, bool, 0400);

static bool live_frame;
module_param(live_frame, bool, 0400);
MODULE_PARM_DESC(live_frame, "Use the sensor image in place of its color bars");

module_param(check_mode, bool, 0400);
MODULE_PARM_DESC(check_mode, "Program and read back vendor 720p mode without streaming");

static int mocha_ov5693_write(struct i2c_client *client, u16 reg, u8 value)
{
	u8 data[] = { reg >> 8, reg & 0xff, value };
	int ret = i2c_master_send(client, data, sizeof(data));

	return ret == sizeof(data) ? 0 : ret < 0 ? ret : -EIO;
}

static int mocha_ov5693_stream(void *data, bool enable)
{
	struct i2c_client *client = data;

	return mocha_ov5693_write(client, 0x0100, enable ? 1 : 0);
}

static int mocha_ov5693_exposure(struct i2c_client *client, bool optical)
{
	int ret;

	if (optical) {
		/* The mode table uses manual exposure near the frame duration
		 * and 4x analog gain. Use 480 lines at 2x gain for the optical probe.
		 */
		ret = mocha_ov5693_write(client, 0x3501, 0x1e);
		if (ret)
			return ret;
		ret = mocha_ov5693_write(client, 0x3502, 0x00);
		if (ret)
			return ret;
		ret = mocha_ov5693_write(client, 0x350b, 0x20);
		if (ret)
			return ret;
	}
	return mocha_ov5693_write(client, 0x5e00, optical ? 0 : 0x80);
}

static int mocha_ov5693_capture(struct i2c_client *client)
{
	typeof(mocha_vi2_run_sensor) *run;
	int ret;

	ret = mocha_ov5693_exposure(client, live_frame);
	if (ret)
		return ret;
	run = symbol_get(mocha_vi2_run_sensor);
	if (!run)
		return -ENODEV;
	ret = run(&client->dev, mocha_ov5693_stream, client, live_frame, false, NULL, NULL);
	symbol_put(mocha_vi2_run_sensor);
	return ret;
}

static int mocha_ov5693_check_mode(struct i2c_client *client)
{
	u8 address[] = { 0x38, 0x08 }, size[4];
	struct i2c_msg messages[] = {
		{ .addr = client->addr, .len = sizeof(address), .buf = address },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = sizeof(size), .buf = size },
	};
	int ret;

	ret = mocha_ov5693_write(client, 0x0103, 1);
	if (ret)
		return ret;
	usleep_range(10000, 12000);
	for (size_t i = 0; i < ARRAY_SIZE(mocha_ov5693_720p); i++) {
		ret = mocha_ov5693_write(client, mocha_ov5693_720p[i].addr,
					mocha_ov5693_720p[i].value);
		if (ret)
			return dev_err_probe(&client->dev, ret, "mode write at 0x%04x failed\n",
					     mocha_ov5693_720p[i].addr);
	}
	ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));
	if (ret != ARRAY_SIZE(messages))
		return ret < 0 ? ret : -EIO;
	if ((size[0] << 8 | size[1]) != 1280 || (size[2] << 8 | size[3]) != 720)
		return dev_err_probe(&client->dev, -EIO, "unexpected 720p mode dimensions\n");
	dev_info(&client->dev, "front vendor mode 1280x720 readback verified (standby)\n");
	return 0;
}

static int mocha_ov5693_configure(struct i2c_client *client, void *data)
{
	int ret = mocha_ov5693_check_mode(client);

	return ret ? ret : mocha_ov5693_exposure(client, true);
}

static int mocha_ov5693_powered_inner(struct i2c_client *client,
		mocha_sensor_action action, void *data)
{
	struct device *dev = &client->dev;
	/* The vendor sequence enables AF and analog before I/O; core comes last. */
	struct regulator_bulk_data supplies[] = {
		{ .supply = "af" }, { .supply = "avdd" },
		{ .supply = "dovdd" }, { .supply = "dvdd" },
	};
	struct gpio_desc *reset, *powerdown, *af;
	struct clk_bulk_data clocks[] = { { .id = "xvclk" }, { .id = "vim2" } };
	int ret, rails = 0;

	if (!of_machine_is_compatible("nvidia,mocha") ||
	    !of_device_is_compatible(dev->of_node, "moze,mocha-ov5693-id"))
		return -ENODEV;

	/* Managed bulk getters retain their array for later cleanup; these
	 * arrays live on the stack. Register each handle individually.
	 */
	for (int i = 0; i < ARRAY_SIZE(supplies); i++) {
		supplies[i].consumer = devm_regulator_get(dev, supplies[i].supply);
		if (IS_ERR(supplies[i].consumer))
			return dev_err_probe(dev, PTR_ERR(supplies[i].consumer),
					     "cannot get rail %s\n", supplies[i].supply);
	}
	for (int i = 0; i < ARRAY_SIZE(clocks); i++) {
		clocks[i].clk = devm_clk_get(dev, clocks[i].id);
		if (IS_ERR(clocks[i].clk))
			return dev_err_probe(dev, PTR_ERR(clocks[i].clk),
					     "cannot get clock %s\n", clocks[i].id);
	}
	if (clk_get_rate(clocks[0].clk) != 24000000)
		return dev_err_probe(dev, -EINVAL, "expected 24 MHz XVCLK\n");

	reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "cannot get reset GPIO\n");
	powerdown = devm_gpiod_get(dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(powerdown))
		return dev_err_probe(dev, PTR_ERR(powerdown), "cannot get powerdown GPIO\n");
	af = devm_gpiod_get(dev, "af-enable", GPIOD_OUT_LOW);
	if (IS_ERR(af))
		return dev_err_probe(dev, PTR_ERR(af), "cannot get autofocus enable GPIO\n");

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(clocks), clocks);
	if (ret)
		return dev_err_probe(dev, ret, "cannot enable camera clocks\n");
	usleep_range(3000, 3500);

	for (; rails < ARRAY_SIZE(supplies); rails++) {
		ret = regulator_enable(supplies[rails].consumer);
		if (ret) {
			dev_err(dev, "cannot enable %s: %d\n", supplies[rails].supply, ret);
			goto power_off;
		}
		if (rails == 2)
			gpiod_set_value_cansleep(powerdown, 0);
	}
	usleep_range(3000, 3500);
	gpiod_set_value_cansleep(reset, 0);
	usleep_range(10000, 12000);

	ret = action(client, data);

power_off:
	gpiod_set_value_cansleep(reset, 1);
	udelay(2);
	while (rails > 2)
		regulator_disable(supplies[--rails].consumer);
	gpiod_set_value_cansleep(powerdown, 1);
	while (rails)
		regulator_disable(supplies[--rails].consumer);
	clk_bulk_disable_unprepare(ARRAY_SIZE(clocks), clocks);
	return ret;
}

static int mocha_ov5693_powered(struct i2c_client *client,
		mocha_sensor_action action, void *data)
{
	void *group = devres_open_group(&client->dev, NULL, GFP_KERNEL);
	int ret;

	if (!group)
		return -ENOMEM;
	ret = mocha_ov5693_powered_inner(client, action, data);
	devres_release_group(&client->dev, group);
	return ret;
}

static int mocha_ov5693_identify(struct i2c_client *client, void *data)
{
	struct device *dev = &client->dev;
	u8 address[] = { 0x30, 0x0a }, id[2];
	struct i2c_msg messages[] = {
		{ .addr = client->addr, .len = sizeof(address), .buf = address },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = sizeof(id), .buf = id },
	};
	int ret;

	ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));
	if (ret == ARRAY_SIZE(messages)) {
		unsigned int chip = id[0] << 8 | id[1];

		if (chip == 0x5690) {
			dev_info(dev, "front camera chip ID 0x%04x verified\n", chip);
			ret = !video && (check_mode || capture_frame) ?
				mocha_ov5693_check_mode(client) : 0;
			if (!ret && !video && capture_frame)
				ret = mocha_ov5693_capture(client);
		} else {
			dev_err(dev, "unexpected front camera chip ID 0x%04x\n", chip);
			ret = -ENODEV;
		}
	} else {
		ret = ret < 0 ? ret : -EIO;
		dev_err(dev, "front camera ID read failed: %d\n", ret);
	}

	return ret;
}

static typeof(mocha_video_unregister) *mocha_ov5693_unregister;

static int mocha_ov5693_id(struct i2c_client *client)
{
	typeof(mocha_video_register) *register_video;
	typeof(mocha_video_unregister) *unregister_video;
	struct mocha_video *camera;
	int ret = mocha_ov5693_powered(client, mocha_ov5693_identify, NULL);

	if (ret || !video)
		return ret;
	register_video = symbol_get(mocha_video_register);
	unregister_video = symbol_get(mocha_video_unregister);
	if (!register_video || !unregister_video) {
		if (register_video)
			symbol_put(mocha_video_register);
		if (unregister_video)
			symbol_put(mocha_video_unregister);
		return -EPROBE_DEFER;
	}
	camera = register_video(client, false,
				mocha_ov5693_powered, mocha_ov5693_configure,
				mocha_ov5693_stream);
	symbol_put(mocha_video_register);
	if (IS_ERR(camera)) {
		symbol_put(mocha_video_unregister);
		return PTR_ERR(camera);
	}
	mocha_ov5693_unregister = unregister_video;
	i2c_set_clientdata(client, camera);
	return 0;
}

static void mocha_ov5693_remove(struct i2c_client *client)
{
	struct mocha_video *camera = i2c_get_clientdata(client);

	if (!camera)
		return;
	mocha_ov5693_unregister(camera);
	symbol_put(mocha_video_unregister);
}

static const struct of_device_id mocha_ov5693_id_match[] = {
	{ .compatible = "moze,mocha-ov5693-id" },
	{ }
};

static struct i2c_driver mocha_ov5693_id_driver = {
	.driver = {
		.name = "mocha-ov5693-id",
		.of_match_table = mocha_ov5693_id_match,
	},
	.probe = mocha_ov5693_id,
	.remove = mocha_ov5693_remove,
};
module_i2c_driver(mocha_ov5693_id_driver);

MODULE_DESCRIPTION("Mocha front OV5693 one-shot ID probe");
MODULE_LICENSE("GPL");
