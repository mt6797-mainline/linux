/*
* Copyright (C) 2016 MediaTek Inc.
*
* This program is free software: you can redistribute it and/or modify it under the terms of the
* GNU General Public License version 2 as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
* without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See the GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with this program.
* If not, see <http://www.gnu.org/licenses/>.
*/

/*! \file
    \brief  Declaration of library functions

    Any definitions in this file will be shared among GLUE Layer and internal Driver Stack.
*/

/*******************************************************************************
*                         C O M P I L E R   F L A G S
********************************************************************************
*/

/*******************************************************************************
*                                 M A C R O S
********************************************************************************
*/

#ifdef DFT_TAG
#undef DFT_TAG
#endif
#define DFT_TAG "[WMT-CONSYS-HW]"

/*******************************************************************************
*                    E X T E R N A L   R E F E R E N C E S
********************************************************************************
*/
#if defined(CONFIG_MTK_CLKMGR)
#include <mt_clkmgr.h>
#else
#include <linux/clk.h>
#endif /* defined(CONFIG_MTK_CLKMGR) */
#include <linux/delay.h>
#include <linux/memblock.h>
#include <linux/iopoll.h>
#include "osal_typedef.h"
#include "mtk_wcn_consys_hw.h"

#if CONSYS_EMI_MPU_SETTING
#include "mt6797-emi.h"
#endif

#if CONSYS_PMIC_CTRL_ENABLE
#include "mt6797-native.h"
#include <linux/regulator/consumer.h>
#endif

#ifdef CONFIG_MTK_HIBERNATION
#include <mtk_hibernate_dpm.h>
#endif

#include <linux/of_reserved_mem.h>

#if CONSYS_CLOCK_BUF_CTRL

#endif

/*******************************************************************************
*                              C O N S T A N T S
********************************************************************************
*/

/*******************************************************************************
*                             D A T A   T Y P E S
********************************************************************************
*/

/*******************************************************************************
*                  F U N C T I O N   D E C L A R A T I O N S
********************************************************************************
*/
static INT32 mtk_wmt_probe(struct platform_device *pdev);
static void mtk_wmt_remove(struct platform_device *pdev);

/*******************************************************************************
*                            P U B L I C   D A T A
********************************************************************************
*/
static DEFINE_MUTEX(consys_sequence_lock);
static DEFINE_MUTEX(consys_init_lock);
enum consys_power_state { CONSYS_OFF, CONSYS_STARTING, CONSYS_ON, CONSYS_FAULT };
static enum consys_power_state consys_state;
static bool consys_vcn18_held, consys_vcn28_held, consys_domain_pending;
static bool consys_vcn28_hw_control;
static unsigned int consys_init_users;

UINT8 __iomem *pEmibaseaddr = NULL;
phys_addr_t gConEmiPhyBase;
EXPORT_SYMBOL_GPL(gConEmiPhyBase);
struct CONSYS_BASE_ADDRESS conn_reg;

/* CCF part */
#if !defined(CONFIG_MTK_CLKMGR)
struct clk *clk_scp_conn_main;	/*ctrl conn_power_on/off */
#if CONSYS_AHB_CLK_MAGEMENT
struct clk *clk_infra_conn_main;	/*ctrl infra_connmcu_bus clk */
#endif
#endif /* !defined(CONFIG_MTK_CLKMGR) */

#ifdef CONFIG_OF
static const struct of_device_id apwmt_of_ids[] = {
	{.compatible = "mediatek,mt6797-consys",},
	{}
};
#endif

static struct platform_driver mtk_wmt_dev_drv = {
	.probe = mtk_wmt_probe,
	.remove = mtk_wmt_remove,
	.driver = {
		   .name = "mtk_wmt",
		   .owner = THIS_MODULE,
#ifdef CONFIG_OF
		   .of_match_table = apwmt_of_ids,
#endif
		   },
};

/* PMIC part */
#if CONSYS_PMIC_CTRL_ENABLE
#if !defined(CONFIG_MTK_PMIC_LEGACY)
struct regulator *reg_VCN18;
struct regulator *reg_VCN28;
struct regulator *reg_VCN33_BT;
struct regulator *reg_VCN33_WIFI;
#endif
#endif

