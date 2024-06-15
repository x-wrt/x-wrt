include $(TOPDIR)/rules.mk

PKG_NAME:=phytool
PKG_VERSION:=20240615

PKG_SOURCE_URL:=https://codeload.github.com/ptpt52/phytool/tar.gz/$(PKG_VERSION)?
PKG_HASH:=2deee43044c2b25560419a7eb4e25b3e8163b42d30c9f6ae33a3757c9172cec0
PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz

PKG_MAINTAINER:=Chen Minqiang <ptpt52@gmail.com>
PKG_LICENSE:=GPL-2.0
PKG_LICENSE_FILES:=COPYING

include $(INCLUDE_DIR)/package.mk

define Package/phytool
  SECTION:=net
  CATEGORY:=Network
  TITLE:=phytool Linux MDIO register access
  URL:=https://github.com/wkz/phytool.git
endef

define Package/phytool/description
   Linux MDIO register access
endef

define Build/Configure
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -Wall" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define Package/phytool/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/phytool $(1)/usr/bin/
endef

$(eval $(call BuildPackage,phytool))
