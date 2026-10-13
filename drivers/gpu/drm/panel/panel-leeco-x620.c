// SPDX-License-Identifier: GPL-2.0-only
/* X620 panel 378C81, initialization captured from the owner's stock LK. */
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <video/mipi_display.h>

struct x620_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regmap *mmsys;
	bool prepared;
};

struct x620_panel_command {
	u8 type;
	u8 length;
	u16 delay_ms;
	u8 data[64];
};

static const struct x620_panel_command x620_init[] = {
	{ .type = 0x23, .length = 2, .data = { 0xb0, 0x00 } },
	{ .type = 0x29, .length = 6, .data = { 0xb3, 0x04, 0x10, 0x00, 0x00, 0x00 } },
	{ .type = 0x29, .length = 3, .data = { 0xb4, 0x0c, 0x00 } },
	{ .type = 0x29, .length = 4, .data = { 0xb6, 0x3b, 0xd3, 0x00 } },
	{ .type = 0x23, .length = 2, .data = { 0xc0, 0x00 } },
	{ .type = 0x15, .length = 2, .data = { 0x36, 0x98 } },
	{ .type = 0x23, .length = 2, .data = { 0xcc, 0x04 } },
	{ .type = 0x29, .length = 32, .data = { 0xc1, 0x84, 0x00, 0x10, 0xef, 0x8b, 0xf1, 0xff, 0xff, 0xdf, 0x9c, 0xc5, 0x9a, 0x73, 0x8d, 0xad, 0x63, 0xfe, 0xff, 0xff, 0xcb, 0xf8, 0x01, 0x00, 0xaa, 0x40, 0x00, 0xc2, 0x01, 0x08, 0x00, 0x01 } },
	{ .type = 0x29, .length = 10, .data = { 0xcb, 0x0d, 0xfe, 0x1f, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ .type = 0x29, .length = 11, .data = { 0xc2, 0x01, 0xf7, 0x80, 0x04, 0x63, 0x00, 0x60, 0x00, 0x01, 0x30 } },
	{ .type = 0x29, .length = 7, .data = { 0xc3, 0x55, 0x01, 0x00, 0x01, 0x00, 0x00 } },
	{ .type = 0x29, .length = 18, .data = { 0xc4, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x00, 0x05, 0x01, 0x00, 0x00, 0x00 } },
	{ .type = 0x29, .length = 15, .data = { 0xc6, 0x59, 0x07, 0x4a, 0x07, 0x4a, 0x01, 0x0e, 0x01, 0x02, 0x01, 0x02, 0x09, 0x15, 0x07 } },
	{ .type = 0x29, .length = 31, .data = { 0xc7, 0x00, 0x14, 0x1d, 0x29, 0x39, 0x47, 0x50, 0x5c, 0x3f, 0x46, 0x50, 0x5d, 0x6e, 0x76, 0x7a, 0x00, 0x14, 0x1d, 0x29, 0x39, 0x47, 0x50, 0x5c, 0x3f, 0x46, 0x50, 0x5d, 0x6e, 0x76, 0x7a } },
	{ .type = 0x29, .length = 20, .data = { 0xc8, 0x01, 0x00, 0xfe, 0xfa, 0x02, 0xfc, 0x00, 0x00, 0xfe, 0xff, 0xfc, 0xf9, 0x00, 0x00, 0x02, 0x07, 0x05, 0xdd, 0x00 } },
	{ .type = 0x29, .length = 9, .data = { 0xc9, 0x1f, 0x68, 0x1f, 0x68, 0x4c, 0x4c, 0xc4, 0x11 } },
	{ .type = 0x29, .length = 17, .data = { 0xd0, 0x33, 0x01, 0x91, 0x0b, 0xd9, 0x19, 0x19, 0x00, 0x00, 0x00, 0x19, 0x99, 0x00, 0x00, 0x00, 0x00 } },
	{ .type = 0x29, .length = 29, .data = { 0xd3, 0x1b, 0x3b, 0xbb, 0xad, 0xa5, 0x33, 0x33, 0x33, 0x00, 0x80, 0xad, 0xa8, 0x5b, 0x5b, 0x33, 0x33, 0x33, 0xf7, 0xf2, 0x1f, 0x7d, 0x7c, 0xff, 0x0f, 0x99, 0x00, 0xff, 0xff } },
	{ .type = 0x29, .length = 4, .data = { 0xd4, 0x57, 0x33, 0x03 } },
	{ .type = 0x29, .length = 12, .data = { 0xd5, 0x66, 0x00, 0x00, 0x01, 0x33, 0x01, 0x33, 0x00, 0x43, 0x00, 0x43 } },
	{ .type = 0x29, .length = 2, .data = { 0xbe, 0x04 } },
	{ .type = 0x29, .length = 17, .data = { 0xcf, 0x40, 0x10, 0x00, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ .type = 0x29, .length = 6, .data = { 0xde, 0x00, 0x00, 0x3f, 0xff, 0x10 } },
	{ .type = 0x29, .length = 2, .data = { 0xe9, 0x00 } },
	{ .type = 0x29, .length = 2, .data = { 0xf2, 0x00 } },
	{ .type = 0x23, .length = 2, .data = { 0xd6, 0x01 } },
	{ .type = 0x39, .length = 2, .data = { 0x35, 0x00 } },
	{ .type = 0x39, .length = 1, .data = { 0x29 } },
	{ .delay_ms = 10 },
	{ .type = 0x39, .length = 1, .data = { 0x11 } },
	{ .delay_ms = 150 },
};

static inline struct x620_panel *to_x620_panel(struct drm_panel *panel)
{
	return container_of(panel, struct x620_panel, panel);
}

static int x620_panel_prepare(struct drm_panel *panel)
{
	struct x620_panel *p = to_x620_panel(panel);
	int ret, i;

	if (p->prepared)
		return 0;
	/* LK's DSI reset utility uses MMSYS LCM_RST_B, not an unrelated GPIO. */
	ret = regmap_update_bits(p->mmsys, 0x150, BIT(0), BIT(0));
	if (ret)
		return ret;
	msleep(5);
	ret = regmap_update_bits(p->mmsys, 0x150, BIT(0), 0);
	if (ret)
		return ret;
	msleep(10);
	ret = regmap_update_bits(p->mmsys, 0x150, BIT(0), BIT(0));
	if (ret)
		return ret;
	msleep(30);
	for (i = 0; i < ARRAY_SIZE(x620_init); i++) {
		const struct x620_panel_command *cmd = &x620_init[i];

		if (cmd->delay_ms) {
			msleep(cmd->delay_ms);
			continue;
		}
		if (cmd->type == MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM ||
		    cmd->type == MIPI_DSI_GENERIC_LONG_WRITE)
			ret = mipi_dsi_generic_write(p->dsi, cmd->data, cmd->length);
		else
			ret = mipi_dsi_dcs_write_buffer(p->dsi, cmd->data, cmd->length);
		if (ret < 0)
			return dev_err_probe(panel->dev, ret, "panel command %u failed\n", i);
	}
	p->prepared = true;
	dev_info(panel->dev, "X620 panel 378C81: native initialization completed\n");
	return 0;
}

static int x620_panel_enable(struct drm_panel *panel)
{
	struct x620_panel *p = to_x620_panel(panel);
	int ret;

	ret = mipi_dsi_dcs_set_column_address(p->dsi, 0, 1079);
	if (ret < 0)
		return ret;
	return mipi_dsi_dcs_set_page_address(p->dsi, 0, 1919);
}

static int x620_panel_disable(struct drm_panel *panel)
{
	struct x620_panel *p = to_x620_panel(panel);
	int ret;

	ret = mipi_dsi_dcs_set_display_off(p->dsi);
	msleep(50);
	return ret;
}

static int x620_panel_unprepare(struct drm_panel *panel)
{
	struct x620_panel *p = to_x620_panel(panel);
	const u8 standby[] = { 0xb1, 0x01 };
	int ret;

	if (!p->prepared)
		return 0;
	ret = mipi_dsi_dcs_enter_sleep_mode(p->dsi);
	if (ret < 0)
		return ret;
	msleep(120);
	ret = mipi_dsi_generic_write(p->dsi, standby, sizeof(standby));
	msleep(20);
	p->prepared = false;
	return ret < 0 ? ret : 0;
}

static const struct drm_display_mode x620_mode = {
	.clock = 147442,
	.hdisplay = 1080,
	.hsync_start = 1080 + 100,
	.hsync_end = 1080 + 100 + 20,
	.htotal = 1080 + 100 + 20 + 82,
	.vdisplay = 1920,
	.vsync_start = 1920 + 3,
	.vsync_end = 1920 + 3 + 15,
	.vtotal = 1920 + 3 + 15 + 9,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int x620_panel_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &x620_mode);
	if (!mode)
		return -ENOMEM;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);
	connector->display_info.width_mm = 68;
	connector->display_info.height_mm = 121;
	connector->display_info.bpc = 8;
	return 1;
}

