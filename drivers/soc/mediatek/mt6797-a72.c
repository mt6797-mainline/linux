// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 stock big-cluster supply preparation around PSCI CPU_ON/OFF. */
#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/psci.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/soc/mediatek/mt6797-a72.h>
#include <uapi/linux/psci.h>
#include <asm/smp_plat.h>

#define SPM_PCM_RESERVE		0x218
#define SPM_EXT_BUCK_ISO		0x290
#define MP2_PLL_CON0		0x4a0
#define MP2_PLL_CON1		0x4a4
#define SIP_IDVFS_READ		0xc200035f
#define SIP_IDVFS_SRAM_SET	0xc20003bf
#define MP2_PHYS		0x10222000

struct mt6797_a72 {
	struct device *dev;
	void __iomem *mcu, *spm;
	struct regulator *vproc;
	struct reset_control *reset;
	struct mutex lock;
	unsigned int prepared;
	bool fault;
};

/* Built-in and not unbindable: PSCI can call this after secondary CPUs die. */
static struct mt6797_a72 *mt6797_big;

static bool mt6797_is_a72(unsigned int cpu)
{
	return of_machine_is_compatible("mediatek,mt6797") &&
		MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1) == 2;
}

static long mt6797_big_smc(unsigned int function, unsigned long arg)
{
	struct arm_smccc_res res;

	arm_smccc_smc(function, arg, 0, 0, 0, 0, 0, 0, &res);
	/* Stock SiP services return a signed 32-bit result in x0. */
	return (s32)res.a0;
}

static void mt6797_big_update(void __iomem *base, u32 reg, u32 mask, u32 value)
{
	writel((readl(base + reg) & ~mask) | value, base + reg);
	readl(base + reg);
}

static int mt6797_big_sram_get(void)
{
	u32 value = mt6797_big_smc(SIP_IDVFS_READ, MP2_PHYS + 0x2b0);
	unsigned int code = value & 15;

	if ((value & 0xff0) != 0x8f0)
		return -EIO;
	if (!code)
		return 1050000;
	if (code < 3)
		return -ERANGE;
	return 900000 + (code - 3) * 25000;
}

