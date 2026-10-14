// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 AUXADC thermal banks and factory calibration, from the stock driver. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of_address.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/thermal.h>
#include "../thermal_hwmon.h"

#define NUM_BANKS 6
#define CORESEL 0x400
#define MONCTL0 0x000
#define MONINT 0x00c
#define MSR0 0x090
#define MSR1 0x094

struct mt6797_thermal;
struct mt6797_sensor {
	struct mt6797_thermal *thermal;
	unsigned int bank;
};
struct mt6797_thermal {
	void __iomem *base, *adc;
	struct regmap *apmixed, *infra;
	struct clk_bulk_data clocks[2];
	struct mutex lock;
	struct mt6797_sensor sensors[NUM_BANKS + 1];
	int gain, oe, degc, slope, room[4];
};

/* BIG: MCU1, GPU: MCU4, SOC: MCU2/3, L/LL/CCI: MCU2. */
static const u8 bank_sensor[NUM_BANKS][2] = {
	{ 0, 0 }, { 3, 3 }, { 1, 2 }, { 1, 1 }, { 1, 1 }, { 1, 1 },
};

static int mt6797_thermal_calibrate(struct device *dev, struct mt6797_thermal *t)
{
	struct nvmem_cell *cell;
	__le32 *buf;
	size_t len;
	u32 cal, adc, tail;
	int ge, vts[4], slope, i;

	cell = devm_nvmem_cell_get(dev, "calibration");
	if (IS_ERR(cell))
		return PTR_ERR(cell);
	buf = nvmem_cell_read(cell, &len);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	if (len != 12) {
		kfree(buf);
		return -EINVAL;
	}
	cal = le32_to_cpu(buf[0]);
	adc = le32_to_cpu(buf[1]);
	tail = le32_to_cpu(buf[2]);
	kfree(buf);
	ge = (adc >> 22) & 0x3ff;
	t->oe = (adc >> 12) & 0x3ff;
	t->degc = (cal >> 1) & 0x3f;
	slope = (adc & BIT(9)) ? (cal >> 26) : 0;
	t->slope = 1663 + ((cal & BIT(7)) ? -slope : slope) * 10;
	vts[0] = (cal >> 17) & 0x1ff;
	vts[1] = (cal >> 8) & 0x1ff;
	vts[2] = adc & 0x1ff;
	vts[3] = (tail >> 23) & 0x1ff;
	if (!(cal & BIT(0))) {
		dev_warn(dev, "uncalibrated chip; using stock thermal defaults\n");
		ge = t->oe = 512;
		t->degc = 40;
		t->slope = 1663;
		for (i = 0; i < 4; i++)
			vts[i] = 260;
	}
	/* Stock eDataCorrector bounds. Do not use another SoC's coefficient. */
	if (ge < 265 || ge > 758)
		ge = 512;
	if (t->oe < 265 || t->oe > 758)
		t->oe = 512;
	if (!t->degc)
		t->degc = 40;
	t->gain = 10000 + (ge - 512) * 10000 / 4096;
	t->oe -= 512;
	for (i = 0; i < 4; i++) {
		if (vts[i] > 484)
			vts[i] = 260;
		t->room[i] = ((vts[i] + 3350 - t->oe) * 10000 / 4096) * 10000 / t->gain;
	}
	dev_info(dev, "factory calibration %08x %08x %08x: gain=%d offset=%d slope=%d\n",
		 cal, adc, tail, t->gain, t->oe, t->slope);
	return 0;
}

static int mt6797_raw_to_temp(struct mt6797_thermal *t, unsigned int raw, int sensor)
{
	int delta;

	delta = ((((int)raw - t->oe) * 10000 >> 12) * 10000 / t->gain) - t->room[sensor];
	delta = delta * 15 / 18;
	return (t->degc * 10 / 2 - delta * 1000 / t->slope) * 100;
}

static void mt6797_select_bank(struct mt6797_thermal *t, int bank)
{
	u32 sel = readl(t->base + CORESEL);

	writel((sel & ~0xf) | bank, t->base + CORESEL);
}