/* GPIO part */
#if !defined(CONFIG_MTK_GPIO_LEGACY)
struct pinctrl *consys_pinctrl = NULL;
#endif

/*******************************************************************************
*                           P R I V A T E   D A T A
********************************************************************************
*/

/*******************************************************************************
*                              F U N C T I O N S
********************************************************************************
*/
#define DYNAMIC_DUMP_GROUP_NUM 5
#if CONSYS_ENALBE_SET_JTAG
UINT32 gJtagCtrl = 0;

#define JTAG_ADDR1_BASE 0x10005000
#define JTAG_ADDR2_BASE 0x10002000

char *jtag_addr1 = (char *)JTAG_ADDR1_BASE;
char *jtag_addr2 = (char *)JTAG_ADDR2_BASE;

#define JTAG1_REG_WRITE(addr, value)	\
writel(value, ((PUINT32)(jtag_addr1+(addr-JTAG_ADDR1_BASE))))
#define JTAG1_REG_READ(addr)			\
readl(((PUINT32)(jtag_addr1+(addr-JTAG_ADDR1_BASE))))

#define JTAG2_REG_WRITE(addr, value)	\
	writel(value, ((PUINT32)(jtag_addr2+(addr-JTAG_ADDR2_BASE))))
#define JTAG2_REG_READ(addr)			\
	readl(((PUINT32)(jtag_addr2+(addr-JTAG_ADDR2_BASE))))

static INT32 mtk_wcn_consys_jtag_set_for_mcu(VOID)
{
#if 1
	INT32 iRet = -1;
	UINT32 tmp = 0;

	WMT_PLAT_DBG_FUNC("WCN jtag_set_for_mcu start...\n");
	jtag_addr1 = ioremap(JTAG_ADDR1_BASE, 0x1000);
	if (jtag_addr1 == 0) {
		WMT_PLAT_ERR_FUNC("remap jtag_addr1 fail!\n");
		return iRet;
	}
	WMT_PLAT_DBG_FUNC("jtag_addr1 = 0x%p\n", jtag_addr1);
	jtag_addr2 = ioremap(JTAG_ADDR2_BASE, 0x1000);
	if (jtag_addr2 == 0) {
		WMT_PLAT_ERR_FUNC("remap jtag_addr2 fail!\n");
		return iRet;
	}
	WMT_PLAT_DBG_FUNC("jtag_addr2 = 0x%p\n", jtag_addr2);

	tmp = JTAG1_REG_READ(0x10005330);
	tmp &= 0xff00ffff;
	tmp |= 0x00550000;
	JTAG1_REG_WRITE(0x10005330, tmp);
	WMT_PLAT_DBG_FUNC("JTAG set reg:%x, val:%x\n", 0x10005330, tmp);

	tmp = JTAG1_REG_READ(0x10005340);
	tmp &= 0xfff00000;
	tmp |= 0x00055555;
	JTAG1_REG_WRITE(0x10005340, tmp);
	WMT_PLAT_DBG_FUNC("JTAG set reg:%x, val:%x\n", 0x10005340, tmp);

	tmp = JTAG2_REG_READ(0x100020F0);
	tmp &= 0xfffffc0c;
	tmp |= 0x000003f3;
	JTAG2_REG_WRITE(0x100020F0, tmp);
	WMT_PLAT_DBG_FUNC("JTAG set reg:%x, val:%x\n", 0x100020F0, tmp);

	tmp = JTAG2_REG_READ(0x100020b0);
	tmp &= 0xfffffe0c;
	tmp |= 0x00000180;
	JTAG2_REG_WRITE(0x100020b0, tmp);
	WMT_PLAT_DBG_FUNC("JTAG set reg:%x, val:%x\n", 0x100020b0, tmp);

	tmp = JTAG2_REG_READ(0x100020d0);
	tmp &= 0xfffffe0c;
	JTAG2_REG_WRITE(0x100020d0, tmp);
	WMT_PLAT_DBG_FUNC("JTAG set reg:%x, val:%x\n", 0x100020d0, tmp);

#endif
	return 0;
}

UINT32 mtk_wcn_consys_jtag_flag_ctrl(UINT32 en)
{
	WMT_PLAT_INFO_FUNC("%s jtag set for MCU\n", en ? "enable" : "disable");
	gJtagCtrl = en;
	return 0;
}

#endif

