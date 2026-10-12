# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=airoha-pon-daemons
PKG_VERSION:=0.3.0
PKG_RELEASE:=4
PKG_URL:=https://github.com/pbs05/openwrt-pon-userspace/tree/main/airoha-pon-daemons

PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/rust-package.mk

CARGO_VENDOR_FILE:=$(PKG_NAME)-$(PKG_VERSION)-cargo-vendor.tar.zst

# Only the manifests enter the dependency archive; the sources stay in src/.
define Download/cargo-vendor
  FILE:=$(CARGO_VENDOR_FILE)
  URL:=file://$(CURDIR)/src
  PROTO:=airoha-pon-cargo-vendor
  HASH:=97b359a90bbda6857ff63a0a640f49c43e137b9a8e173edb5a4ea2f0a3f20107
endef

define DownloadMethod/airoha-pon-cargo-vendor
	( \
		set -e; \
		cargo_source=$$$$(mktemp); \
		trap 'rm -f "$$$$cargo_source"' EXIT; \
		$(TAR) -cf "$$$$cargo_source" src/Cargo.toml src/Cargo.lock; \
		RUST_CARGO_URL="$(RUST_CARGO_URL)" RUST_CARGO_HASH="$(RUST_CARGO_HASH)" \
			$(SCRIPT_DIR)/gen-cargo-vendor-tarball.sh archive "$(PKG_NAME)" "$(PKG_VERSION)" \
			--cached-source "$$$$cargo_source" --output "$(DL_DIR)/$(FILE)" \
			--hash "$(HASH)" --quiet; \
	)
endef

ifeq ($(PKG_SKIP_DOWNLOAD),)
$(eval $(call Download,cargo-vendor))
$(DL_DIR)/$(CARGO_VENDOR_FILE): $(CURDIR)/src/Cargo.toml $(CURDIR)/src/Cargo.lock
endif

define Package/airoha-pond
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=PON
  TITLE:=Airoha OMCI and OAM agent
  DEPENDS:=$(RUST_PKG_DEPENDS) +kmod-airoha-xpon +airoha-ponctl +ip-full
  EXTRA_DEPENDS:=airoha-ponctl (>=0.2.0-r2)
endef

define Package/airoha-pond/description
 OMCI and OAM protocol agent for Airoha PON interfaces.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(STAGING_DIR_HOST)/bin/zstdcat $(DL_DIR)/$(CARGO_VENDOR_FILE) | \
		$(TAR) -C $(PKG_BUILD_DIR) -xf - --strip-components=1
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
