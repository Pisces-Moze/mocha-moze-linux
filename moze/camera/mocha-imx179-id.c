// SPDX-License-Identifier: GPL-2.0-only
/* Mocha rear camera identification and opt-in capture. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include "imx179-mocha-720p.h"
#include "mocha-vi2-capture.h"

static bool video;
module_param(video, bool, 0400);

static bool capture_frame;
module_param(capture_frame, bool, 0400);

/* Mocha RAM board wiring: CAM1_PWDN is driven high for streaming. */
static struct gpiod_lookup_table rear_gpio = {
	.dev_id = "2-0010",
	.table = { GPIO_LOOKUP("tegra-gpio", 221, "cam-enable", GPIO_ACTIVE_HIGH), {} },
};

static int mocha_imx179_write(struct i2c_client *client, u16 reg, u8 value)
{
	u8 data[] = { reg >> 8, reg & 0xff, value };
	int ret = i2c_master_send(client, data, sizeof(data));

	return ret == sizeof(data) ? 0 : ret < 0 ? ret : -EIO;
}

static int mocha_imx179_stream(void *data, bool enable)
{
	struct i2c_client *client = data;
	u8 addr[] = { 0x01, 0x00 }, mode;
	struct i2c_msg msgs[] = {
		{ .addr = client->addr, .len = sizeof(addr), .buf = addr },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = 1, .buf = &mode },
	};
	int ret = mocha_imx179_write(client, 0x0100, enable);

	if (ret || !enable)
		return ret;
	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return ret < 0 ? ret : -EIO;
	dev_info(&client->dev, "rear sensor stream register %#x\n", mode);
	return mode & 1 ? 0 : -EIO;
}

static int mocha_imx179_configure(struct i2c_client *client, void *data)
{
	int ret;

	ret = mocha_imx179_stream(client, false);
	if (ret)
		return ret;
	usleep_range(3000, 4000);
	for (size_t i = 0; i < ARRAY_SIZE(mocha_imx179_720p); i++) {
		ret = mocha_imx179_write(client, mocha_imx179_720p[i].addr,
					 mocha_imx179_720p[i].value);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "mode write at 0x%04x failed\n",
					     mocha_imx179_720p[i].addr);
	}

	return 0;
}

static int mocha_imx179_capture(struct i2c_client *client)
{
	typeof(mocha_vi2_run_sensor) *run;
	int ret;

	ret = mocha_imx179_configure(client, NULL);
	if (ret)
		return ret;

	run = symbol_get(mocha_vi2_run_sensor);
	if (!run)
		return -ENODEV;
	ret = run(&client->dev, mocha_imx179_stream, client, true, true, NULL, NULL);
	symbol_put(mocha_vi2_run_sensor);
	return ret;
}

static int mocha_imx179_powered_inner(struct i2c_client *client,
		mocha_sensor_action action, void *data)
{
	struct device *dev = &client->dev;
	/* Match the Mocha-specific ardbeg_imx179_power_on board sequence. */
	struct regulator_bulk_data supplies[] = {
		{ .supply = "af" }, { .supply = "dvdd" },
		{ .supply = "dovdd" }, { .supply = "avdd" },
	};
	struct gpio_desc *reset, *af, *enable;
	struct clk_bulk_data clocks[] = { { .id = "xvclk" }, { .id = "csus" } };
	int ret, rails = 0;

	if (!of_machine_is_compatible("nvidia,mocha") ||
	    !of_device_is_compatible(dev->of_node, "moze,mocha-imx179-id"))
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
	af = devm_gpiod_get(dev, "af-enable", GPIOD_OUT_LOW);
	if (IS_ERR(af))
		return dev_err_probe(dev, PTR_ERR(af), "cannot get autofocus enable GPIO\n");

	gpiod_add_lookup_table(&rear_gpio);
	enable = devm_gpiod_get(dev, "cam-enable", GPIOD_OUT_LOW);
	gpiod_remove_lookup_table(&rear_gpio);
	if (IS_ERR(enable))
		return dev_err_probe(dev, PTR_ERR(enable), "cannot get camera enable GPIO\n");

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(clocks), clocks);
	if (ret)
		return dev_err_probe(dev, ret, "cannot enable camera clocks\n");

	for (; rails < ARRAY_SIZE(supplies); rails++) {
		ret = regulator_enable(supplies[rails].consumer);
		if (ret) {
			dev_err(dev, "cannot enable %s: %d\n", supplies[rails].supply, ret);
			goto power_off;
		}
	}
	gpiod_set_value_cansleep(af, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(reset, 0);
	gpiod_set_value_cansleep(enable, 1);
	usleep_range(10000, 12000);

	ret = action(client, data);

power_off:
	gpiod_set_value_cansleep(enable, 0);
	gpiod_set_value_cansleep(reset, 1);
	udelay(2);
	gpiod_set_value_cansleep(af, 0);
	while (rails)
		regulator_disable(supplies[--rails].consumer);
	clk_bulk_disable_unprepare(ARRAY_SIZE(clocks), clocks);
	return ret;
}

