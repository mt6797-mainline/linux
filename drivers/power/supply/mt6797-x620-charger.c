// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 X620 battery policy using the native CW2015 and BQ2589x drivers. */
#include <linux/devm-helpers.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

struct mt6797_x620_charger {
	struct device *dev;
	struct power_supply *primary;
	struct power_supply *secondary;
	struct power_supply *battery;
	struct power_supply *typec_source;
	struct power_supply *supply;
	struct thermal_cooling_device *cooling;
	struct delayed_work work;
	struct mutex lock;
	unsigned long thermal_limit;
	unsigned int user_limit;
	int user_behaviour;
	int status, health, online, usb_type;
	int charge_ua, input_ua, voltage_uv;
	bool cold, hot;
	bool ready;
};

static int mt6797_x620_psy_set(struct power_supply *psy,
			enum power_supply_property prop, int value)
{
	const union power_supply_propval val = { .intval = value };

	return power_supply_set_property(psy, prop, &val);
}

static int mt6797_x620_psy_get(struct power_supply *psy,
			enum power_supply_property prop, int *value)
{
	union power_supply_propval val;
	int ret = power_supply_get_property(psy, prop, &val);

	if (!ret)
		*value = val.intval;
	return ret;
}

static int mt6797_x620_charge_disable(struct mt6797_x620_charger *chg)
{
	int ret, ret2;

	ret = mt6797_x620_psy_set(chg->secondary, POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
			    POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE);
	ret2 = mt6797_x620_psy_set(chg->secondary, POWER_SUPPLY_PROP_ONLINE, 0);
	ret = ret ?: ret2;
	ret2 = mt6797_x620_psy_set(chg->primary, POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
			     POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE);
	return ret ?: ret2;
}

