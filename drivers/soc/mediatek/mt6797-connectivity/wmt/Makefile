# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -I$(src)/linux/include -I$(src)/linux/pri/include
ccflags-y += -I$(src)/core/include -I$(src)/include
ccflags-y += -I$(src)/../btif/inc
ccflags-y += -DWMT_CREATE_NODE_DYNAMIC=1 -DWMT_PLAT_ALPS -DCONFIG_ARCH_MT6797=1 -DCONFIG_MTK_PLATFORM=mt6797
obj-$(CONFIG_MT6797_WMT) += mt6797_wmt.o
obj-$(CONFIG_MT6797_WMT_WIFI) += mt6797_wmt_wifi.o
mt6797_wmt_wifi-y := linux/pub/wmt_chrdev_wifi.o
mt6797_wmt-y := core/wmt_core.o core/wmt_ctrl.o core/wmt_func.o core/wmt_ic_soc.o \
 core/wmt_lib.o core/wmt_conf.o linux/pri/wmt_dev.o linux/pri/wmt_exp.o \
 linux/pri/stp_btif.o linux/pub/osal.o linux/pub/bgw_desense.o \
 linux/pri/stp_exp.o core/stp_core.o core/psm_core.o core/btm_core.o \
 linux/pri/stp_dbg.o
