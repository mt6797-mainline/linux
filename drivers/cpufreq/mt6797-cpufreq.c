// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 LL/L/CCI DVFS using the stock shared rail and MCU FHCTL sequence. */
#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/seq_file.h>
#include <linux/thermal.h>
#include <asm/smp_plat.h>
#include "mt6797-opp.h"

#define PLL_CON1(id) (0x204 + (id) * 0x10)
#define PLL_MUX 0x270
#define PLL_DIV 0x274
#define DDS_MASK GENMASK(20, 0)
#define FH_BASE 0xf00
#define FH_CFG(id) (FH_BASE + 0x34 + (id) * 0x14)
#define FH_DDS(id) (FH_CFG(id) + 8)
#define FH_DVFS(id) (FH_CFG(id) + 12)
#define FH_MON(id) (FH_CFG(id) + 16)
#define SRAM_MIN_UV 1000000
#define SRAM_MAX_UV 1200000
#define SRAM_MARGIN_UV 100000
#define SRAM_MAX_DIFF_UV 300000
#define SRAM_STEP_UV 25000
#define MT6797_MCUSYS_WRITE 0x82000201

struct mt6797_dvfs {
	struct device *dev;
	void __iomem *mcu, *cspm, *mcucfg;
	resource_size_t mcucfg_phys;
	struct regmap *infra, *top;
	struct regulator *vproc;
	struct thermal_zone_device *thermal;
	struct mutex lock;
	spinlock_t mcu_lock;
	const struct mt6797_opp (*opps)[16];
	struct cpufreq_frequency_table tables[2][17];
	unsigned int index[3], khz[3], bin;
	bool fault;
	struct dentry *debug;
};
static struct mt6797_dvfs *mt6797_dvfs;

/* Stock silicon workaround: 200 ns, consecutive reads, bounded write retry. */
static u32 mt6797_mcu_read(struct mt6797_dvfs *d, u32 offset)
{
	u32 value;
	int i;

	ndelay(200);
	for (i = 0; i < 3; i++) {
		readl(d->mcu + offset);
		value = readl(d->mcu + offset);
		if (value)
			break;
	}
	return value;
}
static int mt6797_mcu_write(struct mt6797_dvfs *d, u32 offset, u32 value, u32 mask)
{
	int i;

	for (i = 0; i < 4; i++) {
		writel(value, d->mcu + offset);
		ndelay(200);
		if ((mt6797_mcu_read(d, offset) & mask) == (value & mask))
			return 0;
	}
	return -EIO;
}
static int mt6797_mcu_update(struct mt6797_dvfs *d, u32 offset, u32 mask, u32 value)
{
	return mt6797_mcu_write(d, offset,
		(mt6797_mcu_read(d, offset) & ~mask) | (value & mask), mask);
}

