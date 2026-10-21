// SPDX-License-Identifier: GPL-2.0-only
/* Isolated MT6797 MSDC0 clocks; other PLLs remain firmware-owned. */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "clk-pll.h"

#define CLK_CFG_7 0x70
#define MSDC0_SOURCE_GATE BIT(23)

struct msdc_lk_clock {
	struct clk_hw hw;
	struct regmap *map;
	bool source_gate;
	unsigned long rate;
};

/* Same register layout as the upstream MT6797 MSDCPLL descriptor. */
static const struct mtk_pll_data msdc_lk_pll = {
	.id = 0,
	.name = "mt6797-lk-msdcpll",
	.reg = 0x250,
	.pwr_reg = 0x25c,
	.en_mask = 0x120,
	.fmax = 3000000000UL,
	.pcwbits = 21,
	.pd_reg = 0x250,
	.pd_shift = 4,
	.pcw_reg = 0x254,
};

static void msdc_lk_unregister_pll(void *data)
{
	mtk_clk_unregister_plls(&msdc_lk_pll, 1, data);
}

static int msdc_lk_high_speed(struct device *dev, struct clk_hw **source,
			      unsigned long *hclk_rate)
{
	struct clk_hw_onecell_data *pll_data;
	struct device_node *node;
	struct regmap *apmixed;
	struct clk *pll;
	u32 con0, con1;
	u64 rate;
	int ret;

	apmixed = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,apmixedsys");
	if (IS_ERR(apmixed))
		return PTR_ERR(apmixed);
	/* HCLK selector 1 is syspll1_d2 = firmware MAINPLL / 4. */
	ret = regmap_read(apmixed, 0x220, &con0);
	if (ret)
		return ret;
	ret = regmap_read(apmixed, 0x224, &con1);
	if (ret)
		return ret;
	if (!(con0 & BIT(0)) || !(con1 & GENMASK(20, 0)))
		return dev_err_probe(dev, -EINVAL, "firmware MAINPLL is not running\n");
	rate = DIV_ROUND_UP_ULL(26000000ULL * (con1 & GENMASK(20, 0)), BIT(14));
	rate = DIV_ROUND_UP_ULL(rate, BIT((con0 >> 4) & 7));
	*hclk_rate = rate / 4;

	pll_data = devm_kzalloc(dev, struct_size(pll_data, hws, 1), GFP_KERNEL);
	if (!pll_data)
		return -ENOMEM;
	pll_data->num = 1;
	node = of_parse_phandle(dev->of_node, "mediatek,apmixedsys", 0);
	if (!node)
		return -EINVAL;
	ret = mtk_clk_register_plls(node, &msdc_lk_pll, 1, pll_data);
	of_node_put(node);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, msdc_lk_unregister_pll, pll_data);
	if (ret)
		return ret;
	pll = devm_clk_hw_get_clk(dev, pll_data->hws[0], "source");
	if (IS_ERR(pll))
		return PTR_ERR(pll);
	ret = clk_set_rate(pll, 400000000);
	if (ret)
		return ret;
	*source = pll_data->hws[0];
	return 0;
}

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
	struct msdc_lk_clock *c = container_of(hw, struct msdc_lk_clock, hw);

	return c->source_gate ? parent_rate : c->rate;
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
	unsigned long hclk_rate = 26000000;
	bool high_speed = of_property_read_bool(dev->of_node, "mediatek,high-speed");
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
	data = devm_kzalloc(dev, struct_size(data, hws, 3), GFP_KERNEL);
	clocks = devm_kcalloc(dev, 2, sizeof(*clocks), GFP_KERNEL);
	if (!data || !clocks)
		return -ENOMEM;
	data->num = 3;
	if (high_speed) {
		ret = msdc_lk_high_speed(dev, &data->hws[0], &hclk_rate);
		if (ret)
			return ret;
	} else {
		data->hws[0] = devm_clk_hw_register_fixed_rate(dev, "mt6797-lk-msdc0-source",
							    NULL, 0, 26000000);
		if (IS_ERR(data->hws[0]))
			return PTR_ERR(data->hws[0]);
	}
	ret = regmap_update_bits(top, CLK_CFG_7, GENMASK(9, 8) | GENMASK(19, 16),
				high_speed ? BIT(8) | BIT(16) : 0);
	if (ret)
		return ret;
	for (i = 0; i < 2; i++) {
		struct clk_init_data init = {
			.name = i ? "mt6797-lk-msdc0-source-gate" : "mt6797-lk-msdc0-hclk",
			.ops = &msdc_lk_ops,
		};
		const struct clk_hw *parent = data->hws[0];

		if (i) {
			init.parent_hws = &parent;
			init.num_parents = 1;
		}
		clocks[i].map = i ? top : infra;
		clocks[i].source_gate = !!i;
		clocks[i].rate = hclk_rate;
		clocks[i].hw.init = &init;
		ret = devm_clk_hw_register(dev, &clocks[i].hw);
		if (ret)
			return ret;
		data->hws[i + 1] = &clocks[i].hw;
	}
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
	if (!ret)
		dev_info(dev, "MSDC0 source=%lu Hz HCLK=%lu Hz; %s\n",
			 clk_hw_get_rate(data->hws[0]), hclk_rate,
			 high_speed ? "MSDCPLL configured; MAINPLL retained" : "PLLs retained");
	return ret;
}

static const struct of_device_id msdc_lk_ids[] = {
	{ .compatible = "mediatek,mt6797-lk-msdc0-clocks" },
	{ .compatible = "leeco,x620-msdc0-clocks" },
	{ }
};
static struct platform_driver msdc_lk_driver = {
	.probe = msdc_lk_probe,
	.driver = {
		.name = "mt6797-lk-msdc0-clocks",
		.of_match_table = msdc_lk_ids,
	},
};
module_platform_driver(msdc_lk_driver);
MODULE_DESCRIPTION("MT6797 isolated MSDC0 bring-up clocks");
MODULE_LICENSE("GPL");
