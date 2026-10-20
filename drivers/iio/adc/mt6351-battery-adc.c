// SPDX-License-Identifier: GPL-2.0-only
/* MT6351 register sequence and conversion constants from the MT6797 BSP. */
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define FG_CON0		0x0ca4
#define FG_CURRENT	0x0cba
#define FG_OSR		0x0cc2
#define FG_READY	BIT(10)
#define ADC_REQUEST	0x0e98

struct mt6351_battery_adc {
	struct regmap *map;
	struct mutex lock;
	u32 sense_mohm;
	u32 car_tune;
	u32 pullup_ohm;
	u32 pullup_mv;
};

/* Live X620 stock DT, batt_temperature_table; resistance in ohms. */
static const int mt6351_ntc_ohm[] = {
	68237, 53650, 42506, 33892, 27219, 22021, 17926, 14674,
	12081, 10000, 8315, 6948, 5834, 4917, 4161, 3535, 3014,
};

static int mt6351_ntc_temperature(struct mt6351_battery_adc *adc, int mv,
				int current_ua, int *temp)
{
	s64 resistance;
	int i;

	/* Stock common-ground compensation: signed charging current lowers BATON. */
	mv -= div_s64((s64)current_ua * adc->sense_mohm, 1000000);
	/* Open/short NTC is an error, never a fabricated room temperature. */
	if (mv <= 0 || mv >= adc->pullup_mv)
		return -ERANGE;
	resistance = div_s64((s64)adc->pullup_ohm * mv, adc->pullup_mv - mv);
	if (resistance >= mt6351_ntc_ohm[0]) {
		*temp = -20000;
		return 0;
	}
	for (i = 1; i < ARRAY_SIZE(mt6351_ntc_ohm); i++) {
		if (resistance >= mt6351_ntc_ohm[i]) {
			*temp = -20000 + (i - 1) * 5000 +
				div_s64((mt6351_ntc_ohm[i - 1] - resistance) * 5000,
					mt6351_ntc_ohm[i - 1] - mt6351_ntc_ohm[i]);
			return 0;
		}
	}
	*temp = 60000;
	return 0;
}

static int mt6351_current_raw(struct mt6351_battery_adc *adc, int *value)
{
	unsigned int raw, status;
	int ret, clear_ret;

	ret = regmap_update_bits(adc->map, FG_CON0, 0xff00, 0x0200);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(adc->map, FG_CON0, status,
				       status & FG_READY, 50, 10000);
	if (!ret)
		ret = regmap_read(adc->map, FG_CURRENT, &raw);
	clear_ret = regmap_update_bits(adc->map, FG_CON0, 0xff00, 0x0800);
	if (!clear_ret)
		clear_ret = regmap_read_poll_timeout(adc->map, FG_CON0, status,
					     !(status & FG_READY), 50, 10000);
	if (!clear_ret)
		clear_ret = regmap_update_bits(adc->map, FG_CON0, 0xff00, 0);
	else
		regmap_update_bits(adc->map, FG_CON0, 0xff00, 0);
	if (ret || clear_ret)
		return ret ?: clear_ret;

	/* Hardware is one's complement; positive means current INTO the cell. */
	*value = raw > 0x7fff ? (int)raw - 65535 : raw;
	return 0;
}

static int mt6351_current(struct mt6351_battery_adc *adc, int *ua)
{
	int raw, ret = mt6351_current_raw(adc, &raw);

	if (ret)
		return ret;
	*ua = div_s64((s64)raw * 158122 * 20 * adc->car_tune,
		      (s64)100000 * adc->sense_mohm);
	return 0;
}

static int mt6351_voltage(struct mt6351_battery_adc *adc, int channel, int *mv)
{
	unsigned int raw;
	int ret;

	ret = regmap_write(adc->map, ADC_REQUEST, BIT(channel));
	if (ret)
		return ret;
	usleep_range(50, 100);
	/* AP BATSNS is ADC23, whereas BATON uses ADC3. */
	ret = regmap_read_poll_timeout(adc->map, channel ? 0x0e06 : 0x0e2e,
				       raw, raw & BIT(15), 1300, 25000);
	if (ret)
		return ret;
	if (channel)
		*mv = (raw & 0xfff) * 2 * 1800 / 4096;
	else
		*mv = (raw & 0x7fff) * 3 * 1800 / 32768;
	return 0;
}

