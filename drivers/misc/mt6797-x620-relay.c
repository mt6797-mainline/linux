// SPDX-License-Identifier: GPL-2.0-only
/* Temporary diagnostics for the X620 stock LK -> U-Boot -> Linux path. */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/sizes.h>
#include <linux/workqueue.h>

#define MT6797_X620_LOG_BASE 0x444bf000
#define MT6797_X620_LOG_SIGNATURE 0x43474244
#define MT6797_X620_LOG_HEADER 12

static struct earlycon_device *ram_console;
static unsigned int log_capacity = SZ_4K - MT6797_X620_LOG_HEADER;

static void mt6797_x620_ram_write(struct console *con, const char *s, unsigned int count)
{
	void __iomem *base = ram_console->port.membase;
	u32 start = readl(base + 4), size = readl(base + 8);

	if (start > log_capacity || size > log_capacity)
		return;
	/* Before normal ioremap is available retain the first page, not a ring. */
	if (log_capacity == SZ_4K - MT6797_X620_LOG_HEADER)
		count = min(count, log_capacity - size);
	while (count--) {
		writeb(*s++, base + MT6797_X620_LOG_HEADER + start);
		start++;
		if (log_capacity != SZ_4K - MT6797_X620_LOG_HEADER)
			start %= log_capacity;
		size = min(size + 1, log_capacity);
	}
	wmb();
	writel(start, base + 4);
	writel(size, base + 8);
}

static int __init mt6797_x620_ram_setup(struct earlycon_device *dev, const char *options)
{
	if (!initial_boot_params ||
	    fdt_node_check_compatible(initial_boot_params, 0, "leeco,x620") ||
	    dev->port.mapbase != MT6797_X620_LOG_BASE || !dev->port.membase)
		return -ENODEV;
	ram_console = dev;
	writel(MT6797_X620_LOG_SIGNATURE, dev->port.membase);
	writel(0, dev->port.membase + 4);
	writel(0, dev->port.membase + 8);
	dev->con->write = mt6797_x620_ram_write;
	return 0;
}
EARLYCON_DECLARE(mt6797ram, mt6797_x620_ram_setup);
/* Retain the early console name used by existing relay images. */
EARLYCON_DECLARE(x620ram, mt6797_x620_ram_setup);

static int __init mt6797_x620_ram_expand(void)
{
	void __iomem *base;

	if (!ram_console)
		return 0;
	base = ioremap(MT6797_X620_LOG_BASE, SZ_64K);
	if (!base)
		return -ENOMEM;
	console_lock();
	/* The old bounded page has no wrapped data; preserve its write offset. */
	writel(readl(base + 8), base + 4);
	ram_console->port.membase = base;
	log_capacity = SZ_64K - MT6797_X620_LOG_HEADER;
	console_unlock();
	pr_info("X620_RELAY: persistent console expanded to 64 KiB\n");
	return 0;
}
early_initcall(mt6797_x620_ram_expand);

struct mt6797_x620_refresh {
	void __iomem *ovl, *dsi, *mutex;
	struct delayed_work work;
	u32 framebuffer;
};

static void mt6797_x620_refresh_work(struct work_struct *work)
{
	struct mt6797_x620_refresh *p = container_of(to_delayed_work(work),
					    struct mt6797_x620_refresh, work);
	u32 start;

	/* Stop if another owner reprograms layer 0. Never wait for DSI BUSY. */
	if (readl(p->ovl + 0xf40) != p->framebuffer)
		return;
	if (!(readl(p->dsi + 0x0c) & BIT(31))) {
		wmb();
		writel(1, p->mutex + 0x20);
		start = readl(p->dsi);
		writel(start & ~BIT(0), p->dsi);
		writel(start | BIT(0), p->dsi);
	}
	schedule_delayed_work(&p->work, msecs_to_jiffies(100));
}

static void mt6797_x620_refresh_stop(void *data)
{
	struct mt6797_x620_refresh *p = data;

	cancel_delayed_work_sync(&p->work);
}

static int mt6797_x620_refresh_probe(struct platform_device *pdev)
{
	struct mt6797_x620_refresh *p;
	int ret;

	if (!of_machine_is_compatible("leeco,x620"))
		return -ENODEV;
	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->ovl = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->ovl))
		return PTR_ERR(p->ovl);
	p->dsi = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(p->dsi))
		return PTR_ERR(p->dsi);
	p->mutex = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(p->mutex))
		return PTR_ERR(p->mutex);
	ret = of_property_read_u32(pdev->dev.of_node, "leeco,framebuffer", &p->framebuffer);
	if (ret)
		return ret;
	if (p->framebuffer != 0xbd0c0000 ||
	    !(readl(p->ovl + 0x0c) & BIT(0)) ||
	    !(readl(p->ovl + 0x2c) & BIT(0)) ||
	    readl(p->ovl + 0xf40) != p->framebuffer ||
	    (readl(p->ovl + 0x44) & 0xffff) != 4352 ||
	    (readl(p->dsi + 0x14) & 3) ||
	    readl(p->dsi + 0x60) != 1 || readl(p->dsi + 0x200) != 0x002c3909)
		return dev_err_probe(&pdev->dev, -ENODEV, "unexpected retained display state\n");
	writel(BIT(0), p->ovl + 0x2c);
	writel(readl(p->ovl + 0x30) & ~BIT(8), p->ovl + 0x30);
	INIT_DELAYED_WORK(&p->work, mt6797_x620_refresh_work);
	ret = devm_add_action_or_reset(&pdev->dev, mt6797_x620_refresh_stop, p);
	if (ret)
		return ret;
	schedule_delayed_work(&p->work, 0);
	dev_info(&pdev->dev, "X620_RELAY: retained command-mode display refresh active\n");
	return 0;
}

static const struct of_device_id mt6797_x620_refresh_ids[] = {
	{ .compatible = "leeco,x620-lk-refresh" },
	{ }
};
static struct platform_driver mt6797_x620_refresh_driver = {
	.probe = mt6797_x620_refresh_probe,
	.driver = {
		.name = "x620-lk-refresh",
		.of_match_table = mt6797_x620_refresh_ids,
	},
};
builtin_platform_driver(mt6797_x620_refresh_driver);
