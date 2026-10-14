/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT6797_CONNSYS_NATIVE_H
#define MT6797_CONNSYS_NATIVE_H

#include <linux/io.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/mt6351-vcn.h>
#include "mt6797-emi.h"

static inline void mt6797_sync_writel(u32 value, void __iomem *address)
{
	writel(value, address);
	mb();
}

#define mt_reg_sync_writel(value, address) \
	mt6797_sync_writel((value), (void __iomem *)(address))

#define MT6351_PMIC_RG_VCN18_ON_CTRL MT6351_VCN18_CON
#define MT6351_PMIC_RG_VCN28_ON_CTRL MT6351_VCN28_CON
#define MT6351_PMIC_RG_VCN33_ON_CTRL_BT MT6351_VCN33_BT_CON
#define MT6351_PMIC_RG_VCN33_ON_CTRL_WIFI MT6351_VCN33_WIFI_CON

static inline int pmic_set_register_value(unsigned int field, unsigned int value)
{
	if (value > 1)
		return -EINVAL;
	return mt6351_vcn_set_hw_control(field, value);
}

int mt6797_consys_ready(void);
int mt6797_consys_read_cpupcr(u32 *value);
int mt6797_consys_register_gates(struct device *dev);
int mt6797_conn_power(bool on);
int mtk_wdt_swsysret_config(int mask, int set);
bool mt6797_has_gps_lna(void);

#endif