static int mt6351_battery_read_raw(struct iio_dev *indio_dev,
				 const struct iio_chan_spec *chan,
				 int *val, int *val2, long mask)
{
	struct mt6351_battery_adc *adc = iio_priv(indio_dev);
	int battery_current, mv, ret;

	if (mask == IIO_CHAN_INFO_SCALE && chan->type == IIO_CURRENT) {
		/* mA per raw count, retaining sub-mA precision for kernel consumers. */
		*val = 158122 * adc->car_tune;
		*val2 = 5000000 * adc->sense_mohm;
		return IIO_VAL_FRACTIONAL;
	}
	if (mask != IIO_CHAN_INFO_PROCESSED &&
	    !(mask == IIO_CHAN_INFO_RAW && chan->type == IIO_CURRENT))
		return -EINVAL;
	mutex_lock(&adc->lock);
	switch (chan->type) {
	case IIO_CURRENT:
		ret = mt6351_current_raw(adc, val);
		if (!ret)
			ret = IIO_VAL_INT;
		break;
	case IIO_VOLTAGE:
		ret = mt6351_voltage(adc, 0, val);
		if (!ret)
			ret = IIO_VAL_INT;
		break;
	case IIO_TEMP:
		ret = mt6351_current(adc, &battery_current);
		if (!ret)
			ret = mt6351_voltage(adc, 3, &mv);
		if (!ret)
			ret = mt6351_ntc_temperature(adc, mv, battery_current, val);
		if (!ret)
			ret = IIO_VAL_INT;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&adc->lock);
	return ret;
}

static const struct iio_chan_spec mt6351_battery_channels[] = {
	{ .type = IIO_CURRENT, .indexed = 1, .channel = 0,
	  .info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | BIT(IIO_CHAN_INFO_SCALE),
	  .datasheet_name = "battery-current" },
	{ .type = IIO_VOLTAGE, .indexed = 1, .channel = 1,
	  .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED), .datasheet_name = "battery-voltage" },
	{ .type = IIO_TEMP, .indexed = 1, .channel = 2,
	  .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED), .datasheet_name = "battery-temperature" },
};

static const struct iio_info mt6351_battery_adc_info = {
	.read_raw = mt6351_battery_read_raw,
};

static int mt6351_battery_adc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6351_battery_adc *adc;
	struct iio_dev *indio_dev;
	struct device *parent;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;
	adc = iio_priv(indio_dev);
	for (parent = dev->parent; parent && !adc->map; parent = parent->parent)
		adc->map = dev_get_regmap(parent, NULL);
	if (!adc->map)
		return -EPROBE_DEFER;
	mutex_init(&adc->lock);
	ret = device_property_read_u32(dev, "mediatek,sense-resistor-milliohm", &adc->sense_mohm);
	ret |= device_property_read_u32(dev, "mediatek,car-tune-percent", &adc->car_tune);
	ret |= device_property_read_u32(dev, "mediatek,ntc-pullup-ohms", &adc->pullup_ohm);
	ret |= device_property_read_u32(dev, "mediatek,ntc-pullup-millivolt", &adc->pullup_mv);
	if (ret || !adc->sense_mohm || adc->sense_mohm > 100 ||
	    !adc->car_tune || adc->car_tune > 200 ||
	    !adc->pullup_ohm || adc->pullup_mv < 1800 || adc->pullup_mv > 3600)
		return dev_err_probe(dev, -EINVAL, "missing/invalid factory battery calibration\n");

	/* Stock clock/ADC setup. Preserve the fuel-gauge accumulated charge. */
	ret = regmap_update_bits(adc->map, 0x0246, BIT(2) | BIT(3), 0);
	if (!ret)
		ret = regmap_update_bits(adc->map, FG_CON0, 0xff, 0x29);
	if (!ret)
		ret = regmap_update_bits(adc->map, FG_OSR, 0xf, 8);
	if (!ret)
		ret = regmap_update_bits(adc->map, 0x0ea2, BIT(15), BIT(15));
	if (!ret)
		ret = regmap_update_bits(adc->map, 0x023a, BIT(9), 0);
	if (!ret)
		ret = regmap_update_bits(adc->map, 0x0ea8, 0x3f, (6 << 3) | 2);
	if (!ret)
		ret = regmap_update_bits(adc->map, 0x0eaa, 0xfff, 0x83);
	if (!ret)
		ret = regmap_update_bits(adc->map, 0x0eba, BIT(9), BIT(9));
	if (ret)
		return dev_err_probe(dev, ret, "battery ADC initialization\n");

	indio_dev->name = "mt6351-battery-adc";
	indio_dev->info = &mt6351_battery_adc_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = mt6351_battery_channels;
	indio_dev->num_channels = ARRAY_SIZE(mt6351_battery_channels);
	ret = devm_iio_device_register(dev, indio_dev);
	if (!ret)
		dev_info(dev, "stock BATON NTC and signed FG current ready; Rfg=%u mOhm, tune=%u%%\n",
			 adc->sense_mohm, adc->car_tune);
	return ret;
}

static const struct of_device_id mt6351_battery_adc_match[] = {
	{ .compatible = "mediatek,mt6351-battery-adc" }, { }
};
MODULE_DEVICE_TABLE(of, mt6351_battery_adc_match);

static struct platform_driver mt6351_battery_adc_driver = {
	.probe = mt6351_battery_adc_probe,
	.driver = { .name = "mt6351-battery-adc", .of_match_table = mt6351_battery_adc_match },
};
module_platform_driver(mt6351_battery_adc_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6351 battery voltage, thermistor and fuel-gauge current");
