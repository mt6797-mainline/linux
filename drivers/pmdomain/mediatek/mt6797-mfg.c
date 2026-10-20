// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 factory MFG power sequence; shader scheduling is owned by Panfrost. */
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

#define PWR_RST_B	BIT(0)
#define PWR_ISO		BIT(1)
#define PWR_ON		BIT(2)
#define PWR_ON_2ND	BIT(3)
#define PWR_CLK_DIS	BIT(4)
#define MFG_PROTECT	(BIT(21) | BIT(23))
#define MFG_BOOT_RATE	442500000UL
#define MFG_BRIDGE_RATE	416000000UL

struct mt6797_mfg {
	struct device *dev;
	struct generic_pm_domain domain;
	struct regmap *spm, *infra, *top, *pll;
	struct regulator *supply;
	void __iomem *base;
	struct clk_hw clock;
	struct mutex lock;
	unsigned long rate;
	unsigned int voltage, bridge_voltage;
	bool powered;
	bool fault;
};

static const struct mt6797_mfg_island {
	u16 control;
	u16 sram_control;
	u32 status;
	u32 sram_pdn;
	u32 sram_ack;
} mt6797_mfg_islands[] = {
	{ 0x334, 0,     BIT(13), 0,      0 },
	{ 0x338, 0x33c, BIT(12), 3,      GENMASK(17, 16) },
	{ 0x340, 0x340, BIT(11), BIT(8), BIT(20) },
	{ 0x344, 0x344, BIT(10), BIT(8), BIT(21) },
	{ 0x348, 0x348, BIT(9),  BIT(8), BIT(22) },
	{ 0x34c, 0x34c, BIT(8),  BIT(8), BIT(23) },
};

static int mt6797_mfg_poll(struct regmap *map, u32 reg, u32 mask, u32 expected)
{
	u32 value;

	return regmap_read_poll_timeout(map, reg, value,
				       (value & mask) == expected, 10, 10000);
}

static int mt6797_mfg_island(struct mt6797_mfg *m, unsigned int index, bool on)
{
	const struct mt6797_mfg_island *p = &mt6797_mfg_islands[index];
	int ret;

	/* clk-mt6797-pg: all shader SRAM acknowledgements are in MFG_SRAM_CON. */
	if (!on && p->sram_pdn) {
		regmap_set_bits(m->spm, p->sram_control, p->sram_pdn);
		ret = mt6797_mfg_poll(m->spm, 0x33c, p->sram_ack, p->sram_ack);
		if (ret)
			return ret;
	}
	if (on) {
		regmap_set_bits(m->spm, p->control, PWR_ON);
		regmap_set_bits(m->spm, p->control, PWR_ON_2ND);
	} else {
		/* Main MFG isolates before stopping its clock; shaders do the reverse. */
		if (index >= 2) {
			regmap_set_bits(m->spm, p->control, PWR_CLK_DIS);
			regmap_set_bits(m->spm, p->control, PWR_ISO);
		} else {
			regmap_set_bits(m->spm, p->control, PWR_ISO);
			regmap_set_bits(m->spm, p->control, PWR_CLK_DIS);
		}
		regmap_clear_bits(m->spm, p->control, PWR_RST_B);
		regmap_clear_bits(m->spm, p->control, PWR_ON);
		regmap_clear_bits(m->spm, p->control, PWR_ON_2ND);
	}
	ret = mt6797_mfg_poll(m->spm, 0x180, p->status, on ? p->status : 0);
	if (ret)
		return ret;
	ret = mt6797_mfg_poll(m->spm, 0x184, p->status, on ? p->status : 0);
	if (ret || !on)
		return ret;
	if (index >= 2) {
		regmap_set_bits(m->spm, p->control, PWR_RST_B);
		regmap_clear_bits(m->spm, p->control, PWR_ISO);
		regmap_clear_bits(m->spm, p->control, PWR_CLK_DIS);
	} else {
		regmap_clear_bits(m->spm, p->control, PWR_CLK_DIS);
		regmap_clear_bits(m->spm, p->control, PWR_ISO);
		regmap_set_bits(m->spm, p->control, PWR_RST_B);
	}
	if (!p->sram_pdn)
		return 0;
	regmap_clear_bits(m->spm, p->sram_control, p->sram_pdn);
	return mt6797_mfg_poll(m->spm, 0x33c, p->sram_ack, 0);
}

static int mt6797_mfg_opp_voltage(struct mt6797_mfg *m, unsigned long rate)
{
	struct dev_pm_opp *opp;
	int voltage;

	opp = dev_pm_opp_find_freq_exact(m->dev, rate, true);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	voltage = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);
	return voltage ? voltage : -EINVAL;
}

