# SPDX-License-Identifier: GPL-2.0-only
# MT6797 vendor gen3 MAC/firmware core with the integrated AHB transport.
WLAN_CHIP_ID := MT6797
MTK_PLATFORM := mt6797
MODULE_NAME := mt6797_wlan
obj-$(CONFIG_MT6797_WLAN) += $(MODULE_NAME).o
ccflags-y += -DLINUX -DMT6797
ccflags-y += -DCONFIG_ANDROID=0 -DCFG_SPM_WORKAROUND_FOR_HOTSPOT=0 -DDBG=0 -DBUILD_QA_DBG=0 -DWLAN_INCLUDE_PROC
ccflags-y += -DCFG_SUPPORT_DEBUG_FS=0 -DCFG_MET_PACKET_TRACE_SUPPORT=0
ccflags-y += -DCFG_SUPPORT_AGPS_ASSIST=1 -DCFG_SUPPORT_TSF_USING_BOOTTIME=1
ccflags-y += -DARP_MONITER_ENABLE=1 -DCFG_SUPPORT_CHNL_CONFLICT_REVISE=1
ccflags-y += -DCFG_SUPPORT_AEE=0 -DCFG_SUPPORT_PASSPOINT=0
ccflags-y += -I$(src)/os -I$(src)/os/linux/include
ccflags-y += -I$(src)/os/linux/hif/ahb_sdioLike/include
ccflags-y += -I$(src)/include -I$(src)/include/nic -I$(src)/include/mgmt

# ---------------------------------------------------
# Directory List
# ---------------------------------------------------
COMMON_DIR  := common/
OS_DIR      := os/linux/
NIC_DIR     := nic/
MGMT_DIR    := mgmt/

ifeq ($(WLAN_CHIP_ID), MT6797)
HIF_DIR     := os/linux/hif/ahb_sdioLike/
PLAT_DIR    := os/linux/plat/$(MTK_PLATFORM)/
else
HIF_DIR	    := os/linux/hif/sdio/
endif

# ---------------------------------------------------
# Objects List
# ---------------------------------------------------

COMMON_OBJS :=	$(COMMON_DIR)dump.o \
	$(COMMON_DIR)wlan_lib.o \
	$(COMMON_DIR)wlan_oid.o \
			$(COMMON_DIR)wlan_bow.o \
			$(COMMON_DIR)debug.o

NIC_OBJS := $(NIC_DIR)nic.o \
			$(NIC_DIR)nic_tx.o \
			$(NIC_DIR)nic_rx.o \
			$(NIC_DIR)nic_pwr_mgt.o \
            $(NIC_DIR)nic_rate.o \
			$(NIC_DIR)cmd_buf.o \
			$(NIC_DIR)que_mgt.o \
			$(NIC_DIR)nic_cmd_event.o

OS_OBJS :=	$(OS_DIR)gl_init.o \
			$(OS_DIR)gl_kal.o  \
			$(OS_DIR)gl_bow.o \
			$(OS_DIR)gl_wext.o \
			$(OS_DIR)gl_wext_priv.o \
			$(OS_DIR)gl_rst.o \
			$(OS_DIR)gl_cfg80211.o \
			$(OS_DIR)gl_vendor.o \
			$(OS_DIR)platform.o

MGMT_OBJS := $(MGMT_DIR)ais_fsm.o \
			 $(MGMT_DIR)aaa_fsm.o \
			 $(MGMT_DIR)assoc.o \
			 $(MGMT_DIR)auth.o \
			 $(MGMT_DIR)bss.o \
			 $(MGMT_DIR)cnm.o \
			 $(MGMT_DIR)cnm_timer.o \
			 $(MGMT_DIR)cnm_mem.o \
			 $(MGMT_DIR)hem_mbox.o \
			 $(MGMT_DIR)mib.o \
			 $(MGMT_DIR)privacy.o  \
			 $(MGMT_DIR)rate.o \
			 $(MGMT_DIR)rlm.o \
			 $(MGMT_DIR)rlm_domain.o \
			 $(MGMT_DIR)rlm_obss.o \
			 $(MGMT_DIR)rlm_protection.o \
			 $(MGMT_DIR)rsn.o \
			 $(MGMT_DIR)saa_fsm.o \
			 $(MGMT_DIR)scan.o \
			 $(MGMT_DIR)scan_fsm.o \
             $(MGMT_DIR)swcr.o \
             $(MGMT_DIR)roaming_fsm.o \
             $(MGMT_DIR)tkip_mic.o \
             $(MGMT_DIR)hs20.o \
             $(MGMT_DIR)tdls.o
# ---------------------------------------------------
# STATS Objects List
# ---------------------------------------------------
MGMT_OBJS += $(MGMT_DIR)stats.o

# ---------------------------------------------------
# P2P Objects List
# ---------------------------------------------------

COMMON_OBJS += $(COMMON_DIR)wlan_p2p.o

NIC_OBJS += $(NIC_DIR)p2p_nic.o

OS_OBJS += $(OS_DIR)gl_p2p.o \
           $(OS_DIR)gl_p2p_cfg80211.o \
           $(OS_DIR)gl_p2p_init.o \
           $(OS_DIR)gl_p2p_kal.o

MGMT_OBJS += $(MGMT_DIR)p2p_dev_fsm.o\
            $(MGMT_DIR)p2p_dev_state.o\
            $(MGMT_DIR)p2p_role_fsm.o\
            $(MGMT_DIR)p2p_role_state.o\
            $(MGMT_DIR)p2p_func.o\
            $(MGMT_DIR)p2p_scan.o\
            $(MGMT_DIR)p2p_ie.o\
            $(MGMT_DIR)p2p_rlm.o\
            $(MGMT_DIR)p2p_assoc.o\
            $(MGMT_DIR)p2p_bss.o\
            $(MGMT_DIR)p2p_rlm_obss.o

MGMT_OBJS += $(MGMT_DIR)wapi.o

OS_OBJS += $(OS_DIR)gl_proc.o


HIF_OBJS :=  $(HIF_DIR)arm.o
ifeq ($(WLAN_CHIP_ID), MT6797)
HIF_OBJS += $(HIF_DIR)ahb.o\
			$(HIF_DIR)ahb_pdma.o\
			$(HIF_DIR)sdio_bus_driver.o
PLAT_OBJS := $(PLAT_DIR)plat_priv.o

else
HIF_OBJS += $(HIF_DIR)sdio.o
endif


$(MODULE_NAME)-objs  += $(COMMON_OBJS)
$(MODULE_NAME)-objs  += $(NIC_OBJS)
$(MODULE_NAME)-objs  += $(OS_OBJS)
$(MODULE_NAME)-objs  += $(HIF_OBJS)
$(MODULE_NAME)-objs  += $(MGMT_OBJS)
