/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT6797_EMI_H
#define MT6797_EMI_H

#include <linux/types.h>

#define NO_PROTECTION 0
#define FORBIDDEN 5
#define SET_ACCESS_PERMISSON(d7, d6, d5, d4, d3, d2, d1, d0) \
 (((d7) << 21) | ((d6) << 18) | ((d5) << 15) | ((d4) << 12) | \
  ((d3) << 9) | ((d2) << 6) | ((d1) << 3) | (d0))
int emi_mpu_set_region_protection(u64 start, u64 end, int region, u32 permission);
#endif
