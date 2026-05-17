#
# Copyright (C) 2014-2023 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=umdns
PKG_RELEASE:=1
PKG_VERSION:=20260628

PKG_SOURCE_URL:=https://codeload.github.com/ptpt52/mdnsd/tar.gz/$(PKG_VERSION)?
PKG_HASH:=a5f1840c8b48cae03a3a1748f87ffe56f55bb6db4d013b3fd14a53c91c14dec3
PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz

PKG_BUILD_DIR:=$(BUILD_DIR)/mdnsd-$(PKG_VERSION)

PKG_MAINTAINER:=Minqiang Chen <ptpt52@gmail.com>
PKG_LICENSE:=LGPL-2.1

include $(INCLUDE_DIR)/package-seccomp.mk
include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/umdns
  SECTION:=net
  CATEGORY:=Network
  TITLE:=OpenWrt Multicast DNS Daemon
  DEPENDS:=+libubox +libubus +libblobmsg-json +libudebug
endef

TARGET_CFLAGS += -I$(STAGING_DIR)/usr/include

define Package/umdns/conffiles
/etc/config/umdns
endef

define Package/umdns/install
	$(INSTALL_DIR) \
		$(1)/usr/sbin \
		$(1)/etc/init.d \
		$(1)/etc/config \
		$(1)/usr/share/ucode/cli/modules
	$(INSTALL_DATA) ./files/mdns.uc $(1)/usr/share/ucode/cli/modules
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/umdns $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/umdns.init $(1)/etc/init.d/umdns
	$(INSTALL_CONF) ./files/umdns.config $(1)/etc/config/umdns
	$(call InstallSeccomp,$(1),./files/umdns.json)
endef

$(eval $(call BuildPackage,umdns))
