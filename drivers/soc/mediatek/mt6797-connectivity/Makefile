# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -I$(src)/wmt/linux/include -I$(src)/wmt/linux/pri/include
ccflags-y += -I$(src)/wmt/core/include -I$(src)/wmt/include
ccflags-y += -I$(src)/wmt/mt6797/include -I$(src)
ccflags-y += -DCONFIG_ARCH_MT6797=1
obj-$(CONFIG_MT6797_CONNSYS) += mt6797_consys.o
mt6797_consys-y := mt6797-platform.o mt6797-emi.o mt6797-callbacks.o \
	wmt/mt6797/mtk_wcn_consys_hw.o wmt/mt6797/wmt_plat_alps.o
obj-$(CONFIG_MT6797_BTIF) += btif/
obj-y += wmt/