/* Hardware semaphore 3 M0 is shared with the original SPM/ATF. */
static int mt6797_mcu_lock(struct mt6797_dvfs *d, unsigned long *flags)
{
	int i;

	spin_lock_irqsave(&d->mcu_lock, *flags);
	writel(0x0b160001, d->cspm);
	for (i = 0; i < 200; i++) {
		writel(1, d->cspm + 0x440);
		if (readl(d->cspm + 0x440) & 1)
			return 0;
		udelay(10);
	}
	spin_unlock_irqrestore(&d->mcu_lock, *flags);
	return -ETIMEDOUT;
}
static void mt6797_mcu_unlock(struct mt6797_dvfs *d, unsigned long flags)
{
	writel(1, d->cspm + 0x440);
	if (readl(d->cspm + 0x440) & 1) {
		d->fault = true;
		dev_err_ratelimited(d->dev, "MCU semaphore release failed\n");
	}
	spin_unlock_irqrestore(&d->mcu_lock, flags);
}
static unsigned int mt6797_pll_khz(struct mt6797_dvfs *d, unsigned int id)
{
	u32 con, div;
	unsigned long flags;
	unsigned int khz;

	if (mt6797_mcu_lock(d, &flags))
		return 0;
	con = mt6797_mcu_read(d, PLL_CON1(id));
	div = (mt6797_mcu_read(d, PLL_DIV) >> (5 + id * 5)) & 31;
	mt6797_mcu_unlock(d, flags);
	khz = ((con & DDS_MASK) * 26 >> 14) * 1000;
	khz >>= (con >> 24) & 7;
	if (div >= 8 && div <= 11)
		return khz * (12 - div) / 4;
	if (div >= 16 && div <= 20)
		return khz * (21 - div) / 5;
	if (div >= 24 && div <= 29)
		return khz * (30 - div) / 6;
	/* Stock treats reset/default divider encodings as divide-by-one. */
	return khz;
}
static int mt6797_sram_get(struct mt6797_dvfs *d)
{
	u32 enable, a, b, value;
	int i, ret;

	ret = regmap_read(d->infra, 0xf98, &enable);
	if (ret)
		return ret;
	if ((enable & 0xff) != 0xff)
		return -EINVAL;
	ret = regmap_read(d->infra, 0xf9c, &a);
	if (ret)
		return ret;
	ret = regmap_read(d->infra, 0xfa0, &b);
	if (ret)
		return ret;
	value = a & 15;
	for (i = 0; i < 4; i++)
		if (((a >> (8 * i)) & 15) != value || ((b >> (8 * i)) & 15) != value)
			return -EINVAL;
	if (!value)
		return 1050000;
	if (value < 3)
		return -EINVAL;
	return 900000 + (value - 3) * 25000;
}
static int mt6797_sram_set(struct mt6797_dvfs *d, unsigned int uv)
{
	u32 code;
	int ret;

	uv = roundup(uv, SRAM_STEP_UV);
	if (uv < SRAM_MIN_UV || uv > SRAM_MAX_UV)
		return -EINVAL;
	code = (uv - 900000) / 25000 + 3;
	code *= 0x01010101;
	ret = regmap_write(d->infra, 0xf9c, code);
	if (ret)
		return ret;
	ret = regmap_write(d->infra, 0xfa0, code);
	if (ret)
		return ret;
	return mt6797_sram_get(d) == uv ? 0 : -EIO;
}
static int mt6797_set_voltage(struct mt6797_dvfs *d, unsigned int uv)
{
	int proc = regulator_get_voltage(d->vproc), sram = mt6797_sram_get(d);
	int next_proc, next_sram, ret, delay;

	if (proc < 0 || sram < 0)
		return proc < 0 ? proc : sram;
	uv = roundup(uv, 10000); /* DA9214 10 mV selector step */
	if (proc > sram || sram - proc > SRAM_MAX_DIFF_UV)
		return -EINVAL;
	while (proc != uv) {
		if (uv > proc) {
			next_sram = clamp(roundup(min(proc + SRAM_MAX_DIFF_UV - SRAM_STEP_UV,
					(int)uv + SRAM_MARGIN_UV), SRAM_STEP_UV),
					SRAM_MIN_UV, SRAM_MAX_UV);
			ret = mt6797_sram_set(d, next_sram);
			if (ret)
				return ret;
			next_proc = min((int)uv, next_sram == SRAM_MAX_UV ? (int)uv :
					 next_sram - SRAM_MARGIN_UV);
			next_proc = roundup(next_proc, 10000);
			ret = regulator_set_voltage(d->vproc, next_proc, next_proc);
			if (ret)
				return ret;
		} else {
			next_proc = max((int)uv, sram - SRAM_MAX_DIFF_UV + SRAM_STEP_UV);
			next_proc = roundup(next_proc, 10000);
			ret = regulator_set_voltage(d->vproc, next_proc, next_proc);
			if (ret)
				return ret;
			next_sram = clamp(roundup(next_proc + SRAM_MARGIN_UV, SRAM_STEP_UV),
					SRAM_MIN_UV, SRAM_MAX_UV);
			ret = mt6797_sram_set(d, next_sram);
			if (ret)
				return ret;
		}
		/* Stock DA9214 and SRAM ramp formulas (microvolt units). */
		delay = max(abs(next_proc - proc) / 10000 + 1,
			    next_sram >= sram ? DIV_ROUND_UP(next_sram - sram, 12500) + 5 :
			    (sram - next_sram) * 2 / 6250 + 5);
		udelay(max(delay, 25));
		proc = regulator_get_voltage(d->vproc);
		sram = mt6797_sram_get(d);
		if (proc != next_proc || sram != next_sram)
			return -EIO;
	}
	return 0;
}