static void mt6797_x620_charge_work(struct work_struct *work)
{
	struct mt6797_x620_charger *chg = container_of(to_delayed_work(work),
						struct mt6797_x620_charger, work);
	int temp, present, health, ret, status, limit, attached, available_ua;
	int charge_current = 0, input = 500000, voltage = 4400000;

	mutex_lock(&chg->lock);
	ret = mt6797_x620_psy_get(chg->primary,
				  POWER_SUPPLY_PROP_ONLINE,
				  &chg->online);
	if (ret)
		goto failed;
	ret = mt6797_x620_psy_get(chg->primary,
				  POWER_SUPPLY_PROP_USB_TYPE,
				  &chg->usb_type);
	if (ret)
		goto failed;
	/* Stock SDP/unknown 500 mA, charging host 650 mA; 5V DCP screen-on cap 1.5A. */
	switch (chg->usb_type) {
	case POWER_SUPPLY_USB_TYPE_CDP:
		input = 650000;
		break;
	case POWER_SUPPLY_USB_TYPE_DCP:
		input = 1500000;
		break;
	default:
		break;
	}
	/* CC current advertisement supersedes BC1.2 for a Type-C source. */
	if (chg->typec_source) {
		ret = mt6797_x620_psy_get(chg->typec_source,
					  POWER_SUPPLY_PROP_ONLINE,
					  &attached);
		if (ret)
			goto failed;
		if (!attached) {
			chg->online = 0;
			goto disabled;
		}
		ret = mt6797_x620_psy_get(chg->typec_source,
					  POWER_SUPPLY_PROP_CURRENT_MAX,
					  &available_ua);
		if (ret)
			goto failed;
		if (available_ua >= 1500000) {
			/* Retain the stock 5V screen-on board cap, even on a 3A source. */
			input = min(available_ua, 1500000);
			chg->usb_type = POWER_SUPPLY_USB_TYPE_C;
		}
	}
	ret = mt6797_x620_psy_get(chg->primary,
				  POWER_SUPPLY_PROP_HEALTH,
				  &health);
	if (ret)
		goto failed;
	chg->health = health;
	ret = mt6797_x620_psy_get(chg->battery,
				  POWER_SUPPLY_PROP_PRESENT,
				  &present);
	if (ret || !present) {
		ret = ret ?: -ENODEV;
		goto failed;
	}
	ret = mt6797_x620_psy_get(chg->battery,
				  POWER_SUPPLY_PROP_TEMP,
				  &temp);
	if (ret)
		goto failed;

	/* Live stock DT charge range 0..50 C, resume at 1/49 C. */
	chg->cold = temp <= 0 || (chg->cold && temp < 10);
	chg->hot = temp >= 500 || (chg->hot && temp > 490);
	if (chg->cold)
		chg->health = POWER_SUPPLY_HEALTH_COLD;
	if (chg->hot)
		chg->health = POWER_SUPPLY_HEALTH_OVERHEAT;
	if (chg->cold || chg->hot || health != POWER_SUPPLY_HEALTH_GOOD ||
	    !chg->online || chg->user_behaviour == POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE)
		goto disabled;

	charge_current = input;
	/* Stock cool/warm JEITA CV, never use the charger die temperature as TBAT. */
	if (temp < 100 || temp >= 450) {
		charge_current = min(charge_current, 500000);
		voltage = 4240000;
	}
	limit = max_t(unsigned int, chg->user_limit, chg->thermal_limit);
	if (limit == 3)
		goto disabled;
	if (limit)
		charge_current = min(charge_current, limit == 1 ? 1000000 : 500000);

	/* Stock keeps the auxiliary path off below 2.25 A aggregate charge current. */
	ret = mt6797_x620_psy_set(chg->secondary, POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
			    POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE);
	if (!ret)
		ret = mt6797_x620_psy_set(chg->secondary,
					  POWER_SUPPLY_PROP_ONLINE,
					  0);
	if (!ret)
		ret = mt6797_x620_psy_set(chg->primary,
					  POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
					  voltage);
	if (!ret)
		ret = mt6797_x620_psy_set(chg->primary,
					  POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
					  input);
	if (!ret)
		ret = mt6797_x620_psy_set(chg->primary,
					  POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
					  charge_current);
	if (!ret)
		ret = mt6797_x620_psy_set(chg->primary, POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
				    POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO);
	if (ret)
		goto failed;
	ret = mt6797_x620_psy_get(chg->primary,
				  POWER_SUPPLY_PROP_STATUS,
				  &status);
	if (ret)
		goto failed;
	chg->status = status;
	ret = mt6797_x620_psy_get(chg->primary,
				  POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
				  &chg->charge_ua);
	if (!ret)
		ret = mt6797_x620_psy_get(chg->primary,
					  POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
					  &chg->input_ua);
	if (!ret)
		ret = mt6797_x620_psy_get(chg->primary,
					  POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
					  &chg->voltage_uv);
	if (ret)
		goto failed;
	goto done;

failed:
	input = 500000;
	chg->health = POWER_SUPPLY_HEALTH_UNKNOWN;
	dev_err_ratelimited(chg->dev, "battery/charger sample failed (%d); inhibiting charge\n", ret);
disabled:
	/* A CC detach/read error must also remove the previous high input limit. */
	ret = mt6797_x620_psy_set(chg->primary, POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
			    chg->online ? input : 500000);
	if (!ret)
		chg->input_ua = chg->online ? input : 500000;
	else {
		chg->health = POWER_SUPPLY_HEALTH_UNKNOWN;
		dev_err_ratelimited(chg->dev, "input limit update failed: %d\n", ret);
	}
	ret = mt6797_x620_charge_disable(chg);
	if (ret)
		dev_err_ratelimited(chg->dev, "charge inhibit failed: %d\n", ret);
	chg->status = chg->online ? POWER_SUPPLY_STATUS_NOT_CHARGING : POWER_SUPPLY_STATUS_DISCHARGING;
	chg->charge_ua = 0;
done:
	mutex_unlock(&chg->lock);
	power_supply_changed(chg->supply);
	schedule_delayed_work(&chg->work, msecs_to_jiffies(5000));
}