int mt6797_a72_get_voltage(void)
{
	struct mt6797_a72 *d = mt6797_big;
	int ret;

	if (!d)
		return -ENODEV;
	mutex_lock(&d->lock);
	ret = d->prepared ? regulator_get_voltage(d->vproc) : -ENODEV;
	mutex_unlock(&d->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_get_voltage);

int mt6797_a72_get_sram_voltage(void)
{
	struct mt6797_a72 *d = mt6797_big;
	int ret;

	if (!d)
		return -ENODEV;
	mutex_lock(&d->lock);
	ret = d->prepared ? mt6797_big_sram_get() : -ENODEV;
	mutex_unlock(&d->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_get_sram_voltage);

/* Match the stock DA9214/SRAM ramp and its 0..300 mV headroom invariant. */
int mt6797_a72_set_voltage(unsigned int uv)
{
	struct mt6797_a72 *d = mt6797_big;
	int proc, sram, next_proc, next_sram, delay, ret = 0;
	long smc;

	if (!d)
		return -ENODEV;
	if (uv < 800000 || uv > 1200000 || uv % 10000)
		return -EINVAL;
	mutex_lock(&d->lock);
	if (!d->prepared) {
		ret = -ENODEV;
		goto out;
	}
	proc = regulator_get_voltage(d->vproc);
	sram = mt6797_big_sram_get();
	if (d->fault || proc < 800000 || sram < 1000000 ||
	    proc > sram || sram - proc > 300000) {
		ret = -EIO;
		goto fault;
	}
	while (proc != uv) {
		if (uv > proc) {
			next_sram = clamp(roundup(min(proc + 275000, (int)uv + 100000), 25000),
					  1000000, 1200000);
			smc = mt6797_big_smc(SIP_IDVFS_SRAM_SET, next_sram / 10);
			if (smc < 0 || mt6797_big_sram_get() != next_sram) {
				ret = -EIO;
				goto fault;
			}
			next_proc = min((int)uv, next_sram == 1200000 ? (int)uv :
					 next_sram - 100000);
			next_proc = roundup(next_proc, 10000);
			ret = regulator_set_voltage(d->vproc, next_proc, next_proc);
		} else {
			next_proc = roundup(max((int)uv, sram - 275000), 10000);
			ret = regulator_set_voltage(d->vproc, next_proc, next_proc);
			if (ret)
				goto fault;
			next_sram = clamp(roundup(next_proc + 100000, 25000),
					  1000000, 1200000);
			smc = mt6797_big_smc(SIP_IDVFS_SRAM_SET, next_sram / 10);
			if (smc < 0 || mt6797_big_sram_get() != next_sram) {
				ret = -EIO;
				goto fault;
			}
		}
		if (ret)
			goto fault;
		delay = max(abs(next_proc - proc) / 10000 + 1,
			    next_sram >= sram ? DIV_ROUND_UP(next_sram - sram, 12500) + 5 :
			    (sram - next_sram) * 2 / 6250 + 5);
		udelay(max(delay, 25));
		proc = regulator_get_voltage(d->vproc);
		sram = mt6797_big_sram_get();
		if (proc != next_proc || sram != next_sram) {
			ret = -EIO;
			goto fault;
		}
	}
	goto out;
fault:
	d->fault = true;
	dev_err(d->dev, "big-cluster rail transition failed (%d), retaining reached rails\n", ret);
out:
	mutex_unlock(&d->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_a72_set_voltage);

int mt6797_a72_prepare(unsigned int cpu)
{
	struct mt6797_a72 *d = mt6797_big;
	unsigned int bit;
	u32 iso;
	int ret;
	long smc;

	if (!mt6797_is_a72(cpu))
		return 0;
	if (!d) {
		pr_info("MT6797_A72: CPU%u deferred until the regulator/reset drivers are ready\n",
			cpu);
		return -EPERM;
	}
	bit = BIT(MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0));
	mutex_lock(&d->lock);
	ret = -EIO;
	if (d->fault)
		goto out;
	if (d->prepared) {
		d->prepared |= bit;
		ret = 0;
		goto out;
	}

	dev_info(d->dev, "CPU%u: stock big-cluster supply preparation\n", cpu);
	/* Match cpu_power_on_buck() in the factory MT6797 PSCI driver. */
	mt6797_big_update(d->spm, SPM_PCM_RESERVE, BIT(0), BIT(0));
	readl(d->mcu + MP2_PLL_CON0); /* Factory dummy read before reset latch. */
	iso = readl(d->spm + SPM_EXT_BUCK_ISO) & 3;
	ret = reset_control_assert(d->reset);
	if (ret)
		goto out;
	ret = regulator_set_voltage(d->vproc, 1000000, 1000000);
	if (ret)
		goto unlatch;
	ret = regulator_enable(d->vproc);
	if (ret)
		goto unlatch;
	udelay(1000);
	mt6797_big_update(d->spm, SPM_EXT_BUCK_ISO, 3, 0);
	ret = reset_control_deassert(d->reset);
	if (ret) {
		/* Keep the supply if the hardware latch could not be released. */
		d->fault = true;
		goto out;
	}
	udelay(240);
	smc = mt6797_big_smc(SIP_IDVFS_SRAM_SET, 110000);
	if (smc < 0) {
		dev_err(d->dev, "SRAM LDO SiP call failed: %ld\n", smc);
		ret = -EIO;
		/* No CPU_ON was issued: restore isolation before removing supply. */
		mt6797_big_update(d->spm, SPM_EXT_BUCK_ISO, 3, iso);
		if (regulator_disable(d->vproc))
			d->fault = true;
		goto out;
	}
	udelay(240);
	smc = mt6797_big_smc(SIP_IDVFS_READ, MP2_PHYS + 0x2b0);
	if ((smc & 0xfff) != 0x8fb) {
		dev_err(d->dev, "SRAM LDO readback failed: %08lx\n", smc);
		mt6797_big_update(d->spm, SPM_EXT_BUCK_ISO, 3, iso);
		if (regulator_disable(d->vproc))
			d->fault = true;
		ret = -EIO;
		goto out;
	}
	d->prepared = bit;
	dev_info(d->dev, "CPU%u: VPROC2=%d uV VSRAM=1100000 uV; ready for PSCI\n",
		 cpu, regulator_get_voltage(d->vproc));
	ret = 0;
	goto out;

unlatch:
	if (reset_control_deassert(d->reset))
		d->fault = true;
out:
	mutex_unlock(&d->lock);
	return ret;
}

void mt6797_a72_power_off(unsigned int cpu)
{
	struct mt6797_a72 *d = mt6797_big;
	unsigned int bit;

	if (!mt6797_is_a72(cpu) || !d)
		return;
	bit = BIT(MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0));
	mutex_lock(&d->lock);
	if (!(d->prepared & bit))
		goto out;
	d->prepared &= ~bit;
	if (!d->prepared) {
		/* Called only after PSCI AFFINITY_INFO has confirmed CPU_OFF. */
		if (regulator_disable(d->vproc)) {
			d->fault = true;
			dev_err(d->dev, "failed to disable last big-cluster supply\n");
		} else {
			dev_info(d->dev, "CPU%u off: last A72 supply released\n", cpu);
		}
	}
out:
	mutex_unlock(&d->lock);
}

void mt6797_a72_finish(unsigned int cpu, int error)
{
	struct mt6797_a72 *d = mt6797_big;
	u32 con0, con1;
	u64 khz;

	if (!mt6797_is_a72(cpu) || !d)
		return;
	if (error) {
		/* A denied/duplicate CPU_ON must never cut power to a running core. */
		if (psci_ops.affinity_info &&
		    psci_ops.affinity_info(cpu_logical_map(cpu), 0) ==
		    PSCI_0_2_AFFINITY_LEVEL_OFF)
			mt6797_a72_power_off(cpu);
		return;
	}
	mutex_lock(&d->lock);
	/* Retain the firmware's initial PLL/DCM state during first CPU bring-up. */
	con0 = mt6797_big_smc(SIP_IDVFS_READ, MP2_PHYS + MP2_PLL_CON0);
	con1 = mt6797_big_smc(SIP_IDVFS_READ, MP2_PHYS + MP2_PLL_CON1);
	khz = (u64)(con1 & GENMASK(30, 0)) * 26000;
	khz >>= 24 + ((con0 >> 12) & 7);
	dev_info(d->dev, "CPU%u: PSCI returned; PLL=%llu kHz con0=%08x con1=%08x\n",
		 cpu, khz, con0, con1);
	mutex_unlock(&d->lock);
}

static int mt6797_a72_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6797_a72 *d;
	unsigned int cpu;
	int ret;

	if (mt6797_big)
		return -EBUSY;
	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->dev = dev;
	mutex_init(&d->lock);
	d->mcu = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(d->mcu))
		return PTR_ERR(d->mcu);
	d->spm = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(d->spm))
		return PTR_ERR(d->spm);
	d->vproc = devm_regulator_get(dev, "vproc");
	if (IS_ERR(d->vproc))
		return dev_err_probe(dev, PTR_ERR(d->vproc), "VPROC2 not ready\n");
	d->reset = devm_reset_control_get_exclusive(dev, "pwrap-spi");
	if (IS_ERR(d->reset))
		return dev_err_probe(dev, PTR_ERR(d->reset), "PWRAP latch not ready\n");
	mt6797_big = d;
	dev_info(dev, "stock A72 power sequence ready\n");
	/* The early SMP pass cannot use I2C regulators. Retry once they exist. */
	for_each_possible_cpu(cpu) {
		if (!mt6797_is_a72(cpu) || cpu_online(cpu) ||
		    num_online_cpus() >= setup_max_cpus)
			continue;
		if (!get_cpu_device(cpu)) {
			dev_err(dev, "CPU%u device is unavailable\n", cpu);
			continue;
		}
		ret = add_cpu(cpu);
		if (ret)
			dev_err(dev, "deferred CPU%u startup failed: %d\n", cpu, ret);
	}
	return 0;
}

static const struct of_device_id mt6797_a72_ids[] = {
	{ .compatible = "mediatek,mt6797-a72-power" },
	{ }
};

static struct platform_driver mt6797_a72_driver = {
	.probe = mt6797_a72_probe,
	.driver = {
		.name = "mt6797-a72",
		.of_match_table = mt6797_a72_ids,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6797_a72_driver);