static int mt6797_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct mt6797_sensor *s = thermal_zone_device_priv(tz);
	struct mt6797_thermal *t = s->thermal;
	int bank, point, start, end, value, result = INT_MIN, ret = 0;
	u32 saved, raw;

	start = s->bank == NUM_BANKS ? 0 : s->bank;
	end = s->bank == NUM_BANKS ? NUM_BANKS : start + 1;
	mutex_lock(&t->lock);
	saved = readl(t->base + CORESEL);
	for (bank = start; bank < end; bank++) {
		mt6797_select_bank(t, bank);
		for (point = 0; point < (bank == 2 ? 2 : 1); point++) {
			raw = readl(t->base + (point ? MSR1 : MSR0)) & 0xfff;
			if (!raw) {
				ret = -EAGAIN;
				goto restore;
			}
			value = mt6797_raw_to_temp(t, raw, bank_sensor[bank][point]);
			if (value < -30000 || value > 150000) {
				ret = -ERANGE;
				goto restore;
			}
			result = max(result, value);
		}
	}
	*temp = result;
restore:
	writel(saved, t->base + CORESEL);
	mutex_unlock(&t->lock);
	return ret;
}
static const struct thermal_zone_device_ops mt6797_thermal_ops = {
	.get_temp = mt6797_thermal_get_temp,
};

/* Stock dummy conversion primes every bank before using real ADC data. */
static int mt6797_thermal_fast_init(struct mt6797_thermal *t, resource_size_t phys)
{
	int bank, ret;
	u32 value;

	writel(0x1da1, t->base + 0x428);
	for (bank = 0; bank < NUM_BANKS; bank++) {
		mt6797_select_bank(t, bank);
		writel(0, t->base + MONCTL0);
		writel(0, t->base + MONINT);
		writel(1, t->base + 0x004);
		writel(1, t->base + 0x008);
		writel(1, t->base + 0x040);
		writel(0xff, t->base + 0x044);
		writel(0, t->base + 0x014);
		writel(0, t->base + 0x018);
		writel(0, t->base + 0x038);
		writel(1, t->base + 0x048);
		writel(2, t->base + 0x04c);
		writel(3, t->base + 0x050);
		writel(4, t->base + 0x0b4);
		writel(phys + 0x420, t->base + 0x064);
		writel(phys + 0x420, t->base + 0x068);
		writel(phys + 0x424, t->base + 0x074);
		writel(phys + 0x428, t->base + 0x078);
		writel(phys + 0x428, t->base + 0x07c);
		writel(0, t->base + 0x080);
		writel(0x2c, t->base + 0x084);
		writel(0, t->base + 0x088);
		writel(3, t->base + 0x08c);
		writel(0xf, t->base + MONCTL0);
		ret = readl_poll_timeout(t->base + MSR0, value,
			(value & 0xfff) == 0xda1, 2, 1000);
		writel(0, t->base + MONCTL0);
		if (ret)
			return ret;
	}
	return readl_poll_timeout(t->base + 0x418, value,
				 !(value >> 16), 2, 1000);
}

static int mt6797_thermal_start(struct mt6797_thermal *t,
			       resource_size_t adc_phys, resource_size_t apmixed_phys,
			       resource_size_t thermal_phys)
{
	int bank, ret;
	u32 val;

	/* Stock infra reset bit 0, then TS_CON1 buffer on before sensing. */
	ret = regmap_write(t->infra, 0x120, BIT(0));
	if (ret)
		return ret;
	ret = regmap_write(t->infra, 0x124, BIT(0));
	if (ret)
		return ret;
	ret = regmap_update_bits(t->apmixed, 0x604, 0x30, 0);
	if (ret)
		return ret;
	udelay(200);
	ret = mt6797_thermal_fast_init(t, thermal_phys);
	if (ret)
		return ret;
	val = readl(t->adc);
	writel(val & ~BIT(11), t->adc);
	writel(BIT(11), t->adc + 0xc);
	for (bank = 0; bank < NUM_BANKS; bank++) {
		mt6797_select_bank(t, bank);
		writel(0, t->base + MONCTL0);
		writel(0, t->base + MONINT);
		writel(0xc, t->base + 0x004);
		writel(bank == 0 ? 0x0001002e : 0x000101ad, t->base + 0x008);
		writel(0, t->base + 0x014);
		writel(0, t->base + 0x018);
		writel(0x492, t->base + 0x038); /* stock 2 of 4 sample filter */
		writel(0, t->base + 0x03c);
		writel(0x30d, t->base + 0x040);
		writel(0xffffffff, t->base + 0x044);
		writel(bank_sensor[bank][0], t->base + 0x048);
		if (bank == 2)
			writel(bank_sensor[bank][1], t->base + 0x04c);
		writel(BIT(11), t->base + 0x054);
		writel(BIT(11), t->base + 0x060);
		writel(apmixed_phys + 0x604, t->base + 0x064);
		writel(adc_phys + 0xc, t->base + 0x068);
		writel(adc_phys + 0x8, t->base + 0x074);
		writel(adc_phys + 0x40, t->base + 0x078);
		writel(adc_phys + 0x40, t->base + 0x07c);
		writel(0, t->base + 0x080);
		writel(0x2c, t->base + 0x084);
		writel(0, t->base + 0x088);
		writel(3, t->base + 0x08c);
	}
	writel(BIT(11), t->adc + 0x8);
	for (bank = 0; bank < NUM_BANKS; bank++) {
		mt6797_select_bank(t, bank);
		writel(bank == 2 ? 3 : 1, t->base + MONCTL0);
	}
	return 0;
}

