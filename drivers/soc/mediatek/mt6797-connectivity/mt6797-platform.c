// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 power sequencing from the active stock CCF clk-mt6797-pg.c. */
#include <linux/clk-provider.h>
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

struct mt6797_conn_gate {
	struct clk_hw hw;
	void __iomem *infra;
	u32 set, clear, status;
	u8 bit;
};

static int mt6797_gate_enable(struct clk_hw *hw)
{
	struct mt6797_conn_gate *gate = container_of(hw, struct mt6797_conn_gate, hw);
	writel(BIT(gate->bit), gate->infra + gate->clear);
	return 0;
}

static void mt6797_gate_disable(struct clk_hw *hw)
{
	struct mt6797_conn_gate *gate = container_of(hw, struct mt6797_conn_gate, hw);
	writel(BIT(gate->bit), gate->infra + gate->set);
}

static int mt6797_gate_is_enabled(struct clk_hw *hw)
{
	struct mt6797_conn_gate *gate = container_of(hw, struct mt6797_conn_gate, hw);
	return !(readl(gate->infra + gate->status) & BIT(gate->bit));
}

static const struct clk_ops mt6797_conn_gate_ops = {
	.enable = mt6797_gate_enable,
	.disable = mt6797_gate_disable,
	.is_enabled = mt6797_gate_is_enabled,
};

int mt6797_consys_register_gates(struct device *dev)
{
	struct clk_hw_onecell_data *data;
	void __iomem *infra = (void __iomem *)(conn_reg.topckgen_base + 0x1000);
	int ret;

	data = devm_kzalloc(dev, struct_size(data, hws, 2), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->num = 2;
	/* Stock set/clear aliases; CG_CON is read for status only. */
	for (ret = 0; ret < 2; ret++) {
		struct mt6797_conn_gate *gate;
		struct clk_init_data init = {
			.name = ret ? "mt6797-connsys-ap-dma" : "mt6797-connsys-btif",
			.ops = &mt6797_conn_gate_ops,
			.flags = CLK_IGNORE_UNUSED,
		};
		int err;
		gate = devm_kzalloc(dev, sizeof(*gate), GFP_KERNEL);
		if (!gate)
			return -ENOMEM;
		gate->infra = infra;
		gate->set = ret ? 0x88 : 0x80;
		gate->clear = ret ? 0x8c : 0x84;
		gate->status = ret ? 0x94 : 0x90;
		gate->bit = ret ? 18 : 31;
		gate->hw.init = &init;
		err = devm_clk_hw_register(dev, &gate->hw);
		if (err)
			return err;
		data->hws[ret] = &gate->hw;
	}
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
	if (ret)
		return ret;
	mt6797_consys_dev = dev;
	dev_info(dev, "MT6797 CONSYS resources ready; BTIF/AP DMA gates only\n");
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
module_init(mt6797_consys_init);
module_exit(mt6797_consys_exit);
MODULE_SOFTDEP("pre: mt6351_vcn");
MODULE_DESCRIPTION("MT6797 CONSYS platform control");
MODULE_LICENSE("GPL");
