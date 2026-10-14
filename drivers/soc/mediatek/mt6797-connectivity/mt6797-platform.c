// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 power sequencing from the active stock CCF clk-mt6797-pg.c. */
#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_clk.h>
#include <linux/platform_device.h>
#include "mt6797-native.h"
#include "mtk_wcn_consys_hw.h"

static DEFINE_MUTEX(mt6797_power_lock);
static DEFINE_SPINLOCK(mt6797_reset_lock);
static struct device *mt6797_consys_dev;

int mt6797_consys_ready(void)
{
	return mt6797_consys_dev ? 0 : -ENODEV;
}
EXPORT_SYMBOL_GPL(mt6797_consys_ready);

bool mt6797_has_gps_lna(void)
{
	return mt6797_consys_dev &&
		!of_property_read_bool(mt6797_consys_dev->of_node, "mediatek,no-gps-lna");
}

static u32 conn_status(void __iomem *spm)
{
	return (readl(spm + 0x180) & BIT(1) ? 1 : 0) |
	       (readl(spm + 0x184) & BIT(1) ? 2 : 0);
}

static void conn_update(void __iomem *base, u32 offset, u32 mask, bool set)
{
	u32 value = readl(base + offset);
	writel(set ? value | mask : value & ~mask, base + offset);
}

int mt6797_conn_power(bool on)
{
	void __iomem *spm = (void __iomem *)conn_reg.spm_base;
	void __iomem *infra = (void __iomem *)(conn_reg.topckgen_base + 0x1000);
	const u32 protect = BIT(17) | BIT(18);
	u32 value;
	int ret;

	if (mt6797_consys_ready())
		return -ENODEV;
	mutex_lock(&mt6797_power_lock);
	writel(0x0b160001, spm);
	if (on) {
		conn_update(spm, 0x32c, BIT(2), true);
		conn_update(spm, 0x32c, BIT(3), true);
		ret = read_poll_timeout(conn_status, value, value == 3,
				       10, 200000, false, spm);
		if (ret)
			goto out;
		conn_update(spm, 0x32c, BIT(4), false);
		conn_update(spm, 0x32c, BIT(1), false);
		conn_update(spm, 0x32c, BIT(0), true);
		conn_update(infra, 0x220, protect, false);
		ret = readl_poll_timeout(infra + 0x228, value, !(value & protect), 10, 200000);
	} else {
		conn_update(infra, 0x220, protect, true);
		ret = readl_poll_timeout(infra + 0x228, value,
					(value & protect) == protect, 10, 200000);
		/* Never cut power while outstanding bus traffic is unprotected. */
		if (ret)
			goto out;
		conn_update(spm, 0x32c, BIT(1), true);
		conn_update(spm, 0x32c, BIT(4), true);
		conn_update(spm, 0x32c, BIT(0), false);
		conn_update(spm, 0x32c, BIT(2), false);
		conn_update(spm, 0x32c, BIT(3), false);
		ret = read_poll_timeout(conn_status, value, !value, 10, 200000, false, spm);
	}
out:
	if (ret)
		dev_err(mt6797_consys_dev,
			"CONSYS power %s timed out: control=%08x ack=%08x/%08x protect=%08x/%08x\n",
			on ? "on" : "off", readl(spm + 0x32c), readl(spm + 0x180),
			readl(spm + 0x184), readl(infra + 0x220), readl(infra + 0x228));
	mutex_unlock(&mt6797_power_lock);
	return ret;
}

int mtk_wdt_swsysret_config(int mask, int set)
{
	void __iomem *rgu = (void __iomem *)conn_reg.ap_rgu_base;
	unsigned long flags;
	u32 value;

	if (mask != BIT(12) || set < 0 || set > 1)
		return -EINVAL;
	spin_lock_irqsave(&mt6797_reset_lock, flags);
	value = readl(rgu + 0x18) & 0x00ffffff;
	value = set ? value | mask : value & ~mask;
	writel(value | 0x88000000, rgu + 0x18);
	mb();
	spin_unlock_irqrestore(&mt6797_reset_lock, flags);
	return 0;
}

int mt6797_consys_register_gates(struct device *dev)
{
	/* I2C and WMT share AP DMA; the common clock provider owns its gate. */
	struct clk *btif, *dma;

	btif = devm_clk_get(dev, "btifc");
	if (IS_ERR(btif))
		return dev_err_probe(dev, PTR_ERR(btif), "BTIF clock unavailable\n");
	dma = devm_clk_get(dev, "apdmac");
	if (IS_ERR(dma))
		return dev_err_probe(dev, PTR_ERR(dma), "AP DMA clock unavailable\n");

	mt6797_consys_dev = dev;
	dev_info(dev, "MT6797 CONSYS resources ready; shared BTIF/AP DMA gates\n");
	return 0;
}

static int __init mt6797_consys_init(void)
{
	return mtk_wcn_consys_hw_init();
}

static void __exit mt6797_consys_exit(void)
{
	mtk_wcn_consys_hw_deinit();
	mt6797_consys_dev = NULL;
}
#ifdef MODULE
module_init(mt6797_consys_init);
#else
late_initcall(mt6797_consys_init);
#endif
module_exit(mt6797_consys_exit);
MODULE_SOFTDEP("pre: mt6351_vcn");
MODULE_DESCRIPTION("MT6797 CONSYS platform control");
MODULE_LICENSE("GPL");
