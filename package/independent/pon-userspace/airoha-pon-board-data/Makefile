# SPDX-License-Identifier: GPL-2.0-only
# Board tools extracted from pon_userspace cce9d756742487b378749c606b30e13ee9b0f093.
include $(TOPDIR)/rules.mk

PKG_NAME:=airoha-pon-board-data
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_URL:=https://github.com/pbs05/openwrt-pon-userspace

include $(INCLUDE_DIR)/package.mk

define Package/airoha-pon-board-data
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=PON
  TITLE:=PON calibration and board identity tools
  DEPENDS:=@(TARGET_airoha_an7581||TARGET_airoha_an7583) +mtd +ubi-utils +nand-utils +jshn +jsonfilter
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -Wall -Wextra -Werror \
		-o $(PKG_BUILD_DIR)/pon-identity-image $(PKG_BUILD_DIR)/identity-image.c $(TARGET_LDFLAGS)
endef

define Package/airoha-pon-board-data/install
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/pon-identity-image $(1)/usr/libexec/
	$(INSTALL_BIN) ./files/airoha-pon-data ./files/pon-board-identity $(1)/usr/libexec/
endef

$(eval $(call BuildPackage,airoha-pon-board-data))