static INT32 mtk_wmt_probe(struct platform_device *pdev)
{

	reg_VCN18 = devm_regulator_get_optional(&pdev->dev, "vcn18");
	if (IS_ERR(reg_VCN18))
		return dev_err_probe(&pdev->dev, PTR_ERR(reg_VCN18), "VCN18 unavailable\n");
	reg_VCN28 = devm_regulator_get_optional(&pdev->dev, "vcn28");
	if (IS_ERR(reg_VCN28))
		return dev_err_probe(&pdev->dev, PTR_ERR(reg_VCN28), "VCN28 unavailable\n");
	reg_VCN33_BT = devm_regulator_get_optional(&pdev->dev, "vcn33_bt");
	if (IS_ERR(reg_VCN33_BT))
		return dev_err_probe(&pdev->dev, PTR_ERR(reg_VCN33_BT), "VCN33 BT unavailable\n");
	reg_VCN33_WIFI = devm_regulator_get_optional(&pdev->dev, "vcn33_wifi");
	if (IS_ERR(reg_VCN33_WIFI))
		return dev_err_probe(&pdev->dev, PTR_ERR(reg_VCN33_WIFI), "VCN33 WLAN unavailable\n");
	if (!of_property_read_bool(pdev->dev.of_node, "mediatek,no-gps-lna")) {
		consys_pinctrl = devm_pinctrl_get(&pdev->dev);
		if (IS_ERR(consys_pinctrl))
			return PTR_ERR(consys_pinctrl);
	}
	return mt6797_consys_register_gates(&pdev->dev);
}

static void mtk_wmt_remove(struct platform_device *pdev)
{
}

/* Called with consys_sequence_lock held; only release references we acquired. */
static int consys_shutdown_locked(void)
{
	int ret;
	if (consys_domain_pending) {
		ret = mt6797_conn_power(false);
		if (ret)
			return ret;
		consys_domain_pending = false;
	}
	if (consys_vcn28_hw_control) {
		ret = pmic_set_register_value(MT6351_PMIC_RG_VCN28_ON_CTRL, 0);
		if (ret)
			return ret;
		consys_vcn28_hw_control = false;
	}
	if (consys_vcn28_held) {
		ret = regulator_disable(reg_VCN28);
		if (ret)
			return ret;
		consys_vcn28_held = false;
	}
	if (consys_vcn18_held) {
		ret = regulator_disable(reg_VCN18);
		if (ret)
			return ret;
		consys_vcn18_held = false;
	}
	consys_state = CONSYS_OFF;
	return 0;
}