static int mt6797_x620_charger_get_property(struct power_supply *psy,
					     enum power_supply_property prop,
				     union power_supply_propval *val)
{
	struct mt6797_x620_charger *chg = power_supply_get_drvdata(psy);
	int ret = 0;

	mutex_lock(&chg->lock);
	switch (prop) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = chg->online;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = chg->status;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = chg->health;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = chg->usb_type;
		break;
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		val->intval = chg->user_behaviour;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		val->intval = chg->user_limit;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		val->intval = 3;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		val->intval = chg->charge_ua;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = chg->input_ua;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		val->intval = chg->voltage_uv;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&chg->lock);
	return ret;
}

static int mt6797_x620_charger_set_property(struct power_supply *psy,
					     enum power_supply_property prop,
				     const union power_supply_propval *val)
{
	struct mt6797_x620_charger *chg = power_supply_get_drvdata(psy);

	if (prop == POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR) {
		if (val->intval != POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO &&
		    val->intval != POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE)
			return -EINVAL;
	} else if (prop == POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT) {
		if (val->intval < 0 || val->intval > 3)
			return -EINVAL;
	} else {
		return -EINVAL;
	}
	mutex_lock(&chg->lock);
	if (prop == POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR)
		chg->user_behaviour = val->intval;
	else
		chg->user_limit = val->intval;
	mutex_unlock(&chg->lock);
	mod_delayed_work(system_wq, &chg->work, 0);
	return 0;
}

static int mt6797_x620_charger_writeable(struct power_supply *psy,
				  enum power_supply_property prop)
{
	return prop == POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR ||
		prop == POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT;
}

static void mt6797_x620_charger_source_changed(struct power_supply *psy)
{
	struct mt6797_x620_charger *chg = power_supply_get_drvdata(psy);

	if (READ_ONCE(chg->ready))
		mod_delayed_work(system_wq, &chg->work, 0);
}

static enum power_supply_property mt6797_x620_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_USB_TYPE, POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT, POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT, POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
};

static const struct power_supply_desc mt6797_x620_charger_desc = {
	.name = "mt6797-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = mt6797_x620_charger_props,
	.num_properties = ARRAY_SIZE(mt6797_x620_charger_props),
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) | BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) | BIT(POWER_SUPPLY_USB_TYPE_DCP) |
		     BIT(POWER_SUPPLY_USB_TYPE_C),
	.charge_behaviours = BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) |
			     BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE),
	.get_property = mt6797_x620_charger_get_property,
	.set_property = mt6797_x620_charger_set_property,
	.property_is_writeable = mt6797_x620_charger_writeable,
	.external_power_changed = mt6797_x620_charger_source_changed,
};

static int mt6797_x620_charge_get_max(struct thermal_cooling_device *cdev, unsigned long *state)
{
	*state = 3;
	return 0;
}

static int mt6797_x620_charge_get_cur(struct thermal_cooling_device *cdev, unsigned long *state)
{
	struct mt6797_x620_charger *chg = cdev->devdata;

	*state = READ_ONCE(chg->thermal_limit);
	return 0;
}

static int mt6797_x620_charge_set_cur(struct thermal_cooling_device *cdev, unsigned long state)
{
	struct mt6797_x620_charger *chg = cdev->devdata;

	if (state > 3)
		return -EINVAL;
	mutex_lock(&chg->lock);
	chg->thermal_limit = state;
	mutex_unlock(&chg->lock);
	mod_delayed_work(system_wq, &chg->work, 0);
	return 0;
}

static const struct thermal_cooling_device_ops mt6797_x620_charge_cooling_ops = {
	.get_max_state = mt6797_x620_charge_get_max,
	.get_cur_state = mt6797_x620_charge_get_cur,
	.set_cur_state = mt6797_x620_charge_set_cur,
};

static void mt6797_x620_charge_stop(void *data)
{
	struct mt6797_x620_charger *chg = data;

	WRITE_ONCE(chg->ready, false);
	cancel_delayed_work_sync(&chg->work);
	mt6797_x620_charge_disable(chg);
}

