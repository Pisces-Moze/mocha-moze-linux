// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experimental Linux 6.12 port for the Xiaomi Mocha dual-DSI panel.
 * Mode and command sequence derived from Alexandrov Dmitry's 2018 driver:
 * Insei/linux commit 1e3857d7a1ea87cd2cc15eca2d36f57cb591c4bf.
 * This port has not been tested on the physical panel.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <video/mipi_display.h>

struct mocha_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *links[2];
	struct regulator *vddio;
	struct regulator *vsp;
	struct regulator *vsn;
	struct gpio_desc *reset;
	bool prepared;
};

static inline struct mocha_panel *to_mocha(struct drm_panel *panel)
{
	return container_of(panel, struct mocha_panel, panel);
}

static int mocha_dcs(struct mocha_panel *p, u8 cmd, const void *buf, size_t len)
{
	int i;
	ssize_t ret;

	for (i = 0; i < ARRAY_SIZE(p->links); i++) {
		ret = mipi_dsi_dcs_write(p->links[i], cmd, buf, len);
		if (ret < 0) {
			dev_err(p->panel.dev, "MOCHA_PANEL_CMD: link=%d cmd=%02x error=%zd\n", i, cmd, ret);
			return ret;
		}
		/* Tegra returns the 4-byte wire header for short writes.
		 * Successful nonnegative host results are accepted by DCS helpers.
		 */
	}
	return 0;
}

static void mocha_poweroff(struct mocha_panel *p)
{
	gpiod_set_value_cansleep(p->reset, 1);
	regulator_disable(p->vsn);
	regulator_disable(p->vsp);
	regulator_disable(p->vddio);
	p->prepared = false;
}

static int mocha_prepare(struct drm_panel *panel)
{
	struct mocha_panel *p = to_mocha(panel);
	u8 value;
	int ret;

	if (p->prepared)
		return 0;
	ret = regulator_enable(p->vddio);
	if (ret)
		return ret;
	msleep(12);
	ret = regulator_enable(p->vsp);
	if (ret)
		goto disable_vddio;
	msleep(12);
	ret = regulator_enable(p->vsn);
	if (ret)
		goto disable_vsp;
	msleep(70);

	/* Logical 1 asserts the active-low reset described by reset-gpios. */
	gpiod_set_value_cansleep(p->reset, 0);
	usleep_range(1000, 3000);
	gpiod_set_value_cansleep(p->reset, 1);
	usleep_range(1000, 3000);
	gpiod_set_value_cansleep(p->reset, 0);
	msleep(32);
	ret = mocha_dcs(p, MIPI_DCS_EXIT_SLEEP_MODE, NULL, 0);
	if (ret)
		goto poweroff;
	msleep(120);
	value = 0xff;
	ret = mocha_dcs(p, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, &value, 1);
	if (ret)
		goto poweroff;
	msleep(20);
	value = 0x01;
	ret = mocha_dcs(p, MIPI_DCS_WRITE_POWER_SAVE, &value, 1);
	if (ret)
		goto poweroff;
	msleep(20);
	value = 0x2c; /* Exact MIUI sequence used by the verified U-Boot. */
	ret = mocha_dcs(p, MIPI_DCS_WRITE_CONTROL_DISPLAY, &value, 1);
	if (ret)
		goto poweroff;
	msleep(20);
	ret = mocha_dcs(p, MIPI_DCS_SET_DISPLAY_ON, NULL, 0);
	if (ret)
		goto poweroff;
	msleep(150);
	p->prepared = true;
	return 0;

poweroff:
	mocha_poweroff(p);
	return dev_err_probe(panel->dev, ret, "panel initialization failed\n");
disable_vsp:
	regulator_disable(p->vsp);
disable_vddio:
	regulator_disable(p->vddio);
	return ret;
}

static int mocha_unprepare(struct drm_panel *panel)
{
	struct mocha_panel *p = to_mocha(panel);
	int first, ret;

	if (!p->prepared)
		return 0;
	first = mocha_dcs(p, MIPI_DCS_SET_DISPLAY_OFF, NULL, 0);
	msleep(100);
	ret = mocha_dcs(p, MIPI_DCS_ENTER_SLEEP_MODE, NULL, 0);
	msleep(150);
	mocha_poweroff(p);
	return first ? first : ret;
}

