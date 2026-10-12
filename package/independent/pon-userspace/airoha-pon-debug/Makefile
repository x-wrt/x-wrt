# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=airoha-pon-debug
PKG_VERSION:=0.1.0
PKG_RELEASE:=2
PKG_URL:=https://github.com/pbs05/openwrt-pon-userspace/tree/main/airoha-pon-debug

PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/airoha-pon-debug
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=PON
  TITLE:=Airoha PON diagnostic collector
  DEPENDS:=+airoha-ponctl +airoha-pond +tcpdump +ip-full +ip-bridge +ethtool
endef

define Package/airoha-pon-debug/description
 Collects PON status, logs and packet captures.
endef

define Build/Compile
endef

define Package/airoha-pon-debug/install
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/airoha-pon-debug.config $(1)/etc/config/airoha-pon-debug
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/airoha-pon-debug.init $(1)/etc/init.d/airoha-pon-debug
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) ./files/airoha-pon-debug $(1)/usr/libexec/airoha-pon-debug
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/pon-debug $(1)/usr/sbin/pon-debug
endef

$(eval $(call BuildPackage,airoha-pon-debug))
