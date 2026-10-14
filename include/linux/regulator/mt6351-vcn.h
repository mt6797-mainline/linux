/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LINUX_MT6351_VCN_H
#define LINUX_MT6351_VCN_H

#include <linux/types.h>

/* MT6351 upmu_hw.h: the ON_CTRL field is bit 3 of each CON register. */
#define MT6351_VCN18_CON 0x0a52
#define MT6351_VCN28_CON 0x0a0c
#define MT6351_VCN33_BT_CON 0x0a98
#define MT6351_VCN33_WIFI_CON 0x0a9a

int mt6351_vcn_set_hw_control(unsigned int reg, bool enable);

#endif