static int mocha_imx179_powered(struct i2c_client *client,
		mocha_sensor_action action, void *data)
{
	void *group = devres_open_group(&client->dev, NULL, GFP_KERNEL);
	int ret;

	if (!group)
		return -ENOMEM;
	ret = mocha_imx179_powered_inner(client, action, data);
	devres_release_group(&client->dev, group);
	return ret;
}

static int mocha_imx179_identify(struct i2c_client *client, void *data)
{
	struct device *dev = &client->dev;
	u8 address[] = { 0x00, 0x02 }, id[2];
	struct i2c_msg messages[] = {
		{ .addr = client->addr, .len = sizeof(address), .buf = address },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = sizeof(id), .buf = id },
	};
	int ret;

	ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));
	if (ret == ARRAY_SIZE(messages)) {
		unsigned int chip = id[0] << 8 | id[1];

		if (chip == 0x8179) {
			dev_info(dev, "rear camera chip ID 0x%04x verified\n", chip);
			ret = !video && capture_frame ? mocha_imx179_capture(client) : 0;
		} else {
			dev_err(dev, "unexpected rear camera chip ID 0x%04x\n", chip);
			ret = -ENODEV;
		}
	} else {
		ret = ret < 0 ? ret : -EIO;
		dev_err(dev, "rear camera ID read failed: %d\n", ret);
	}

	return ret;
}

static typeof(mocha_video_unregister) *mocha_imx179_unregister;

static int mocha_imx179_id(struct i2c_client *client)
{
	typeof(mocha_video_register) *register_video;
	typeof(mocha_video_unregister) *unregister_video;
	struct mocha_video *camera;
	int ret = mocha_imx179_powered(client, mocha_imx179_identify, NULL);

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
	camera = register_video(client, true,
				mocha_imx179_powered, mocha_imx179_configure,
				mocha_imx179_stream);
	symbol_put(mocha_video_register);
	if (IS_ERR(camera)) {
		symbol_put(mocha_video_unregister);
		return PTR_ERR(camera);
	}
	mocha_imx179_unregister = unregister_video;
	i2c_set_clientdata(client, camera);
	return 0;
}

static void mocha_imx179_remove(struct i2c_client *client)
{
	struct mocha_video *camera = i2c_get_clientdata(client);

	if (!camera)
		return;
	mocha_imx179_unregister(camera);
	symbol_put(mocha_video_unregister);
}

static const struct of_device_id mocha_imx179_id_match[] = {
	{ .compatible = "moze,mocha-imx179-id" },
	{ }
};

static struct i2c_driver mocha_imx179_id_driver = {
	.driver = {
		.name = "mocha-imx179-id",
		.of_match_table = mocha_imx179_id_match,
	},
	.probe = mocha_imx179_id,
	.remove = mocha_imx179_remove,
};
module_i2c_driver(mocha_imx179_id_driver);

MODULE_DESCRIPTION("Mocha rear IMX179 one-shot ID probe");
MODULE_LICENSE("GPL");
