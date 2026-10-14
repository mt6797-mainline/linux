// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 factory devinfo words supplied by LK in its reserved hardware DT. */
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#define MT6797_DEVINFO_WORDS 100

static int mt6797_devinfo_read(void *context, unsigned int offset,
			       void *value, size_t bytes)
{
	memcpy(value, context + offset, bytes);
	return 0;
}

static int mt6797_devinfo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nvmem_config config = { };
	struct device_node *np;
	struct resource res;
	struct nvmem_device *nvmem;
	const __le32 *tag;
	void *fdt, *words;
	int node, len, ret;

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np)
		return -EINVAL;
	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret)
		return ret;
	fdt = memremap(res.start, resource_size(&res), MEMREMAP_WB);
	if (!fdt)
		return -ENOMEM;
	ret = fdt_check_header(fdt);
	if (ret || fdt_totalsize(fdt) > resource_size(&res)) {
		ret = -EINVAL;
		goto unmap;
	}
	node = fdt_path_offset(fdt, "/chosen");
	tag = node < 0 ? NULL : fdt_getprop(fdt, node, "atag,devinfo", &len);
	if (!tag || len != (MT6797_DEVINFO_WORDS + 3) * sizeof(*tag) ||
	    le32_to_cpu(tag[0]) != MT6797_DEVINFO_WORDS + 3 ||
	    le32_to_cpu(tag[MT6797_DEVINFO_WORDS + 2]) != MT6797_DEVINFO_WORDS) {
		ret = -EINVAL;
		goto unmap;
	}
	words = devm_kmemdup(dev, tag + 2, MT6797_DEVINFO_WORDS * sizeof(*tag), GFP_KERNEL);
	if (!words) {
		ret = -ENOMEM;
		goto unmap;
	}
	memunmap(fdt);
	config.name = "mt6797-devinfo";
	config.dev = dev;
	config.owner = THIS_MODULE;
	config.size = MT6797_DEVINFO_WORDS * sizeof(*tag);
	config.word_size = 1;
	config.stride = 1;
	config.read_only = true;
	config.reg_read = mt6797_devinfo_read;
	config.priv = words;
	config.add_legacy_fixed_of_cells = true;
	nvmem = devm_nvmem_register(dev, &config);
	if (IS_ERR(nvmem))
		return PTR_ERR(nvmem);
	dev_info(dev, "100 factory devinfo words from live LK handoff\n");
	return 0;
unmap:
	memunmap(fdt);
	return dev_err_probe(dev, ret, "invalid LK devinfo tag\n");
}

static const struct of_device_id mt6797_devinfo_ids[] = {
	{ .compatible = "mediatek,mt6797-lk-devinfo" }, { }
};
MODULE_DEVICE_TABLE(of, mt6797_devinfo_ids);
static struct platform_driver mt6797_devinfo_driver = {
	.probe = mt6797_devinfo_probe,
	.driver = { .name = "mt6797-lk-devinfo", .of_match_table = mt6797_devinfo_ids },
};
module_platform_driver(mt6797_devinfo_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6797 LK factory calibration and silicon bin NVMEM");