INT32 mtk_wcn_consys_hw_reg_ctrl(UINT32 on, UINT32 co_clock_type)
{

	void __iomem *afe;
	u32 chip, value;
	int ret, cleanup;

	if (mt6797_consys_ready())
		return -ENODEV;
	/* The device's WMT_SOC.cfg selects the dedicated 26 MHz oscillator. */
	if (co_clock_type)
		return -EOPNOTSUPP;
	mutex_lock(&consys_sequence_lock);
	if (on && consys_state == CONSYS_ON) {
		ret = 0;
		goto out;
	}
	if (!on) {
		ret = consys_shutdown_locked();
		if (ret)
			consys_state = CONSYS_FAULT;
		goto out;
	}
	if (consys_state != CONSYS_OFF) {
		ret = -EIO;
		pr_err("MT6797 CONSYS: previous power cleanup failed; power off before retry\n");
		goto out;
	}
	consys_state = CONSYS_STARTING;
	ret = pmic_set_register_value(MT6351_PMIC_RG_VCN18_ON_CTRL, 0);
	if (ret)
		goto fail;
	ret = regulator_set_voltage(reg_VCN18, 1800000, 1800000);
	if (ret)
		goto fail;
	ret = regulator_enable(reg_VCN18);
	if (ret)
		goto fail;
	consys_vcn18_held = true;
	udelay(240);
	ret = pmic_set_register_value(MT6351_PMIC_RG_VCN28_ON_CTRL, 1);
	if (ret)
		goto fail;
	consys_vcn28_hw_control = true;
	ret = regulator_set_voltage(reg_VCN28, 2800000, 2800000);
	if (ret)
		goto fail;
	ret = regulator_enable(reg_VCN28);
	if (ret)
		goto fail;
	consys_vcn28_held = true;
	value = CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_CONN2AP_SLEEP_MASK_OFFSET);
	CONSYS_REG_WRITE(conn_reg.topckgen_base + CONSYS_CONN2AP_SLEEP_MASK_OFFSET,
			 value | CONSYS_CONN2AP_SLEEP_MASK_BIT);
	mtk_wdt_swsysret_config(BIT(12), 1);
	consys_domain_pending = true;
	ret = mt6797_conn_power(true);
	if (ret)
		goto fail;
	udelay(30);
	ret = readl_poll_timeout((void __iomem *)(conn_reg.mcu_base + CONSYS_CHIP_ID_OFFSET),
				chip, chip == 0x0279, 20000, 200000);
	if (ret) {
		pr_err("MT6797 CONSYS: MCU chip ID %08x, expected 00000279\n", chip);
		goto fail;
	}
	CONSYS_REG_WRITE(conn_reg.mcu_base + CONSYS_MCU_CFG_ACR_OFFSET,
			 CONSYS_REG_READ(conn_reg.mcu_base + CONSYS_MCU_CFG_ACR_OFFSET) |
			 CONSYS_MCU_CFG_ACR_MBIST_BIT);
	afe = ioremap(CONSYS_AFE_REG_BASE, 0x100);
	if (!afe) {
		ret = -ENOMEM;
		goto fail;
	}
	writel(CONSYS_AFE_REG_DIG_RCK_01_VALUE, afe + CONSYS_AFE_REG_WBG_RCK_01_OFFSET);
	writel(CONSYS_AFE_REG_WBG_RX_01_VALUE, afe + CONSYS_AFE_REG_WBG_GPS_01_OFFSET);
	writel(CONSYS_AFE_REG_WBG_RX_02_VALUE, afe + CONSYS_AFE_REG_WBG_GPS_02_OFFSET);
	writel(CONSYS_AFE_REG_WBG_RX_01_VALUE, afe + CONSYS_AFE_REG_WBG_BT_RX_01_OFFSET);
	writel(CONSYS_AFE_REG_WBG_RX_02_VALUE, afe + CONSYS_AFE_REG_WBG_BT_RX_02_OFFSET);
	writel(CONSYS_AFE_REG_WBG_TX_01_VALUE, afe + CONSYS_AFE_REG_WBG_BT_TX_01_OFFSET);
	writel(CONSYS_AFE_REG_WBG_TX_02_VALUE, afe + CONSYS_AFE_REG_WBG_BT_TX_02_OFFSET);
	writel(CONSYS_AFE_REG_WBG_TX_03_VALUE, afe + CONSYS_AFE_REG_WBG_BT_TX_03_OFFSET);
	writel(CONSYS_AFE_REG_WBG_RX_02_VALUE, afe + CONSYS_AFE_REG_WBG_WF_RX_02_OFFSET);
	writel(CONSYS_AFE_REG_WBG_TX_01_VALUE, afe + CONSYS_AFE_REG_WBG_WF_TX_01_OFFSET);
	writel(CONSYS_AFE_REG_WBG_TX_03_VALUE, afe + CONSYS_AFE_REG_WBG_WF_TX_03_OFFSET);
	mb();
	iounmap(afe);
	mtk_wdt_swsysret_config(BIT(12), 0);
	msleep(20);
	consys_state = CONSYS_ON;
	pr_info("MT6797_CONSYS_POWER_READY: chip=%04x EMI=%pa\n", chip, &gConEmiPhyBase);
	goto out;
fail:
	cleanup = consys_shutdown_locked();
	if (cleanup) {
		consys_state = CONSYS_FAULT;
		pr_err("MT6797 CONSYS: cleanup failed (%d); retaining supply references\n", cleanup);
	}
out:
	mutex_unlock(&consys_sequence_lock);
	return ret;
}

/* The MCU window must not be read after the power domain is isolated. */
int mt6797_consys_read_cpupcr(u32 *value)
{
	int ret = 0;
	mutex_lock(&consys_sequence_lock);
	if (mt6797_consys_ready() || consys_state != CONSYS_ON)
		ret = -EHOSTDOWN;
	else
		*value = CONSYS_REG_READ(conn_reg.mcu_base + CONSYS_CPUPCR_OFFSET);
	mutex_unlock(&consys_sequence_lock);
	return ret;
}

