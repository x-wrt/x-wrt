# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

BOARD:=zte
BOARDNAME:=ZTE SoCs
SUBTARGETS:=zx279133
FEATURES:=dt gpio ramdisk squashfs usb
KERNEL_PATCHVER:=7.3
LINUX_RELEASE:=3

include $(INCLUDE_DIR)/target.mk
ZX279133_NPU_FIRMWARE := $(TOPDIR)/package/firmware/zte/files/zx279133-npu.bin

ifneq ($(DUMP),1)
  ifneq ($(call qstrip,$(CONFIG_EXTERNAL_KERNEL_TREE))$(call qstrip,$(CONFIG_KERNEL_GIT_CLONE_URI)),)
    $(error The ZTE target requires its kernel patch series. Clear CONFIG_EXTERNAL_KERNEL_TREE and CONFIG_KERNEL_GIT_CLONE_URI)
  endif
endif

KERNEL_FILE_DEPENDS += $(ZX279133_NPU_FIRMWARE)

define Kernel/Prepare
	$(call Kernel/Prepare/Default)
	$(INSTALL_DIR) $(LINUX_DIR)/firmware/zte/zx279133
	$(CP) $(ZX279133_NPU_FIRMWARE) \
		$(LINUX_DIR)/firmware/zte/zx279133/mcode_intel.bin
endef

define Target/Description
	Build images for the ZTE ZX279133 SoC family.
endef

$(eval $(call BuildTarget))