static int mt6797_sync_dcm(struct mt6797_dvfs *d, int id, unsigned int khz)
{
	struct arm_smccc_res result;
	u32 shift = id == 2 ? 1 : 9 + id * 8, toggle = BIT(shift + 5);
	u32 value = readl(d->mcucfg + 0x744);

	value &= ~toggle;
	arm_smccc_smc(MT6797_MCUSYS_WRITE, d->mcucfg_phys + 0x744, value,
		     0, 0, 0, 0, 0, &result);
	if ((int)result.a0)
		return -EIO;
	value &= ~(31 << shift);
	value |= min(khz / 70000, 31U) << shift;
	value |= toggle;
	arm_smccc_smc(MT6797_MCUSYS_WRITE, d->mcucfg_phys + 0x744, value,
		     0, 0, 0, 0, 0, &result);
	return (int)result.a0 ? -EIO : 0;
}
static int mt6797_posdiv(struct mt6797_dvfs *d, int id, unsigned int div)
{
	u32 shift = 2 + id * 2, saved;
	int ret, restore, top_ret;

	ret = regmap_read(d->top, 0x104, &saved);
	if (ret)
		return ret;
	ret = regmap_update_bits(d->top, 0x104, GENMASK(5, 4), GENMASK(5, 4));
	if (ret)
		return ret;

	ret = mt6797_mcu_update(d, PLL_MUX, 3 << shift, 2 << shift); /* MAINPLL */
	if (ret)
		goto restore_top;
	ret = mt6797_mcu_write(d, PLL_CON1(id),
		(mt6797_mcu_read(d, PLL_CON1(id)) & ~GENMASK(26, 24)) |
		(ilog2(div) << 24) | BIT(31), GENMASK(26, 24));
	udelay(2);
	restore = mt6797_mcu_update(d, PLL_MUX, 3 << shift, 1 << shift); /* ARMPLL */
	ret = ret ?: restore;
restore_top:
	top_ret = regmap_update_bits(d->top, 0x104, GENMASK(5, 4), saved & GENMASK(5, 4));
	return ret ?: top_ret;
}
static int mt6797_hop(struct mt6797_dvfs *d, int id, unsigned int dds)
{
	u32 value, con;
	int ret, err;

	con = mt6797_mcu_read(d, PLL_CON1(id));
	ret = mt6797_mcu_write(d, FH_CFG(id), 0, ~0U);
	if (ret)
		return ret;
	ret = mt6797_mcu_write(d, FH_DDS(id), (con & DDS_MASK) | BIT(31), DDS_MASK);
	if (ret)
		return ret;
	ret = mt6797_mcu_write(d, FH_CFG(id), BIT(2) | BIT(0), ~0U);
	if (ret)
		return ret;
	ret = mt6797_mcu_write(d, FH_BASE + 0xc, 0x06003c97, ~0U);
	if (ret)
		goto stop;
	ret = mt6797_mcu_update(d, FH_BASE, BIT(id), BIT(id));
	if (ret)
		goto stop;
	ret = mt6797_mcu_write(d, FH_DVFS(id), dds | BIT(31), DDS_MASK);
	if (!ret)
		ret = read_poll_timeout_atomic(mt6797_mcu_read, value,
			(value & DDS_MASK) == dds, 10, 1000, false, d, FH_MON(id));
	/* Preserve the reached DDS on timeout; never lower its supply afterwards. */
	value = mt6797_mcu_read(d, FH_MON(id)) & DDS_MASK;
	if (value) {
		err = mt6797_mcu_write(d, PLL_CON1(id),
			(con & ~DDS_MASK) | value | BIT(31), DDS_MASK);
		if (!ret)
			ret = err;
	} else {
		/* The old NCPO remains valid if the controller never started. */
		ret = ret ?: -EIO;
	}
	err = mt6797_mcu_update(d, FH_BASE, BIT(id), 0);
	if (!ret)
		ret = err;
stop:
	err = mt6797_mcu_write(d, FH_CFG(id), 0, ~0U);
	return ret ?: err;
}
static int mt6797_set_frequency(struct mt6797_dvfs *d, int id, unsigned int index)
{
	const struct mt6797_opp *opp = &d->opps[id][index];
	unsigned int old_pos, old_div, old_khz = d->khz[id];
	unsigned long flags;
	u32 con, div, shift = 5 + id * 5;
	int ret;

	if (old_khz == opp->khz) {
		d->index[id] = index;
		return 0;
	}
	if (old_khz > opp->khz) {
		ret = mt6797_sync_dcm(d, id, id == 2 ? 0 : opp->khz);
		if (ret)
			return ret;
	}
	ret = mt6797_mcu_lock(d, &flags);
	if (ret)
		return ret;
	con = mt6797_mcu_read(d, PLL_CON1(id));
	div = (mt6797_mcu_read(d, PLL_DIV) >> shift) & 31;
	old_pos = BIT((con >> 24) & 7);
	if (div != 0 && div != 8 && div != 16 && div != 24 &&
	    div != 10 && div != 11) {
		ret = -EINVAL;
		goto unlock;
	}
	old_div = div == 10 ? 2 : div == 11 ? 4 : 1;
	ret = 0;
	if (old_pos < opp->posdiv)
		ret = mt6797_posdiv(d, id, opp->posdiv);
	if (!ret && old_div < opp->clkdiv)
		ret = mt6797_mcu_update(d, PLL_DIV, 31 << shift,
			(opp->clkdiv == 2 ? 10 : 11) << shift);
	if (!ret)
		ret = mt6797_hop(d, id, (opp->khz * opp->posdiv * opp->clkdiv / 1000 << 14) / 26);
	if (!ret && old_div > opp->clkdiv)
		ret = mt6797_mcu_update(d, PLL_DIV, 31 << shift,
			(opp->clkdiv == 1 ? 8 : 10) << shift);
	if (!ret && old_pos > opp->posdiv)
		ret = mt6797_posdiv(d, id, opp->posdiv);
unlock:
	mt6797_mcu_unlock(d, flags);
	if (ret)
		return ret;
	d->khz[id] = mt6797_pll_khz(d, id);
	if (d->khz[id] != opp->khz)
		return -EIO;
	d->index[id] = index;
	if (old_khz < opp->khz)
		return mt6797_sync_dcm(d, id, opp->khz);
	return 0;
}

