// SPDX-License-Identifier: GPL-2.0-only
/* Read firmware's MM parent; native MMSYS gates and MIPI TX own the display. */
#include <linux/clk-provider.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

static int mt6797_display_clock_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *top, *apmixed;
	struct clk_hw *hw;
	u32 mux, con0, pcw, pll, divisor = 1;
	u64 rate = 26000000;
	int ret;

	top = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,topckgen");
	if (IS_ERR(top))
		return PTR_ERR(top);
	apmixed = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,apmixedsys");
	if (IS_ERR(apmixed))
		return PTR_ERR(apmixed);
	ret = regmap_read(top, 0x40, &mux);
	if (ret)
		return ret;
	mux = (mux >> 24) & 3;
	if (mux) {
		/* clk-mt6797.c: imgpll, univpll1_d2, syspll1_d2. */
		pll = mux == 1 ? 0x260 : mux == 2 ? 0x230 : 0x220;
		ret = regmap_read(apmixed, pll, &con0);
		if (ret)
			return ret;
		ret = regmap_read(apmixed, pll + 4, &pcw);
		if (ret)
			return ret;
		if (!(con0 & BIT(0)))
			return dev_err_probe(dev, -EINVAL, "MM parent PLL is not enabled\n");
		if (mux == 2)
			rate = 26000000ULL * ((pcw >> 14) & 0x7f);
		else
			rate = DIV_ROUND_UP_ULL(26000000ULL * (pcw & GENMASK(20, 0)), BIT(14));
		divisor = BIT((con0 >> 4) & 7) * (mux == 1 ? 1 : 4);
		rate = DIV_ROUND_UP_ULL(rate, divisor);
	}
	hw = devm_clk_hw_register_fixed_rate(dev, "mm_sel", NULL, 0, rate);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, hw);
	if (!ret)
		dev_info(dev, "retained MM parent %u: %llu Hz; no mux/PLL writes\n", mux, rate);
	return ret;
}

static const struct of_device_id mt6797_display_clock_ids[] = {
	{ .compatible = "leeco,x620-display-clock" },
	{ }
};
static struct platform_driver mt6797_display_clock_driver = {
	.probe = mt6797_display_clock_probe,
	.driver = {
		.name = "x620-display-clock",
		.of_match_table = mt6797_display_clock_ids,
	},
};
module_platform_driver(mt6797_display_clock_driver);
MODULE_DESCRIPTION("X620 firmware-retained MM clock parent");
MODULE_LICENSE("GPL");