INT32 mtk_wcn_consys_hw_gpio_ctrl(UINT32 on)
{
	INT32 iRet = 0;

	WMT_PLAT_DBG_FUNC("CONSYS-HW-GPIO-CTRL(0x%08x), start\n", on);

	if (on) {

		/*if external modem used,GPS_SYNC still needed to control */
		iRet += wmt_plat_gpio_ctrl(PIN_GPS_SYNC, PIN_STA_INIT);
		iRet += wmt_plat_gpio_ctrl(PIN_GPS_LNA, PIN_STA_INIT);

		iRet += wmt_plat_gpio_ctrl(PIN_I2S_GRP, PIN_STA_INIT);

		/* TODO: [FixMe][GeorgeKuo] double check if BGF_INT is implemented ok */
		/* iRet += wmt_plat_gpio_ctrl(PIN_BGF_EINT, PIN_STA_MUX); */
		iRet += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_INIT);
		iRet += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_EINT_DIS);
		WMT_PLAT_DBG_FUNC("CONSYS-HW, BGF IRQ registered and disabled\n");

	} else {

		/* set bgf eint/all eint to deinit state, namely input low state */
		iRet += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_EINT_DIS);
		iRet += wmt_plat_eirq_ctrl(PIN_BGF_EINT, PIN_STA_DEINIT);
		WMT_PLAT_DBG_FUNC("CONSYS-HW, BGF IRQ unregistered and disabled\n");
		/* iRet += wmt_plat_gpio_ctrl(PIN_BGF_EINT, PIN_STA_DEINIT); */

		/*if external modem used,GPS_SYNC still needed to control */
		iRet += wmt_plat_gpio_ctrl(PIN_GPS_SYNC, PIN_STA_DEINIT);
		iRet += wmt_plat_gpio_ctrl(PIN_I2S_GRP, PIN_STA_DEINIT);
		/* deinit gps_lna */
		iRet += wmt_plat_gpio_ctrl(PIN_GPS_LNA, PIN_STA_DEINIT);

	}
	WMT_PLAT_DBG_FUNC("CONSYS-HW-GPIO-CTRL(0x%08x), finish\n", on);
	return iRet;

}

INT32 mtk_wcn_consys_hw_pwr_on(UINT32 co_clock_type)
{

	int ret = mtk_wcn_consys_hw_reg_ctrl(1, co_clock_type);
	if (ret)
		return ret;
	ret = mtk_wcn_consys_hw_gpio_ctrl(1);
	if (ret) {
		mtk_wcn_consys_hw_gpio_ctrl(0);
		mtk_wcn_consys_hw_reg_ctrl(0, co_clock_type);
	}
	return ret;
}

INT32 mtk_wcn_consys_hw_pwr_off(UINT32 co_clock_type)
{

	int ret = mtk_wcn_consys_hw_gpio_ctrl(0);
	int power_ret = mtk_wcn_consys_hw_reg_ctrl(0, co_clock_type);
	return ret ? ret : power_ret;
}

INT32 mtk_wcn_consys_hw_rst(UINT32 co_clock_type)
{

	int ret = mtk_wcn_consys_hw_reg_ctrl(0, co_clock_type);
	return ret ? ret : mtk_wcn_consys_hw_reg_ctrl(1, co_clock_type);
}