static int mt6797_mfg_voltage(struct mt6797_mfg *m, unsigned int voltage)
{
	int ret;

	ret = regulator_set_voltage(m->supply, voltage, voltage);
	if (ret)
		return ret;
	/* The DT selects the factory 32 mV/us ramp; regulator core waits for it. */
	ret = regulator_get_voltage(m->supply);
	if (ret < 0)
		return ret;
	if (ret != voltage)
		return -EIO;
	m->voltage = voltage;
	return 0;
}

static unsigned int mt6797_mfg_postdiv(unsigned long rate)
{
	/*
	 * Keep VCO in 1..2 GHz. Larger PCWs are rejected/clamped on this silicon.
	 * All eight factory output OPPs remain available, with /2 above 500 MHz.
	 */
	return rate < 250000000 ? 3 : rate <= 500000000 ? 2 : 1;
}

static int mt6797_mfg_mux(struct mt6797_mfg *m, unsigned int source)
{
	int ret;

	ret = regmap_write(m->top, 0x58, GENMASK(25, 24) | BIT(31));
	if (ret)
		return ret;
	ret = regmap_write(m->top, 0x54, source << 24);
	if (ret)
		return ret;
	ret = regmap_write(m->top, 0x4, BIT(7));
	if (ret)
		return ret;
	ret = mt6797_mfg_poll(m->top, 0x4, BIT(7), 0);
	if (ret)
		return ret;
	return mt6797_mfg_poll(m->top, 0x50, GENMASK(25, 24) | BIT(31), source << 24);
}

static int mt6797_mfg_pll_rate(struct mt6797_mfg *m, unsigned long rate)
{
	u32 pcw = DIV_ROUND_CLOSEST_ULL((u64)rate * BIT(mt6797_mfg_postdiv(rate)) *
				      BIT(14), 26000000);
	u32 value = (mt6797_mfg_postdiv(rate) << 24) | pcw;
	int ret;

	/* Commit postdiv and PCW together, as in the factory GPU clock path. */
	ret = regmap_write(m->pll, 0x244, BIT(31) | value);
	if (ret)
		return ret;
	udelay(20);
	return mt6797_mfg_poll(m->pll, 0x244, GENMASK(26, 24) | GENMASK(20, 0), value);
}

static unsigned long mt6797_mfg_recalc_rate(struct clk_hw *hw, unsigned long parent)
{
	struct mt6797_mfg *m = container_of(hw, struct mt6797_mfg, clock);

	return READ_ONCE(m->rate);
}

static long mt6797_mfg_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *parent)
{
	struct mt6797_mfg *m = container_of(hw, struct mt6797_mfg, clock);
	struct dev_pm_opp *opp;

	opp = dev_pm_opp_find_freq_ceil(m->dev, &rate);
	if (IS_ERR(opp))
		opp = dev_pm_opp_find_freq_floor(m->dev, &rate);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);
	return rate;
}

static int mt6797_mfg_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long parent)
{
	struct mt6797_mfg *m = container_of(hw, struct mt6797_mfg, clock);
	u32 con;
	unsigned int boost;
	bool bridge;
	int voltage, ret = 0;

	voltage = mt6797_mfg_opp_voltage(m, rate);
	if (voltage < 0)
		return voltage;
	mutex_lock(&m->lock);
	if (m->fault) {
		ret = -EIO;
		goto out;
	}
	if (m->rate == rate)
		goto out;
	if (!m->powered) {
		/* Thermal limits may change while suspended; restore this OPP on wake. */
		WRITE_ONCE(m->rate, rate);
		goto out;
	}
	ret = regmap_read(m->pll, 0x244, &con);
	if (ret)
		goto out;
	bridge = ((con >> 24) & 7) != mt6797_mfg_postdiv(rate);
	boost = max_t(unsigned int, m->voltage, voltage);
	if (bridge)
		boost = max(boost, m->bridge_voltage);
	ret = mt6797_mfg_voltage(m, boost);
	if (ret)
		goto out;
	if (bridge) {
		/* Factory sequence: raise VGPU, UNIVPLL_D3, PLL postdiv, MFGPLL. */
		ret = mt6797_mfg_mux(m, 3);
		if (ret)
			goto fault;
	}
	ret = mt6797_mfg_pll_rate(m, rate);
	if (ret)
		goto fault;
	if (bridge) {
		ret = mt6797_mfg_mux(m, 1);
		if (ret)
			goto fault;
	}
	WRITE_ONCE(m->rate, rate);
	/* Lower voltage only after the final clock and divider are confirmed. */
	ret = mt6797_mfg_voltage(m, voltage);
	if (ret)
		goto fault;
	dev_dbg(m->dev, "OPP %lu Hz %d uV, bridge=%d\n", rate, voltage, bridge);
	goto out;
fault:
	/* Keep the raised supply after an uncertain clock transition. */
	m->fault = true;
	dev_err(m->dev, "OPP transition to %lu Hz failed: %d\n", rate, ret);
out:
	mutex_unlock(&m->lock);
	return ret;
}

