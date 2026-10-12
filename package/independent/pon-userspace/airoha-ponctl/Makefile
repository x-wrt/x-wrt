# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=airoha-ponctl
PKG_VERSION:=0.2.0
PKG_RELEASE:=1
PKG_URL:=https://github.com/pbs05/openwrt-pon-userspace/tree/main/airoha-ponctl

PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_DEPENDS:=rust/host
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk
include $(TOPDIR)/feeds/packages/lang/rust/rust-package.mk

define Package/airoha-ponctl
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=PON
  TITLE:=Airoha PON control utility
  DEPENDS:=$(RUST_ARCH_DEPENDS) +kmod-airoha-xpon
endef

define Package/airoha-ponctl/description
 Configures PON mode, identity, status and data paths.
endef

define Package/airoha-ponctl/conffiles
/etc/config/pon
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/Cargo.toml ./src/Cargo.lock $(PKG_BUILD_DIR)/
	$(CP) ./src/src $(PKG_BUILD_DIR)/
endef

define Package/airoha-ponctl/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/bin/ponctl $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/pon.config $(1)/etc/config/pon
	chmod 0600 $(1)/etc/config/pon
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/pon.init $(1)/etc/init.d/pon
endef

$(eval $(call RustBinPackage,airoha-ponctl))
$(eval $(call BuildPackage,airoha-ponctl))
