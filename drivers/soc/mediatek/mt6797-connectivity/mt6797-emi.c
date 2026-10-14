// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 stock ATF EMI MPU ABI, from the vendor mt_secure_api.h. */
#include <linux/arm-smccc.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include "mt6797-emi.h"

static DEFINE_SPINLOCK(mt6797_emi_lock);

int emi_mpu_set_region_protection(u64 start, u64 end, int region, u32 permission)
{
	struct arm_smccc_res res;
	unsigned long flags;

	if (region < 0 || region > 31 || start > end || end > U32_MAX)
		return -EINVAL;
	permission = (permission & 0xffffff) | ((region & 0x1f) << 27);
	spin_lock_irqsave(&mt6797_emi_lock, flags);
	arm_smccc_smc(0x82000209, start, end, permission, 0, 0, 0, 0, &res);
	spin_unlock_irqrestore(&mt6797_emi_lock, flags);
	return (int)res.a0;
}

EXPORT_SYMBOL_GPL(emi_mpu_set_region_protection);
