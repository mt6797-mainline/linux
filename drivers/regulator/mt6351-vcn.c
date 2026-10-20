// SPDX-License-Identifier: GPL-2.0-only
/* Register fields and voltage tables from the MT6797 MT6351 vendor driver. */
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/mt6351-vcn.h>

static DEFINE_MUTEX(mt6351_vcn_lock);
static struct regmap *mt6351_vcn_map;

int mt6351_vcn_set_hw_control(unsigned int reg, bool enable)
{
	int ret;

	if (reg != MT6351_VCN18_CON && reg != MT6351_VCN28_CON &&
	    reg != MT6351_VCN33_BT_CON && reg != MT6351_VCN33_WIFI_CON)
		return -EINVAL;
	mutex_lock(&mt6351_vcn_lock);
	ret = mt6351_vcn_map ? regmap_update_bits(mt6351_vcn_map, reg,
						BIT(3), enable ? BIT(3) : 0) : -ENODEV;
	mutex_unlock(&mt6351_vcn_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6351_vcn_set_hw_control);

static const struct regulator_ops mt6351_vcn_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
};

static const unsigned int vcn18_volts[] = {
	900000, 950000, 1000000, 1050000, 1200000, 1500000, 1800000,
};
static const unsigned int vcn28_volts[] = {
	1800000, 2200000, 2375000, 2800000,
};
static const unsigned int vcn33_volts[] = {
	3300000, 3400000, 3500000, 3600000,
};

#define VCN_DESC(_id, _name, _enable, _vsel, _mask, _table) \
	{ .id = _id, .name = _name, .of_match = "ldo_" _name, \
	  .regulators_node = "regulators", .type = REGULATOR_VOLTAGE, \
	  .owner = THIS_MODULE, .ops = &mt6351_vcn_ops, \
	  .enable_reg = _enable, .enable_mask = BIT(1), \
	  .vsel_reg = _vsel, .vsel_mask = _mask, \
	  .volt_table = _table, .n_voltages = ARRAY_SIZE(_table) }

static const struct regulator_desc mt6351_vcn_descs[] = {
	VCN_DESC(0, "vcn18", MT6351_VCN18_CON, 0x0b18, GENMASK(10, 8), vcn18_volts),
	VCN_DESC(1, "vcn28", MT6351_VCN28_CON, 0x0acc, GENMASK(9, 8), vcn28_volts),
	VCN_DESC(2, "vcn33_bt", MT6351_VCN33_BT_CON, 0x0ada, GENMASK(10, 9), vcn33_volts),
	VCN_DESC(3, "vcn33_wifi", MT6351_VCN33_WIFI_CON, 0x0ada, GENMASK(10, 9), vcn33_volts),
};

static int mt6351_vcn_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regulator_config config = { .dev = dev };
	struct regmap *map = dev_get_regmap(dev->parent, NULL);
	struct regulator_dev *rdev;
	unsigned int cid;
	int i, ret;

	if (!map)
		return -EPROBE_DEFER;
	ret = regmap_read(map, 0x0202, &cid);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read MT6351 SWCID\n");
	if (!cid || cid == 0xffff)
		return dev_err_probe(dev, -ENODEV, "invalid MT6351 SWCID: %04x\n", cid);
	config.regmap = map;
	config.of_node = dev->of_node;
	for (i = 0; i < ARRAY_SIZE(mt6351_vcn_descs); i++) {
		rdev = devm_regulator_register(dev, &mt6351_vcn_descs[i], &config);
		if (IS_ERR(rdev))
			return dev_err_probe(dev, PTR_ERR(rdev), "register %s\n",
					     mt6351_vcn_descs[i].name);
	}
	ret = devm_of_platform_populate(dev);
	if (ret)
		return ret;
	mutex_lock(&mt6351_vcn_lock);
	if (mt6351_vcn_map) {
		mutex_unlock(&mt6351_vcn_lock);
		return -EBUSY;
	}
	mt6351_vcn_map = map;
	mutex_unlock(&mt6351_vcn_lock);
	platform_set_drvdata(pdev, map);
	dev_info(dev, "MT6351 SWCID=%04x; VCN18/28/33 supplies registered\n", cid);
	return 0;
}

static void mt6351_vcn_remove(struct platform_device *pdev)
{
	mutex_lock(&mt6351_vcn_lock);
	if (mt6351_vcn_map == platform_get_drvdata(pdev))
		mt6351_vcn_map = NULL;
	mutex_unlock(&mt6351_vcn_lock);
}

static const struct of_device_id mt6351_vcn_of_match[] = {
	{ .compatible = "mediatek,mt6351" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6351_vcn_of_match);

static struct platform_driver mt6351_vcn_driver = {
	.probe = mt6351_vcn_probe,
	.remove = mt6351_vcn_remove,
	.driver = {
		.name = "mt6351-vcn",
		.of_match_table = mt6351_vcn_of_match,
	},
};
module_platform_driver(mt6351_vcn_driver);
MODULE_DESCRIPTION("MT6351 connectivity regulators");
MODULE_LICENSE("GPL");