static const struct drm_display_mode mocha_mode = {
	.clock = 215000, /* Same mode as the verified U-Boot. */
	.hdisplay = 1536,
	.hsync_start = 1536 + 136,
	.hsync_end = 1536 + 136 + 28,
	.htotal = 1536 + 136 + 28 + 28,
	.vdisplay = 2048,
	.vsync_start = 2048 + 14,
	.vsync_end = 2048 + 14 + 2,
	.vtotal = 2048 + 14 + 2 + 8,
};

static int mocha_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &mocha_mode);
	if (!mode)
		return -ENOMEM;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);
	connector->display_info.width_mm = 120;
	connector->display_info.height_mm = 160;
	return 1;
}

static const struct drm_panel_funcs mocha_panel_funcs = {
	.prepare = mocha_prepare,
	.unprepare = mocha_unprepare,
	.get_modes = mocha_get_modes,
};

static int mocha_probe(struct mipi_dsi_device *dsi)
{
	struct mipi_dsi_device *secondary;
	struct device_node *node;
	struct mocha_panel *p;
	int ret;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM;
	node = of_parse_phandle(dsi->dev.of_node, "link2", 0);
	/* Only the primary endpoint owns power resources and the DRM panel. */
	if (!node)
		return mipi_dsi_attach(dsi);
	secondary = of_find_mipi_dsi_device_by_node(node);
	of_node_put(node);
	if (!secondary)
		return -EPROBE_DEFER;
	p = devm_kzalloc(&dsi->dev, sizeof(*p), GFP_KERNEL);
	if (!p) {
		ret = -ENOMEM;
		goto put_secondary;
	}
	p->links[0] = dsi;
	p->links[1] = secondary;
	p->vddio = devm_regulator_get(&dsi->dev, "vddio");
	if (IS_ERR(p->vddio)) {
		ret = PTR_ERR(p->vddio);
		goto put_secondary;
	}
	p->vsp = devm_regulator_get(&dsi->dev, "vsp");
	if (IS_ERR(p->vsp)) {
		ret = PTR_ERR(p->vsp);
		goto put_secondary;
	}
	p->vsn = devm_regulator_get(&dsi->dev, "vsn");
	if (IS_ERR(p->vsn)) {
		ret = PTR_ERR(p->vsn);
		goto put_secondary;
	}
	p->reset = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(p->reset)) {
		ret = PTR_ERR(p->reset);
		goto put_secondary;
	}
	drm_panel_init(&p->panel, &dsi->dev, &mocha_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	ret = drm_panel_of_backlight(&p->panel);
	if (ret)
		goto put_secondary;
	mipi_dsi_set_drvdata(dsi, p);
	drm_panel_add(&p->panel);
	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&p->panel);
		goto put_secondary;
	}
	return 0;
put_secondary:
	put_device(&secondary->dev);
	return ret;
}

static void mocha_shutdown(struct mipi_dsi_device *dsi)
{
	struct mocha_panel *p = mipi_dsi_get_drvdata(dsi);

	if (p) {
		drm_panel_disable(&p->panel);
		drm_panel_unprepare(&p->panel);
	}
}

static void mocha_remove(struct mipi_dsi_device *dsi)
{
	struct mocha_panel *p = mipi_dsi_get_drvdata(dsi);

	mocha_shutdown(dsi);
	mipi_dsi_detach(dsi);
	if (p) {
		drm_panel_remove(&p->panel);
		put_device(&p->links[1]->dev);
	}
}

static const struct of_device_id mocha_of_match[] = {
	{ .compatible = "sharp,lq079l1sx01" },
	{ }
};
MODULE_DEVICE_TABLE(of, mocha_of_match);

static struct mipi_dsi_driver mocha_panel_driver = {
	.driver = {
		.name = "panel-sharp-lq079l1sx01",
		.of_match_table = mocha_of_match,
	},
	.probe = mocha_probe,
	.remove = mocha_remove,
	.shutdown = mocha_shutdown,
};
module_mipi_dsi_driver(mocha_panel_driver);
MODULE_DESCRIPTION("Experimental Mocha Sharp LQ079L1SX01 Linux 6.12 port");
MODULE_LICENSE("GPL");
