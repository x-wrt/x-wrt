# SPDX-License-Identifier: GPL-2.0-only
#

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=airoha-pon-frontends
PKG_RELEASE:=12
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/airoha-pon-frontend
  SUBMENU:=Network Devices
  TITLE:=Airoha PON frontend core
  DEPENDS:=@(TARGET_airoha_an7581||TARGET_airoha_an7583)
  FILES:=$(PKG_BUILD_DIR)/airoha-pon-frontend-core.ko
  AUTOLOAD:=$(call AutoProbe,airoha-pon-frontend-core)
endef

define KernelPackage/airoha-pon-frontend/description
  Common interface for Airoha PON optical frontends.
endef

define KernelPackage/airoha-en7572
  SUBMENU:=Network Devices
  TITLE:=Airoha EN7572 PON frontend
  DEPENDS:=@(TARGET_airoha_an7581||TARGET_airoha_an7583) +kmod-airoha-pon-frontend
  FILES:=$(PKG_BUILD_DIR)/airoha-en7572.ko
  AUTOLOAD:=$(call AutoProbe,airoha-en7572)
endef

define KernelPackage/airoha-en7572/description
  Driver and firmware for Airoha EN7572 optical frontends.
endef

define KernelPackage/airoha-en7572/install
	$(INSTALL_DIR) $(1)/lib/firmware/airoha/en7572
	$(INSTALL_DATA) $(CURDIR)/firmware/A60993.elf.pm \
		$(1)/lib/firmware/airoha/en7572/A60993.elf.pm
	$(INSTALL_DATA) $(CURDIR)/firmware/A60993.elf.dm \
		$(1)/lib/firmware/airoha/en7572/A60993.elf.dm
endef

define KernelPackage/airoha-paged-bosa
  SUBMENU:=Network Devices
  TITLE:=GN28L95 and UX3363 PON frontends
  DEPENDS:=@TARGET_airoha_an7581 +kmod-airoha-pon-frontend
  FILES:=$(PKG_BUILD_DIR)/airoha-paged-bosa.ko
  AUTOLOAD:=$(call AutoProbe,airoha-paged-bosa)
endef

define KernelPackage/airoha-paged-bosa/description
  Paged I2C frontend driver for GN28L95 and UX3363.
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,airoha-pon-frontend))
$(eval $(call KernelPackage,airoha-en7572))
$(eval $(call KernelPackage,airoha-paged-bosa))