static int mt6797_cluster(unsigned int cpu)
{
	return MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1);
}
static unsigned int mt6797_get(unsigned int cpu)
{
	struct mt6797_dvfs *d = mt6797_dvfs;
	int id = mt6797_cluster(cpu);

	return d && id < 2 ? mt6797_pll_khz(d, id) : 0;
}
static int mt6797_target(struct cpufreq_policy *policy, unsigned int index)
{
	struct mt6797_dvfs *d = policy->driver_data;
	int id = mt6797_cluster(policy->cpu), other = 1 - id;
	unsigned int uv, cci = 0, freq;
	int ret, current_uv;

	mutex_lock(&d->lock);
	if (d->fault) {
		ret = -EIO;
		goto out;
	}
	freq = max(d->opps[id][index].khz, d->khz[other]) / 2;
	while (cci < 15 && d->opps[2][cci + 1].khz >= freq)
		cci++;
	uv = max(d->opps[id][index].uv, d->opps[other][d->index[other]].uv);
	uv = max(uv, d->opps[2][cci].uv);
	/* Stock selects the fastest CCI OPP possible at the shared voltage. */
	while (cci && d->opps[2][cci - 1].uv <= uv)
		cci--;
	current_uv = regulator_get_voltage(d->vproc);
	if (current_uv < 0) {
		ret = current_uv;
		goto fault;
	}
	ret = mt6797_set_voltage(d, max(uv, (unsigned int)current_uv));
	if (ret)
		goto fault;
	if (d->khz[2] < d->opps[2][cci].khz) {
		ret = mt6797_set_frequency(d, 2, cci);
		if (ret)
			goto fault;
	}
	ret = mt6797_set_frequency(d, id, index);
	if (ret)
		goto fault;
	ret = mt6797_set_frequency(d, 2, cci);
	if (ret)
		goto fault;
	ret = mt6797_set_voltage(d, uv);
	if (ret)
		goto fault;
	goto out;
fault:
	/* Failed transitions keep the higher rail and inhibit further writes. */
	d->fault = true;
	dev_err(d->dev, "DVFS failed (%d): cluster%d OPP%u; keeping rail, stopping transitions\n",
		ret, id, index);
out:
	mutex_unlock(&d->lock);
	return ret;
}
static int mt6797_policy_init(struct cpufreq_policy *policy)
{
	struct mt6797_dvfs *d = mt6797_dvfs;
	int id = mt6797_cluster(policy->cpu), cpu;

	if (id > 1)
		return -ENODEV;
	policy->driver_data = d;
	cpumask_clear(policy->cpus);
	for_each_possible_cpu(cpu)
		if (mt6797_cluster(cpu) == id)
			cpumask_set_cpu(cpu, policy->cpus);
	policy->freq_table = d->tables[id];
	policy->cpuinfo.transition_latency = 1000000;
	policy->transition_delay_us = 2000;
	return 0;
}
static struct cpufreq_driver mt6797_cpufreq_driver = {
	.name = "mt6797",
	.flags = CPUFREQ_NEED_INITIAL_FREQ_CHECK | CPUFREQ_IS_COOLING_DEV,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = mt6797_target,
	.get = mt6797_get,
	.init = mt6797_policy_init,
	.attr = cpufreq_generic_attr,
};