#if CONSYS_BT_WIFI_SHARE_V33
INT32 mtk_wcn_consys_hw_bt_paldo_ctrl(UINT32 enable)
{
	/* spin_lock_irqsave(&gBtWifiV33.lock,gBtWifiV33.flags); */
	if (enable) {
		if (1 == gBtWifiV33.counter) {
			gBtWifiV33.counter++;
			WMT_PLAT_DBG_FUNC("V33 has been enabled,counter(%d)\n", gBtWifiV33.counter);
		} else if (2 == gBtWifiV33.counter) {
			WMT_PLAT_DBG_FUNC("V33 has been enabled,counter(%d)\n", gBtWifiV33.counter);
		} else {
#if CONSYS_PMIC_CTRL_ENABLE
			/*do BT PMIC on,depenency PMIC API ready */
			/*switch BT PALDO control from SW mode to HW mode:0x416[5]-->0x1 */
			/* VOL_DEFAULT, VOL_3300, VOL_3400, VOL_3500, VOL_3600 */
			hwPowerOn(MT6351_POWER_LDO_VCN33_BT, VOL_3300, "wcn_drv");
			mt6351_upmu_set_rg_vcn33_on_ctrl(1);
#endif
			WMT_PLAT_INFO_FUNC("WMT do BT/WIFI v3.3 on\n");
			gBtWifiV33.counter++;
		}

	} else {
		if (1 == gBtWifiV33.counter) {
			/*do BT PMIC off */
			/*switch BT PALDO control from HW mode to SW mode:0x416[5]-->0x0 */
#if CONSYS_PMIC_CTRL_ENABLE
			mt6351_upmu_set_rg_vcn33_on_ctrl(0);
			hwPowerDown(MT6351_POWER_LDO_VCN33_BT, "wcn_drv");
#endif
			WMT_PLAT_INFO_FUNC("WMT do BT/WIFI v3.3 off\n");
			gBtWifiV33.counter--;
		} else if (2 == gBtWifiV33.counter) {
			gBtWifiV33.counter--;
			WMT_PLAT_DBG_FUNC("V33 no need disabled,counter(%d)\n", gBtWifiV33.counter);
		} else {
			WMT_PLAT_DBG_FUNC("V33 has been disabled,counter(%d)\n", gBtWifiV33.counter);
		}

	}
	/* spin_unlock_irqrestore(&gBtWifiV33.lock,gBtWifiV33.flags); */
	return 0;
}

INT32 mtk_wcn_consys_hw_wifi_paldo_ctrl(UINT32 enable)
{
	mtk_wcn_consys_hw_bt_paldo_ctrl(enable);
	return 0;
}

#else
INT32 mtk_wcn_consys_hw_bt_paldo_ctrl(UINT32 enable)
{

	int ret;
	if (mt6797_consys_ready())
		return -ENODEV;
	if (!enable)
		return regulator_disable(reg_VCN33_BT);
	ret = regulator_set_voltage(reg_VCN33_BT, 3300000, 3300000);
	return ret ? ret : regulator_enable(reg_VCN33_BT);
}

INT32 mtk_wcn_consys_hw_wifi_paldo_ctrl(UINT32 enable)
{

	int ret;
	if (mt6797_consys_ready())
		return -ENODEV;
	if (!enable)
		return regulator_disable(reg_VCN33_WIFI);
	ret = regulator_set_voltage(reg_VCN33_WIFI, 3300000, 3300000);
	return ret ? ret : regulator_enable(reg_VCN33_WIFI);
}

#endif
INT32 mtk_wcn_consys_hw_vcn28_ctrl(UINT32 enable)
{

	int ret;
	if (mt6797_consys_ready())
		return -ENODEV;
	if (!enable)
		return regulator_disable(reg_VCN28);
	ret = regulator_set_voltage(reg_VCN28, 2800000, 2800000);
	return ret ? ret : regulator_enable(reg_VCN28);
}

INT32 mtk_wcn_consys_hw_state_show(VOID)
{
	return 0;
}

INT32 mtk_wcn_consys_hw_restore(struct device *device)
{
	UINT32 addrPhy = 0;

	if (gConEmiPhyBase) {

#if CONSYS_EMI_MPU_SETTING
		/*set MPU for EMI share Memory */
		WMT_PLAT_INFO_FUNC("setting MPU for EMI share memory\n");

#if defined(CONFIG_ARCH_MT6797)
		emi_mpu_set_region_protection(gConEmiPhyBase + SZ_1M / 2,
							gConEmiPhyBase + SZ_1M - 1,
							19,
							SET_ACCESS_PERMISSON(FORBIDDEN, FORBIDDEN, FORBIDDEN, FORBIDDEN,
							FORBIDDEN, NO_PROTECTION, FORBIDDEN, NO_PROTECTION));
#else
		WMT_PLAT_WARN_FUNC("not define platform config\n");
#endif

#endif
		/*consys to ap emi remapping register:10000320, cal remapping address */
		addrPhy = (gConEmiPhyBase & 0xFFF00000) >> 20;

		/*enable consys to ap emi remapping bit12 */
		addrPhy = addrPhy | 0x1000;

		CONSYS_REG_WRITE(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET,
				 CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET) | addrPhy);

		WMT_PLAT_INFO_FUNC("CONSYS_EMI_MAPPING dump in restore cb(0x%08x)\n",
				   CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET));