static const struct clk_ops mt6797_mfg_clock_ops = {
	.recalc_rate = mt6797_mfg_recalc_rate,
	.round_rate = mt6797_mfg_round_rate,
	.set_rate = mt6797_mfg_set_rate,
};

static int mt6797_mfg_clock_on(struct mt6797_mfg *m)
{
	int ret;

	/* MFGPLL alone: no writes to the shared MAINPLL or UNIVPLL. */
	regmap_set_bits(m->pll, 0x24c, BIT(0));
	udelay(1);
	regmap_clear_bits(m->pll, 0x24c, BIT(1));
	ret = mt6797_mfg_pll_rate(m, m->rate);
	if (ret)
		return ret;
	regmap_set_bits(m->pll, 0x240, BIT(0) | BIT(8));
	udelay(20);
	/* MFG source = MFGPLL, update bit 7; 52 MHz source = UNIVPLL2_D8. */
	ret = mt6797_mfg_mux(m, 1);
	if (ret)
		return ret;
	regmap_update_bits(m->top, 0x104, GENMASK(2, 1), BIT(1));
	return regmap_write(m->infra, 0x8c, BIT(14));
}

static int mt6797_mfg_do_power_on(struct generic_pm_domain *domain)
{
	struct mt6797_mfg *m = container_of(domain, struct mt6797_mfg, domain);
	int i, ret;

	if (m->fault)
		return -EIO;
	ret = mt6797_mfg_opp_voltage(m, m->rate);
	if (ret < 0)
		return ret;
	ret = mt6797_mfg_voltage(m, ret);
	if (ret)
		return ret;
	regmap_write(m->infra, 0xfbc, 0x1ff);
	ret = regulator_enable(m->supply);
	if (ret) {
		regmap_write(m->infra, 0xfbc, 0);
		return ret;
	}
	ret = mt6797_mfg_clock_on(m);
	if (ret) {
		m->fault = true;
		return dev_err_probe(m->dev, ret, "clock restore failed\n");
	}
	regmap_write(m->spm, 0, 0x0b160001);
	for (i = 0; i < ARRAY_SIZE(mt6797_mfg_islands); i++) {
		ret = mt6797_mfg_island(m, i, true);
		if (ret) {
			/* Acknowledgement failure leaves the supply on for diagnosis. */
			m->fault = true;
			dev_err(m->dev, "island %d power-on failed: %d\n", i, ret);
			return ret;
		}
	}
	writel(BIT(0), m->base + 8);
	udelay(100);
	regmap_clear_bits(m->infra, 0x220, MFG_PROTECT);
	ret = mt6797_mfg_poll(m->infra, 0x228, MFG_PROTECT, 0);
	if (ret) {
		m->fault = true;
		return dev_err_probe(m->dev, ret, "bus protection release failed\n");
	}
	writel(readl(m->base + 0x1c) | 5, m->base + 0x1c);
	for (i = 0x3e0; i <= 0x3f0; i += 4)
		writel(U32_MAX, m->base + i);
	dev_info_once(m->dev, "six MFG islands ready at %lu Hz, %d uV\n",
		      m->rate, regulator_get_voltage(m->supply));
	return 0;
}

static int mt6797_mfg_do_power_off(struct generic_pm_domain *domain)
{
	struct mt6797_mfg *m = container_of(domain, struct mt6797_mfg, domain);
	u32 value;
	int i, ret;

	if (m->fault)
		return -EIO;
	writel(3, m->base + 0x180);
	ret = readl_poll_timeout(m->base + 0x184, value, value & BIT(2), 10, 100000);
	if (ret)
		return ret;
	regmap_set_bits(m->infra, 0x220, MFG_PROTECT);
	ret = mt6797_mfg_poll(m->infra, 0x228, MFG_PROTECT, MFG_PROTECT);
	if (ret)
		return ret;
	writel(BIT(0), m->base + 4);
	regmap_write(m->spm, 0, 0x0b160001);
	for (i = ARRAY_SIZE(mt6797_mfg_islands) - 1; i >= 0; i--) {
		ret = mt6797_mfg_island(m, i, false);
		if (ret) {
			m->fault = true;
			dev_err(m->dev, "island %d power-off failed: %d\n", i, ret);
			return ret;
		}
	}
	regmap_write(m->infra, 0x88, BIT(14));
	regmap_clear_bits(m->pll, 0x240, BIT(0));
	regmap_set_bits(m->pll, 0x24c, BIT(1));
	regmap_clear_bits(m->pll, 0x24c, BIT(0));
	ret = regulator_disable(m->supply);
	regmap_write(m->infra, 0xfbc, 0);
	return ret;
}