static int mt6797_dvfs_show(struct seq_file *s, void *unused)
{
	struct mt6797_dvfs *d = s->private;
	int id;

	mutex_lock(&d->lock);
	seq_printf(s, "bin=%u fault=%u VPROC=%d VSRAM=%d uV\n", d->bin, d->fault,
		   regulator_get_voltage(d->vproc), mt6797_sram_get(d));
	for (id = 0; id < 3; id++)
		seq_printf(s, "%s: actual=%u requested=%u kHz OPP=%u\n",
			   id == 0 ? "LL" : id == 1 ? "L" : "CCI",
			   mt6797_pll_khz(d, id), d->khz[id], d->index[id]);
	mutex_unlock(&d->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt6797_dvfs);

static int mt6797_cpufreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_dvfs *d;
	struct nvmem_cell *cell;
	struct resource *res;
	__le32 *buf;
	size_t len;
	unsigned long flags;
	u32 code, mux, pc;
	int id, i, ret, temp, uv, sram;

	if (mt6797_dvfs)
		return -EBUSY;
	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->dev = dev;
	mutex_init(&d->lock);
	spin_lock_init(&d->mcu_lock);
	d->mcu = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(d->mcu))
		return PTR_ERR(d->mcu);
	d->cspm = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(d->cspm))
		return PTR_ERR(d->cspm);
	d->mcucfg = devm_platform_get_and_ioremap_resource(pdev, 2, &res);
	if (IS_ERR(d->mcucfg))
		return PTR_ERR(d->mcucfg);
	d->mcucfg_phys = res->start;
	d->infra = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(d->infra))
		return PTR_ERR(d->infra);
	d->top = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,topckgen");
	if (IS_ERR(d->top))
		return PTR_ERR(d->top);
	d->vproc = devm_regulator_get(dev, "vproc");
	if (IS_ERR(d->vproc))
		return dev_err_probe(dev, PTR_ERR(d->vproc), "CPU rail not ready\n");
	d->thermal = thermal_zone_get_zone_by_name("mt6797-soc");
	if (IS_ERR(d->thermal))
		return dev_err_probe(dev, -EPROBE_DEFER, "waiting for thermal monitor\n");
	ret = thermal_zone_get_temp(d->thermal, &temp);
	if (ret)
		return dev_err_probe(dev, -EPROBE_DEFER, "waiting for valid thermal samples (%d)\n", ret);
	cell = devm_nvmem_cell_get(dev, "speed-bin");
	if (IS_ERR(cell))
		return PTR_ERR(cell);
	buf = nvmem_cell_read(cell, &len);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	code = len == 4 ? le32_to_cpu(*buf) & 15 : 0xff;
	kfree(buf);
	switch (code) {
	case 0:
	case 3:
		d->bin = 0;
		break;
	case 1:
	case 4:
	case 6:
		d->bin = 1;
		break;
	case 2:
	case 7:
		d->bin = 2;
		break;
	case 15:
		d->bin = 3;
		break;
	default: return dev_err_probe(dev, -EINVAL, "unknown factory function code %x\n", code);
	}
	d->opps = mt6797_opps[d->bin];
	/* Do not fight an already running stock hybrid DVFS controller. */
	pc = readl(d->cspm + 0x13c);
	if (pc)
		return dev_err_probe(dev, -EBUSY, "stock DVFSP PCM is running (PC=%08x)\n", pc);
	uv = regulator_get_voltage(d->vproc);
	sram = mt6797_sram_get(d);
	if (uv < 0 || sram < 0 || uv > sram || sram - uv > SRAM_MAX_DIFF_UV)
		return dev_err_probe(dev, -EINVAL, "invalid retained CPU rails: VPROC=%d VSRAM=%d\n", uv, sram);
	ret = mt6797_mcu_lock(d, &flags);
	if (ret)
		return ret;
	mux = mt6797_mcu_read(d, PLL_MUX);
	for (id = 0; id < 3; id++) {
		if (((mux >> (2 + id * 2)) & 3) != 1) {
			ret = -EINVAL;
			break;
		}
		/* Enable/reset only the three active FH channels, retaining all others. */
		ret = mt6797_mcu_update(d, FH_BASE + 4, BIT(id), BIT(id));
		if (!ret)
			ret = mt6797_mcu_update(d, FH_BASE + 8, BIT(id), 0);
		if (!ret)
			ret = mt6797_mcu_update(d, FH_BASE + 8, BIT(id), BIT(id));
		if (!ret)
			ret = mt6797_mcu_write(d, FH_CFG(id), 0, ~0U);
		if (ret)
			break;
	}
	mt6797_mcu_unlock(d, flags);
	if (ret)
		return dev_err_probe(dev, ret, "retained PLL/FHCTL state unsupported\n");
	for (id = 0; id < 3; id++) {
		d->khz[id] = mt6797_pll_khz(d, id);
		if (!d->khz[id] || d->khz[id] > d->opps[id][0].khz)
			return dev_err_probe(dev, -EINVAL, "unsupported retained domain%d frequency %u\n", id, d->khz[id]);
		for (i = 0; i < 15 && d->opps[id][i + 1].khz >= d->khz[id]; i++)
			;
		d->index[id] = i;
		uv = max_t(int, uv, d->opps[id][i].uv);
		if (id < 2) {
			for (i = 0; i < 16; i++)
				d->tables[id][i].frequency = i &&
					d->opps[id][i].khz == d->opps[id][i - 1].khz ?
					CPUFREQ_ENTRY_INVALID : d->opps[id][i].khz;
			d->tables[id][16].frequency = CPUFREQ_TABLE_END;
		}
	}
	ret = mt6797_set_voltage(d, uv);
	if (ret)
		return dev_err_probe(dev, ret, "cannot establish shared CPU supply\n");
	mt6797_dvfs = d;
	ret = cpufreq_register_driver(&mt6797_cpufreq_driver);
	if (ret) {
		mt6797_dvfs = NULL;
		return ret;
	}
	platform_set_drvdata(pdev, d);
	d->debug = debugfs_create_dir("mt6797-dvfs", NULL);
	debugfs_create_file("state", 0444, d->debug, d, &mt6797_dvfs_fops);
	dev_info(dev, "stock bin%u: LL %u / L %u / CCI %u kHz; temperature=%d mC\n",
		 d->bin, d->opps[0][0].khz, d->opps[1][0].khz, d->opps[2][0].khz, temp);
	return 0;
}
static void mt6797_cpufreq_remove(struct platform_device *pdev)
{
	struct mt6797_dvfs *d = platform_get_drvdata(pdev);

	cpufreq_unregister_driver(&mt6797_cpufreq_driver);
	debugfs_remove_recursive(d->debug);
	mt6797_dvfs = NULL;
}
static const struct of_device_id mt6797_cpufreq_ids[] = {
	{ .compatible = "mediatek,mt6797-cpufreq" }, { }
};
MODULE_DEVICE_TABLE(of, mt6797_cpufreq_ids);
static struct platform_driver mt6797_cpufreq_platform_driver = {
	.probe = mt6797_cpufreq_probe,
	.remove = mt6797_cpufreq_remove,
	.driver = { .name = "mt6797-cpufreq", .of_match_table = mt6797_cpufreq_ids },
};
module_platform_driver(mt6797_cpufreq_platform_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6797 stock LL/L/CCI coordinated DVFS");
