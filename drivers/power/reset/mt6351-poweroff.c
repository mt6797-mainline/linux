// SPDX-License-Identifier: GPL-2.0-only
/* MT6351 RTC/BBPU power-down sequence from the MT6797 factory kernel. */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>

#define MT6351_RTC_BBPU		0x4000
#define MT6351_RTC_AL_SEC		0x4018
#define MT6351_RTC_PDN1		0x402c
#define MT6351_RTC_PROT		0x4036
#define MT6351_RTC_WRTGR		0x403c
#define MT6351_RTC_CON		0x403e
#define MT6351_RTC_KEY		0x4300
#define MT6351_RTC_PWREN		BIT(0)
#define MT6351_RTC_AUTO		BIT(3)
#define MT6351_RTC_BUSY		BIT(6)
#define MT6351_RTC_GPIO_USERS	GENMASK(12, 8)
#define MT6351_RTC_F32KOB		BIT(5)
#define MT6351_RTC_2SEC_EN	BIT(8)
#define MT6351_RTC_AUTO_PDN	BIT(6)

struct mt6351_poweroff {
	struct device *dev;
	struct regmap *map;
};

static int mt6351_rtc_commit(struct regmap *map)
{
	unsigned int value;
	int ret;

	ret = regmap_write(map, MT6351_RTC_WRTGR, 1);
	if (ret)
		return ret;
	return regmap_read_poll_timeout_atomic(map, MT6351_RTC_BBPU, value,
					      !(value & MT6351_RTC_BUSY),
					      10, 100000);
}

static int mt6351_power_off(struct sys_off_data *data)
{
	struct mt6351_poweroff *pwrc = data->cb_data;
	struct regmap *map = pwrc->map;
	unsigned int users;
	int ret;

	dev_emerg(pwrc->dev, "power off: factory RTC/BBPU sequence\n");
	ret = regmap_write(map, MT6351_RTC_PROT, 0x586a);
	if (!ret)
		ret = mt6351_rtc_commit(map);
	if (!ret)
		ret = regmap_write(map, MT6351_RTC_PROT, 0x9136);
	if (!ret)
		ret = mt6351_rtc_commit(map);
	/* Preserve alarm seconds; disable the factory two-second reboot logic. */
	if (!ret)
		ret = regmap_update_bits(map, MT6351_RTC_AL_SEC,
					 MT6351_RTC_2SEC_EN | MT6351_RTC_AUTO_PDN, 0);
	if (!ret)
		ret = mt6351_rtc_commit(map);
	if (!ret)
		ret = regmap_read(map, MT6351_RTC_PDN1, &users);
	if (!ret && !(users & MT6351_RTC_GPIO_USERS)) {
		ret = regmap_update_bits(map, MT6351_RTC_CON,
					 MT6351_RTC_F32KOB, MT6351_RTC_F32KOB);
		if (!ret)
			ret = mt6351_rtc_commit(map);
	}
	if (!ret)
		ret = regmap_write(map, MT6351_RTC_BBPU,
			MT6351_RTC_KEY | MT6351_RTC_AUTO | MT6351_RTC_PWREN);
	if (!ret)
		ret = mt6351_rtc_commit(map);
	if (ret)
		dev_emerg(pwrc->dev, "RTC power-down failed: %d\n", ret);
	else {
		mdelay(1000);
		dev_emerg(pwrc->dev, "PMIC did not cut power; falling back to firmware\n");
	}
	return NOTIFY_DONE;
}

static int mt6351_poweroff_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev, *parent;
	struct mt6351_poweroff *pwrc;
	unsigned int bbpu, spar0;
	int ret;

	pwrc = devm_kzalloc(dev, sizeof(*pwrc), GFP_KERNEL);
	if (!pwrc)
		return -ENOMEM;
	pwrc->dev = dev;
	for (parent = dev->parent; parent && !pwrc->map; parent = parent->parent)
		pwrc->map = dev_get_regmap(parent, NULL);
	if (!pwrc->map)
		return -EPROBE_DEFER;
	ret = regmap_read(pwrc->map, MT6351_RTC_BBPU, &bbpu);
	if (!ret)
		ret = regmap_read(pwrc->map, 0x4030, &spar0);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read RTC power state\n");
	/* The stock GPIO148 workaround is only used without an external 32k crystal. */
	if (!(spar0 & BIT(6)))
		return dev_err_probe(dev, -ENODEV, "32k-less power-down is not implemented\n");
	ret = devm_register_sys_off_handler(dev, SYS_OFF_MODE_POWER_OFF,
					   SYS_OFF_PRIO_HIGH, mt6351_power_off, pwrc);
	if (!ret)
		dev_info(dev, "RTC/BBPU power-off ready: BBPU=%04x SPAR0=%04x\n",
			 bbpu, spar0);
	return ret;
}

static const struct of_device_id mt6351_poweroff_match[] = {
	{ .compatible = "mediatek,mt6351-poweroff" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6351_poweroff_match);

static struct platform_driver mt6351_poweroff_driver = {
	.probe = mt6351_poweroff_probe,
	.driver = {
		.name = "mt6351-poweroff",
		.of_match_table = mt6351_poweroff_match,
	},
};
module_platform_driver(mt6351_poweroff_driver);

MODULE_DESCRIPTION("MT6351 RTC/BBPU power-off");
MODULE_LICENSE("GPL");