#if 1
		pEmibaseaddr = ioremap(gConEmiPhyBase + SZ_1M / 2, CONSYS_EMI_MEM_SIZE);
#else
		pEmibaseaddr = ioremap(CONSYS_EMI_AP_PHY_BASE, CONSYS_EMI_MEM_SIZE);
#endif
		if (pEmibaseaddr) {
			WMT_PLAT_INFO_FUNC("EMI mapping OK(0x%p)\n", pEmibaseaddr);
			memset_io(pEmibaseaddr, 0, CONSYS_EMI_MEM_SIZE);
		} else {
			WMT_PLAT_ERR_FUNC("EMI mapping fail\n");
		}
	} else {
		WMT_PLAT_ERR_FUNC("consys emi memory address gConEmiPhyBase invalid\n");
	}

	return 0;
}

/*Reserved memory by device tree!*/
int reserve_memory_consys_fn(struct reserved_mem *rmem)
{
	WMT_PLAT_WARN_FUNC(" name: %s, base: 0x%llx, size: 0x%llx\n", rmem->name,
			   (unsigned long long)rmem->base, (unsigned long long)rmem->size);
	gConEmiPhyBase = rmem->base;
	return 0;
}
RESERVEDMEM_OF_DECLARE(reserve_memory_test, "mediatek,consys-reserve-memory", reserve_memory_consys_fn);


INT32 mtk_wcn_consys_hw_init(void)
{

	struct device_node *node, *mem;
	struct resource resource;
	u32 value;
	int i, ret;
	SIZE_T *bases[] = { &conn_reg.mcu_base, &conn_reg.ap_rgu_base,
			   &conn_reg.topckgen_base, &conn_reg.spm_base };

	mutex_lock(&consys_init_lock);
	if (consys_init_users) {
		consys_init_users++;
		ret = 0;
		goto unlock;
	}
	node = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-consys");
	if (!node || !of_device_is_available(node)) {
		ret = -ENODEV;
		goto putnode;
	}
	mem = of_parse_phandle(node, "memory-region", 0);
	if (!mem) {
		ret = -EINVAL;
		goto putnode;
	}
	ret = of_address_to_resource(mem, 0, &resource);
	of_node_put(mem);
	if (ret)
		goto putnode;
	if (resource_size(&resource) < SZ_1M || (resource.start & (SZ_1M - 1)) ||
	    resource.end > U32_MAX) {
		ret = -EINVAL;
		goto putnode;
	}
	gConEmiPhyBase = resource.start;
	for (i = 0; i < ARRAY_SIZE(bases); i++) {
		*bases[i] = (SIZE_T)of_iomap(node, i);
		if (!*bases[i]) {
			ret = -ENOMEM;
			goto unmap;
		}
	}
	ret = emi_mpu_set_region_protection(gConEmiPhyBase + SZ_1M / 2,
			gConEmiPhyBase + SZ_1M - 1, 19,
			SET_ACCESS_PERMISSON(FORBIDDEN, FORBIDDEN, FORBIDDEN, FORBIDDEN,
					      FORBIDDEN, NO_PROTECTION, FORBIDDEN, NO_PROTECTION));
	if (ret) {
		pr_err("MT6797 CONSYS: EMI MPU setup failed: %d\n", ret);
		goto unmap;
	}
	value = CONSYS_REG_READ(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET);
	value = (value & ~0x1fff) | ((gConEmiPhyBase >> 20) & 0xfff) | BIT(12);
	CONSYS_REG_WRITE(conn_reg.topckgen_base + CONSYS_EMI_MAPPING_OFFSET, value);
	pEmibaseaddr = ioremap(gConEmiPhyBase + SZ_1M / 2, CONSYS_EMI_MEM_SIZE);
	if (!pEmibaseaddr) {
		ret = -ENOMEM;
		goto unmap;
	}
	memset_io(pEmibaseaddr, 0, CONSYS_EMI_MEM_SIZE);
	ret = platform_driver_register(&mtk_wmt_dev_drv);
	if (ret)
		goto unmap;
	ret = mt6797_consys_ready();
	if (ret) {
		platform_driver_unregister(&mtk_wmt_dev_drv);
		goto unmap;
	}
	consys_init_users = 1;
	mtk_wcn_wmt_set_chipid(PLATFORM_SOC_CHIP);
	pr_info("MT6797_CONSYS_RESOURCES_READY: EMI=%pa size=%llu\n",
		&gConEmiPhyBase, (unsigned long long)resource_size(&resource));
	goto putnode;
unmap:
	if (pEmibaseaddr) {
		iounmap(pEmibaseaddr);
		pEmibaseaddr = NULL;
	}
	for (i = 0; i < ARRAY_SIZE(bases); i++) {
		if (*bases[i])
			iounmap((void __iomem *)*bases[i]);
		*bases[i] = 0;
	}
putnode:
	of_node_put(node);
unlock:
	mutex_unlock(&consys_init_lock);
	return ret;
}

