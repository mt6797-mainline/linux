// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 firmware AXI parent and shared peripheral gates. */
#include <linux/clk-provider.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <dt-bindings/clock/mt6797-clk.h>

#include "clk-gate.h"

static const struct mtk_gate_regs mt6797_infra0_regs = {
	.set_ofs = 0x80, .clr_ofs = 0x84, .sta_ofs = 0x90,
};

static const struct mtk_gate_regs mt6797_infra1_regs = {
	.set_ofs = 0x88, .clr_ofs = 0x8c, .sta_ofs = 0x94,
};

static const struct mtk_gate_regs mt6797_infra2_regs = {
	.set_ofs = 0xa8, .clr_ofs = 0xac, .sta_ofs = 0xb0,
};

static const struct mtk_gate mt6797_infra_lk_gates[] = {
	GATE_MTK_FLAGS(CLK_INFRA_THERM, "mt6797-infra-therm", "mt6797-lk-axi",
		       &mt6797_infra0_regs, 10, &mtk_clk_gate_ops_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(CLK_INFRA_AUXADC, "mt6797-infra-auxadc", "clk26m",
		       &mt6797_infra1_regs, 10, &mtk_clk_gate_ops_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(CLK_INFRA_I2C_APPM, "mt6797-infra-i2c-appm", "mt6797-lk-axi",
		       &mt6797_infra2_regs, 1, &mtk_clk_gate_ops_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(CLK_INFRA_BTIF, "mt6797-infra-btif", "mt6797-lk-axi",
		       &mt6797_infra0_regs, 31, &mtk_clk_gate_ops_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(CLK_INFRA_AP_DMA, "mt6797-infra-ap-dma", "mt6797-lk-axi",
		       &mt6797_infra1_regs, 18, &mtk_clk_gate_ops_setclr,
		       CLK_IGNORE_UNUSED),
	GATE_MTK_FLAGS(CLK_INFRA_I2C4, "mt6797-infra-i2c4", "mt6797-lk-axi",
		       &mt6797_infra2_regs, 0, &mtk_clk_gate_ops_setclr,
		       CLK_IGNORE_UNUSED),
};

static void mt6797_infra_unregister_gates(void *data)
{
	mtk_clk_unregister_gates(mt6797_infra_lk_gates,
				 ARRAY_SIZE(mt6797_infra_lk_gates), data);
}

static int mt6797_infra_lk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *data;
	struct regmap *top, *apmixed;
	struct device_node *infra;
	struct clk_hw *parent;
	u32 mux, con0, pcw, divisor;
	u64 rate = 26000000;
	int i, ret;

	top = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,topckgen");
	if (IS_ERR(top))
		return PTR_ERR(top);
	apmixed = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,apmixedsys");
	if (IS_ERR(apmixed))
		return PTR_ERR(apmixed);
	ret = regmap_read(top, 0x40, &mux);
	if (ret)
		return ret;

	/* clk-mt6797: AXI selects 26M, MAINPLL/7 or the ULPOSC mux.
	 * That mux selects MAINPLL/8 when bit 2 is clear. No oscillator
	 * frequency is guessed if firmware selected the calibrated ULPOSC.
	 */
	if ((mux & 3) == 3 || ((mux & 3) == 2 && (mux & BIT(2))))
		return dev_err_probe(dev, -EINVAL,
				     "unsupported retained AXI oscillator selection: %08x\n", mux);
	if (mux & 3) {
		ret = regmap_read(apmixed, 0x220, &con0);
		if (ret)
			return ret;
		ret = regmap_read(apmixed, 0x224, &pcw);
		if (ret)
			return ret;
		if (!(con0 & BIT(0)) || !(pcw & GENMASK(20, 0)))
			return dev_err_probe(dev, -EINVAL, "firmware MAINPLL is not running\n");
		divisor = BIT((con0 >> 4) & 7) * ((mux & 3) == 1 ? 7 : 8);
		rate = DIV_ROUND_UP_ULL(26000000ULL * (pcw & GENMASK(20, 0)),
					BIT(14) * divisor);
	}
	parent = devm_clk_hw_register_fixed_rate(dev, "mt6797-lk-axi", NULL, 0, rate);
	if (IS_ERR(parent))
		return PTR_ERR(parent);
	data = devm_kzalloc(dev, struct_size(data, hws, CLK_INFRA_NR), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->num = CLK_INFRA_NR;
	for (i = 0; i < data->num; i++)
		data->hws[i] = ERR_PTR(-ENOENT);
	infra = of_parse_phandle(dev->of_node, "mediatek,infracfg", 0);
	if (!infra)
		return -EINVAL;
	ret = mtk_clk_register_gates(dev, infra, mt6797_infra_lk_gates,
				     ARRAY_SIZE(mt6797_infra_lk_gates), data);
	of_node_put(infra);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, mt6797_infra_unregister_gates, data);
	if (ret)
		return ret;
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
	if (ret)
		return ret;
	dev_info(dev, "retained AXI parent %u: %llu Hz; shared I2C4/AP DMA/BTIF gates\n",
		 mux & 3, rate);
	return 0;
}

static const struct of_device_id mt6797_infra_lk_ids[] = {
	{ .compatible = "mediatek,mt6797-infracfg-retained" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_infra_lk_ids);

static struct platform_driver mt6797_infra_lk_driver = {
	.probe = mt6797_infra_lk_probe,
	.driver = {
		.name = "mt6797-infra-lk",
		.of_match_table = mt6797_infra_lk_ids,
	},
};
module_platform_driver(mt6797_infra_lk_driver);
MODULE_DESCRIPTION("MT6797 firmware-retained AXI clock and peripheral gates");
MODULE_LICENSE("GPL");
