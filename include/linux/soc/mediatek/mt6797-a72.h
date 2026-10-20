/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6797_A72_H
#define __LINUX_SOC_MEDIATEK_MT6797_A72_H

#include <linux/kconfig.h>
#include <linux/errno.h>
#include <linux/of.h>

static inline bool mt6797_a72_late_boot(void)
{
	return IS_ENABLED(CONFIG_MTK_MT6797_A72) &&
		of_machine_is_compatible("mediatek,mt6797");
}

#if IS_ENABLED(CONFIG_MTK_MT6797_A72)
int mt6797_a72_prepare(unsigned int cpu);
void mt6797_a72_finish(unsigned int cpu, int error);
void mt6797_a72_power_off(unsigned int cpu);
int mt6797_a72_get_voltage(void);
int mt6797_a72_get_sram_voltage(void);
int mt6797_a72_set_voltage(unsigned int uv);
#else
static inline int mt6797_a72_prepare(unsigned int cpu) { return 0; }
static inline void mt6797_a72_finish(unsigned int cpu, int error) { }
static inline void mt6797_a72_power_off(unsigned int cpu) { }
static inline int mt6797_a72_get_voltage(void) { return -ENODEV; }
static inline int mt6797_a72_get_sram_voltage(void) { return -ENODEV; }
static inline int mt6797_a72_set_voltage(unsigned int uv) { return -ENODEV; }
#endif

#endif
