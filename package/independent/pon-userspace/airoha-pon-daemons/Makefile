# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=airoha-pon-daemons
PKG_VERSION:=0.3.0
PKG_RELEASE:=2
PKG_URL:=https://github.com/pbs05/openwrt-pon-userspace/tree/main/airoha-pon-daemons

PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_DEPENDS:=rust/host
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk
include $(TOPDIR)/feeds/packages/lang/rust/rust-package.mk

define Package/airoha-pond
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=PON
  TITLE:=Airoha OMCI and OAM agent
  DEPENDS:=$(RUST_ARCH_DEPENDS) +kmod-airoha-xpon +ip-full
endef

define Package/airoha-pond/description
 OMCI and OAM protocol agent for Airoha PON interfaces.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/Cargo.toml ./src/Cargo.lock ./src/LICENSE $(PKG_BUILD_DIR)/
	$(CP) ./src/src $(PKG_BUILD_DIR)/
endef

define Package/airoha-pond/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/bin/airoha-pond $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/usr/bin
	$(LN) /usr/sbin/airoha-pond $(1)/usr/bin/pondctl
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/airoha-pond.init $(1)/etc/init.d/airoha-pond
endef

$(eval $(call RustBinPackage,airoha-pond))
$(eval $(call BuildPackage,airoha-pond))