INT32 mtk_wcn_consys_hw_deinit(void)
{

	mutex_lock(&consys_init_lock);
	if (!consys_init_users || --consys_init_users)
		goto out;
	if (consys_state != CONSYS_OFF)
		mtk_wcn_consys_hw_pwr_off(0);
	platform_driver_unregister(&mtk_wmt_dev_drv);
	if (pEmibaseaddr) {
		iounmap(pEmibaseaddr);
		pEmibaseaddr = NULL;
	}
	iounmap((void __iomem *)conn_reg.mcu_base);
	iounmap((void __iomem *)conn_reg.ap_rgu_base);
	iounmap((void __iomem *)conn_reg.topckgen_base);
	iounmap((void __iomem *)conn_reg.spm_base);
	memset(&conn_reg, 0, sizeof(conn_reg));
out:
	mutex_unlock(&consys_init_lock);
	return 0;
}

UINT8 *mtk_wcn_consys_emi_virt_addr_get(UINT32 ctrl_state_offset)
{
	UINT8 *p_virtual_addr = NULL;

	if (!pEmibaseaddr) {
		WMT_PLAT_ERR_FUNC("EMI base address is NULL\n");
		return NULL;
	}
	WMT_PLAT_DBG_FUNC("ctrl_state_offset(%08x)\n", ctrl_state_offset);
	p_virtual_addr = pEmibaseaddr + ctrl_state_offset;

	return p_virtual_addr;
}

UINT32 mtk_wcn_consys_soc_chipid(void)
{
	return PLATFORM_SOC_CHIP;
}

#if !defined(CONFIG_MTK_GPIO_LEGACY)
struct pinctrl *mtk_wcn_consys_get_pinctrl()
{
	return consys_pinctrl;
}
#endif
INT32 mtk_wcn_consys_set_dbg_mode(UINT32 flag)
{
	INT32 ret = -1;
	PUINT8 vir_addr = NULL;

	vir_addr = mtk_wcn_consys_emi_virt_addr_get(EXP_APMEM_CTRL_CHIP_FW_DBGLOG_MODE);
	if (!vir_addr) {
		WMT_PLAT_ERR_FUNC("get vir address fail\n");
		return -2;
	}
	if (flag) {
		ret = 0;
		CONSYS_REG_WRITE(vir_addr, 0x1);
	} else {
		CONSYS_REG_WRITE(vir_addr, 0x0);
	}
	WMT_PLAT_ERR_FUNC("fw dbg mode register value(0x%08x)\n", CONSYS_REG_READ(vir_addr));
	return ret;
}
INT32 mtk_wcn_consys_set_dynamic_dump(PUINT32 str_buf)
{
	PUINT8 vir_addr = NULL;

	vir_addr = mtk_wcn_consys_emi_virt_addr_get(EXP_APMEM_CTRL_CHIP_DYNAMIC_DUMP);
	if (!vir_addr) {
		WMT_PLAT_ERR_FUNC("get vir address fail\n");
		return -2;
	}
	memcpy(vir_addr, str_buf, DYNAMIC_DUMP_GROUP_NUM*8);
	WMT_PLAT_INFO_FUNC("dynamic dump register value(0x%08x)\n", CONSYS_REG_READ(vir_addr));
	return 0;
}

EXPORT_SYMBOL_GPL(mtk_wcn_consys_hw_wifi_paldo_ctrl);
