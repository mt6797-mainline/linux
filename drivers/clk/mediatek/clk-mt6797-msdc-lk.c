// SPDX-License-Identifier: GPL-2.0-only
/* Isolated MSDC0 clocks for X620 bring-up; no global PLL initialization. */
#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define CLK_CFG_7 0x70
#define MSDC0_SOURCE_GATE BIT(23)

struct msdc_lk_clock {
	struct clk_hw hw;
	struct regmap *map;
	bool source_gate;
};

static int msdc_lk_enable(struct clk_hw *hw)
{
	struct msdc_lk_clock *c = container_of(hw, struct msdc_lk_clock, hw);

	if (c->source_gate)
		return regmap_update_bits(c->map, CLK_CFG_7, MSDC0_SOURCE_GATE, 0);
	return regmap_write(c->map, 0x8c, BIT(2));
}

static void msdc_lk_disable(struct clk_hw *hw)
{
	struct msdc_lk_clock *c = container_of(hw, struct msdc_lk_clock, hw);

	if (c->source_gate)
		regmap_update_bits(c->map, CLK_CFG_7, MSDC0_SOURCE_GATE, MSDC0_SOURCE_GATE);
	else
		regmap_write(c->map, 0x88, BIT(2));
}

static int msdc_lk_is_enabled(struct clk_hw *hw)
{
	struct msdc_lk_clock *c = container_of(hw, struct msdc_lk_clock, hw);
	u32 value;
	int ret;

	ret = regmap_read(c->map, c->source_gate ? CLK_CFG_7 : 0x94, &value);
	if (ret)
		return 0;
	return !(value & (c->source_gate ? MSDC0_SOURCE_GATE : BIT(2)));
}

static unsigned long msdc_lk_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	return 26000000;
}

static const struct clk_ops msdc_lk_ops = {
	.enable = msdc_lk_enable,
	.disable = msdc_lk_disable,
	.is_enabled = msdc_lk_is_enabled,
	.recalc_rate = msdc_lk_rate,
};

static int msdc_lk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *data;
	struct regmap *top, *infra;
	struct msdc_lk_clock *clocks;
	int i, ret;

	if (!of_machine_is_compatible("leeco,x620"))
		return -ENODEV;
	top = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,topckgen");
	if (IS_ERR(top))
		return PTR_ERR(top);
	infra = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(infra))
		return PTR_ERR(infra);
	/* CLK_CFG_7: MSDC0 HCLK [9:8], source [19:16], source gate [23]. */
	ret = regmap_update_bits(top, CLK_CFG_7, MSDC0_SOURCE_GATE, MSDC0_SOURCE_GATE);
	if (ret)
		return ret;
	ret = regmap_update_bits(top, CLK_CFG_7, GENMASK(9, 8) | GENMASK(19, 16), 0);
	if (ret)
		return ret;
	data = devm_kzalloc(dev, struct_size(data, hws, 3), GFP_KERNEL);
	clocks = devm_kcalloc(dev, 2, sizeof(*clocks), GFP_KERNEL);
	if (!data || !clocks)
		return -ENOMEM;
	data->num = 3;
	data->hws[0] = devm_clk_hw_register_fixed_rate(dev, "x620-msdc0-source", NULL, 0, 26000000);
	if (IS_ERR(data->hws[0]))
		return PTR_ERR(data->hws[0]);
	for (i = 0; i < 2; i++) {
		struct clk_init_data init = {
			.name = i ? "x620-msdc0-source-gate" : "x620-msdc0-hclk",
			.ops = &msdc_lk_ops,
		};

		clocks[i].map = i ? top : infra;
		clocks[i].source_gate = !!i;
		clocks[i].hw.init = &init;
		ret = devm_clk_hw_register(dev, &clocks[i].hw);
		if (ret)
			return ret;
		data->hws[i + 1] = &clocks[i].hw;
	}
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
	if (!ret)
		dev_info(dev, "MSDC0 source/HCLK: 26 MHz oscillator; PLLs retained\n");
	return ret;
}

static const struct of_device_id msdc_lk_ids[] = {
	{ .compatible = "leeco,x620-msdc0-clocks" },
	{ }
};
static struct platform_driver msdc_lk_driver = {
	.probe = msdc_lk_probe,
	.driver = {
		.name = "x620-msdc0-clocks",
		.of_match_table = msdc_lk_ids,
	},
};
module_platform_driver(msdc_lk_driver);
MODULE_DESCRIPTION("X620 isolated MSDC0 bring-up clocks");
MODULE_LICENSE("GPL");