static int mt6797_mfg_power_on(struct generic_pm_domain *domain)
{
	struct mt6797_mfg *m = container_of(domain, struct mt6797_mfg, domain);
	int ret;

	mutex_lock(&m->lock);
	ret = mt6797_mfg_do_power_on(domain);
	if (!ret)
		m->powered = true;
	mutex_unlock(&m->lock);
	return ret;
}

static int mt6797_mfg_power_off(struct generic_pm_domain *domain)
{
	struct mt6797_mfg *m = container_of(domain, struct mt6797_mfg, domain);
	int ret;

	mutex_lock(&m->lock);
	ret = mt6797_mfg_do_power_off(domain);
	if (!ret)
		m->powered = false;
	mutex_unlock(&m->lock);
	return ret;
}

static int mt6797_mfg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_mfg *m;
	struct reset_control *reset;
	struct dev_pm_opp *opp;
	struct clk_init_data init = {
		.name = "mt6797-mfg",
		.ops = &mt6797_mfg_clock_ops,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	unsigned long rate = MFG_BRIDGE_RATE;
	u32 value;
	int ret;

	m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->dev = dev;
	mutex_init(&m->lock);
	m->rate = MFG_BOOT_RATE;
	m->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(m->base))
		return PTR_ERR(m->base);
	m->spm = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(m->spm))
		return PTR_ERR(m->spm);
	m->infra = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(m->infra))
		return PTR_ERR(m->infra);
	m->top = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,topckgen");
	if (IS_ERR(m->top))
		return PTR_ERR(m->top);
	m->pll = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,apmixedsys");
	if (IS_ERR(m->pll))
		return PTR_ERR(m->pll);
	m->supply = devm_regulator_get(dev, "mali");
	if (IS_ERR(m->supply))
		return dev_err_probe(dev, PTR_ERR(m->supply), "VGPU unavailable\n");
	ret = devm_pm_opp_of_add_table(dev);
	if (ret)
		return ret;
	opp = dev_pm_opp_find_freq_ceil(dev, &rate);
	if (IS_ERR(opp))
		return dev_err_probe(dev, PTR_ERR(opp), "no OPP for 416 MHz bridge\n");
	m->bridge_voltage = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);
	if (!m->bridge_voltage)
		return -EINVAL;
	reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(reset))
		return PTR_ERR(reset);
	ret = regmap_read(m->spm, 0x180, &value);
	if (ret)
		return ret;
	/* Do not reset a GPU still owned by another boot stage. */
	if (value & GENMASK(13, 8))
		return dev_err_probe(dev, -EBUSY, "firmware left MFG islands on\n");
	ret = reset_control_assert(reset);
	if (ret)
		return ret;
	udelay(1);
	ret = reset_control_deassert(reset);
	if (ret)
		return ret;
	/* Factory GPU internal LDO configuration, before enabling the rail. */
	regmap_write(m->infra, 0xfd8, 0x88888888);
	regmap_write(m->infra, 0xfe4, 8);
	regmap_write(m->infra, 0xfc0, 0x0f0f0f0f);
	regmap_write(m->infra, 0xfc4, 0x0f0f0f0f);
	regmap_write(m->infra, 0xfc8, 0x0f);
	regmap_set_bits(m->infra, 0x220, MFG_PROTECT);
	m->clock.init = &init;
	ret = devm_clk_hw_register(dev, &m->clock);
	if (ret)
		return ret;
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &m->clock);
	if (ret)
		return ret;
	m->domain.name = "mt6797-mfg";
	m->domain.power_on = mt6797_mfg_power_on;
	m->domain.power_off = mt6797_mfg_power_off;
	ret = pm_genpd_init(&m->domain, NULL, true);
	if (ret)
		return ret;
	ret = of_genpd_add_provider_simple(dev->of_node, &m->domain);
	if (ret)
		pm_genpd_remove(&m->domain);
	return ret;
}

static const struct of_device_id mt6797_mfg_ids[] = {
	{ .compatible = "mediatek,mt6797-mfg" },
	{ }
};

static struct platform_driver mt6797_mfg_driver = {
	.probe = mt6797_mfg_probe,
	.driver = {
		.name = "mt6797-mfg",
		.of_match_table = mt6797_mfg_ids,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6797_mfg_driver);
