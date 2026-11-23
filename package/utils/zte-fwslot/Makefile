# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=zte-fwslot
PKG_RELEASE:=3
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/zte-fwslot
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=ZTE SR1010 firmware slot flasher
  DEPENDS:=@TARGET_zte
endef

define Package/zte-fwslot/description
  Flash vendor-format upgrade packages to the inactive SR1010 firmware slot
  and select which slot the vendor bootloader starts, for OpenWrt/vendor
  dual boot on the device.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Package/zte-fwslot/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/zte-fwslot $(1)/usr/sbin/zte-fwslot
endef

$(eval $(call BuildPackage,zte-fwslot))