static int mt6797_x620_charger_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config config = { .of_node = dev->of_node };
	struct mt6797_x620_charger *chg;
	int ret;

	chg = devm_kzalloc(dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;
	chg->dev = dev;
	mutex_init(&chg->lock);
	INIT_DELAYED_WORK(&chg->work, mt6797_x620_charge_work);
	platform_set_drvdata(pdev, chg);
	chg->primary = devm_power_supply_get_by_phandle(dev, "primary-charger");
	if (IS_ERR(chg->primary))
		return dev_err_probe(dev, PTR_ERR(chg->primary), "primary charger\n");
	chg->secondary = devm_power_supply_get_by_phandle(dev, "secondary-charger");
	if (IS_ERR(chg->secondary))
		return dev_err_probe(dev, PTR_ERR(chg->secondary), "secondary charger\n");
	chg->battery = devm_power_supply_get_by_phandle(dev, "battery-gauge");
	if (IS_ERR(chg->battery))
		return dev_err_probe(dev, PTR_ERR(chg->battery), "battery gauge\n");
	if (!chg->primary || !chg->secondary || !chg->battery)
		return -EPROBE_DEFER;
	if (device_property_present(dev, "typec-current-source")) {
		chg->typec_source = devm_power_supply_get_by_phandle(dev, "typec-current-source");
		if (IS_ERR(chg->typec_source))
			return dev_err_probe(dev, PTR_ERR(chg->typec_source), "Type-C current source\n");
		if (!chg->typec_source)
			return -EPROBE_DEFER;
	}
	ret = mt6797_x620_charge_disable(chg);
	if (ret)
		return dev_err_probe(dev, ret, "initial charge inhibit\n");
	config.drv_data = chg;
	chg->supply = devm_power_supply_register(dev, &mt6797_x620_charger_desc, &config);
	if (IS_ERR(chg->supply))
		return PTR_ERR(chg->supply);
	ret = devm_add_action_or_reset(dev, mt6797_x620_charge_stop, chg);
	if (ret)
		return ret;
	chg->cooling = devm_thermal_of_cooling_device_register(dev, dev->of_node,
				"mt6797-charger", chg, &mt6797_x620_charge_cooling_ops);
	if (IS_ERR(chg->cooling))
		return PTR_ERR(chg->cooling);
	WRITE_ONCE(chg->ready, true);
	schedule_delayed_work(&chg->work, 0);
	dev_info(dev, "stock 0..50C battery limits; 5V USB policy, auxiliary charger inhibited\n");
	return 0;
}

static int mt6797_x620_charger_suspend(struct device *dev)
{
	mt6797_x620_charge_stop(dev_get_drvdata(dev));
	return 0;
}

static int mt6797_x620_charger_resume(struct device *dev)
{
	struct mt6797_x620_charger *chg = dev_get_drvdata(dev);

	WRITE_ONCE(chg->ready, true);
	mod_delayed_work(system_wq, &chg->work, 0);
	return 0;
}

static void mt6797_x620_charger_shutdown(struct platform_device *pdev)
{
	mt6797_x620_charge_stop(platform_get_drvdata(pdev));
}

static DEFINE_SIMPLE_DEV_PM_OPS(mt6797_x620_charger_pm,
			      mt6797_x620_charger_suspend,
			      mt6797_x620_charger_resume);
static const struct of_device_id mt6797_x620_charger_match[] = {
	{ .compatible = "mediatek,mt6797-x620-charger" }, { }
};
MODULE_DEVICE_TABLE(of, mt6797_x620_charger_match);
static struct platform_driver mt6797_x620_charger_driver = {
	.probe = mt6797_x620_charger_probe,
	.shutdown = mt6797_x620_charger_shutdown,
	.driver = {
		.name = "mt6797-x620-charger",
		.of_match_table = mt6797_x620_charger_match,
		.pm = pm_sleep_ptr(&mt6797_x620_charger_pm),
	},
};
module_platform_driver(mt6797_x620_charger_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6797 X620 battery temperature and 5V USB charging policy");