static void mt6797_thermal_stop(void *data)
{
	struct mt6797_thermal *t = data;
	int bank;

	for (bank = 0; bank < NUM_BANKS; bank++) {
		mt6797_select_bank(t, bank);
		writel(0, t->base + MONCTL0);
	}
	clk_bulk_disable_unprepare(ARRAY_SIZE(t->clocks), t->clocks);
}

static int mt6797_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct thermal_zone_device *tz;
	struct mt6797_thermal *t;
	struct device_node *np;
	struct resource adc, apmixed;
	int i, ret;

	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	mutex_init(&t->lock);
	t->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(t->base))
		return PTR_ERR(t->base);
	np = of_parse_phandle(dev->of_node, "mediatek,auxadc", 0);
	if (!np)
		return -EINVAL;
	ret = of_address_to_resource(np, 0, &adc);
	of_node_put(np);
	if (ret)
		return ret;
	np = of_parse_phandle(dev->of_node, "mediatek,apmixedsys", 0);
	if (!np)
		return -EINVAL;
	ret = of_address_to_resource(np, 0, &apmixed);
	of_node_put(np);
	if (ret)
		return ret;
	t->adc = devm_ioremap(dev, adc.start, resource_size(&adc));
	if (!t->adc)
		return -ENOMEM;
	t->apmixed = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,apmixedsys");
	if (IS_ERR(t->apmixed))
		return PTR_ERR(t->apmixed);
	t->infra = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(t->infra))
		return PTR_ERR(t->infra);
	ret = mt6797_thermal_calibrate(dev, t);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get factory calibration\n");
	t->clocks[0].id = "therm";
	t->clocks[1].id = "auxadc";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(t->clocks), t->clocks);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(t->clocks), t->clocks);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, mt6797_thermal_stop, t);
	if (ret)
		return ret;
	ret = mt6797_thermal_start(t, adc.start, apmixed.start,
			    platform_get_resource(pdev, IORESOURCE_MEM, 0)->start);
	if (ret)
		return ret;
	/* First filtered conversion takes several stock sensing intervals. */
	msleep(100);
	for (i = 0; i <= NUM_BANKS; i++) {
		t->sensors[i].thermal = t;
		t->sensors[i].bank = i;
		tz = devm_thermal_of_zone_register(dev, i, &t->sensors[i], &mt6797_thermal_ops);
		if (IS_ERR(tz)) {
			if (PTR_ERR(tz) == -ENODEV)
				continue;
			return PTR_ERR(tz);
		}
		devm_thermal_add_hwmon_sysfs(dev, tz);
	}
	dev_info(dev, "six stock thermal banks ready; factory-calibrated AUXADC\n");
	return 0;
}
static const struct of_device_id mt6797_thermal_ids[] = {
	{ .compatible = "mediatek,mt6797-thermal" }, { }
};
MODULE_DEVICE_TABLE(of, mt6797_thermal_ids);
static struct platform_driver mt6797_thermal_driver = {
	.probe = mt6797_thermal_probe,
	.driver = { .name = "mt6797-thermal", .of_match_table = mt6797_thermal_ids },
};
module_platform_driver(mt6797_thermal_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6797 factory-calibrated AUXADC thermal controller");
