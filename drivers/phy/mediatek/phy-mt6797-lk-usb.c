// SPDX-License-Identifier: GPL-2.0-only
/* X620 retained-firmware adapter, matching the verified U-Boot USB2 path. */
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

struct x620_usb_gate {
	struct clk_hw hw;
	void __iomem *infra;
	u32 mask;
};

struct x620_usb_phy {
	struct device *dev;
	void __iomem *base;
	struct x620_usb_gate gates[3];
};

static int x620_gate_enable(struct clk_hw *hw)
{
	struct x620_usb_gate *gate = container_of(hw, struct x620_usb_gate, hw);

	writel(gate->mask, gate->infra + 0xac);
	return 0;
}

static void x620_gate_disable(struct clk_hw *hw)
{
	/* Keep firmware clocks running even if a consumer probe fails. */
}

static int x620_gate_is_enabled(struct clk_hw *hw)
{
	struct x620_usb_gate *gate = container_of(hw, struct x620_usb_gate, hw);

	return !(readl(gate->infra + 0xb0) & gate->mask);
}

static const struct clk_ops x620_gate_ops = {
	.enable = x620_gate_enable,
	.disable = x620_gate_disable,
	.is_enabled = x620_gate_is_enabled,
};

static void x620_usb_update(void __iomem *base, u32 offset, u32 mask, u32 value)
{
	writel((readl(base + offset) & ~mask) | value, base + offset);
}

static int x620_usb_power_on(struct phy *phy)
{
	struct x620_usb_phy *p = phy_get_drvdata(phy);
	void __iomem *u2 = p->base + 0x800;

	/* MT6797 reference usb_phy_recover(), as tested by the U-Boot relay. */
	udelay(50);
	x620_usb_update(u2, 0x18, BIT(31) | BIT(23) | 0xf, BIT(20) | 2);
	x620_usb_update(u2, 0x20, BIT(9) | BIT(8), 0);
	x620_usb_update(u2, 0x68, BIT(26) | BIT(23) | BIT(21) | BIT(20) |
			BIT(19) | BIT(18) | BIT(17) | GENMASK(13, 10) |
			BIT(7) | BIT(6) | GENMASK(5, 4) | BIT(2), 0);
	x620_usb_update(u2, 0x6c, BIT(16), 0);
	x620_usb_update(u2, 0x14, 0, BIT(11));
	x620_usb_update(u2, 0x70, GENMASK(19, 16), 9 << 16);
	udelay(800);
	x620_usb_update(u2, 0x6c, BIT(4), BIT(5) | BIT(2));
	dev_info(p->dev, "X620_USB_PHY_READY: retained USB2 recovery complete\n");
	return 0;
}

static const struct phy_ops x620_usb_phy_ops = {
	.power_on = x620_usb_power_on,
	.owner = THIS_MODULE,
};

static int x620_usb_resources_probe(struct platform_device *pdev)
{
	static const u32 masks[] = { BIT(18), BIT(24), BIT(9) };
	static const char * const names[] = { "bus", "sys", "ref" };
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *clocks;
	struct phy_provider *provider;
	struct x620_usb_phy *p;
	void __iomem *infra;
	struct phy *phy;
	int i, ret;

	if (!of_machine_is_compatible("leeco,x620"))
		return -ENODEV;
	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	p->base = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);
	infra = devm_platform_ioremap_resource_byname(pdev, "infracfg");
	if (IS_ERR(infra))
		return PTR_ERR(infra);
	clocks = devm_kzalloc(dev, struct_size(clocks, hws, 3), GFP_KERNEL);
	if (!clocks)
		return -ENOMEM;
	clocks->num = 3;
	for (i = 0; i < clocks->num; i++) {
		struct clk_init_data init = { .ops = &x620_gate_ops };

		init.name = devm_kasprintf(dev, GFP_KERNEL, "%s-%s", dev_name(dev), names[i]);
		if (!init.name)
			return -ENOMEM;
		p->gates[i].infra = infra;
		p->gates[i].mask = masks[i];
		p->gates[i].hw.init = &init;
		ret = devm_clk_hw_register(dev, &p->gates[i].hw);
		if (ret)
			return ret;
		clocks->hws[i] = &p->gates[i].hw;
	}
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clocks);
	if (ret)
		return ret;
	phy = devm_phy_create(dev, NULL, &x620_usb_phy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, p);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(provider))
		return PTR_ERR(provider);
	dev_info(dev, "X620_USB_RESOURCES_READY: LK PLLs/rails retained, gate-only clocks\n");
	return 0;
}

static const struct of_device_id x620_usb_resources_ids[] = {
	{ .compatible = "leeco,x620-lk-usb-resources" },
	{ }
};
static struct platform_driver x620_usb_resources_driver = {
	.probe = x620_usb_resources_probe,
	.driver = {
		.name = "x620-lk-usb-resources",
		.of_match_table = x620_usb_resources_ids,
	},
};
module_platform_driver(x620_usb_resources_driver);
MODULE_DESCRIPTION("X620 LK-retained USB2 PHY and clock gates");
MODULE_LICENSE("GPL");