static const struct drm_panel_funcs x620_panel_funcs = {
	.prepare = x620_panel_prepare,
	.enable = x620_panel_enable,
	.disable = x620_panel_disable,
	.unprepare = x620_panel_unprepare,
	.get_modes = x620_panel_get_modes,
};

static int x620_panel_probe(struct mipi_dsi_device *dsi)
{
	struct x620_panel *p;
	int ret;

	p = devm_kzalloc(&dsi->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->mmsys = syscon_regmap_lookup_by_phandle(dsi->dev.of_node, "mediatek,mmsys");
	if (IS_ERR(p->mmsys))
		return dev_err_probe(&dsi->dev, PTR_ERR(p->mmsys), "MMSYS reset unavailable\n");
	p->dsi = dsi;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS;
	dsi->hs_rate = 920000000; /* LK PLL_CLOCK = 460 MHz, doubled lane bit rate. */
	drm_panel_init(&p->panel, &dsi->dev, &x620_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	drm_panel_add(&p->panel);
	mipi_dsi_set_drvdata(dsi, p);
	ret = mipi_dsi_attach(dsi);
	if (ret)
		drm_panel_remove(&p->panel);
	return ret;
}

static void x620_panel_remove(struct mipi_dsi_device *dsi)
{
	struct x620_panel *p = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&p->panel);
}

static const struct of_device_id x620_panel_ids[] = {
	{ .compatible = "leeco,x620-378c81" },
	{ }
};
MODULE_DEVICE_TABLE(of, x620_panel_ids);

static struct mipi_dsi_driver x620_panel_driver = {
	.probe = x620_panel_probe,
	.remove = x620_panel_remove,
	.driver = {
		.name = "panel-x620-378c81",
		.of_match_table = x620_panel_ids,
	},
};
module_mipi_dsi_driver(x620_panel_driver);
MODULE_DESCRIPTION("LeEco X620 378C81 command-mode panel");
MODULE_LICENSE("GPL");
