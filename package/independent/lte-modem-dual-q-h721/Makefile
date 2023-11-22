#
# Copyright (C) 2017-2019 Chen Minqiang <ptpt52@gmail.com>
# Copyright (C) 2019 X-WRT <dev@x-wrt.com>
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

BD_NAME=dual-q-h721
BD_CFGID=dual-q_h721

PKG_NAME:=lte-modem-$(BD_NAME)
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_MAINTAINER:=X-WRT <dev@x-wrt.com>
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define Package/lte-modem-$(BD_NAME)/Default
  SECTION:=Applications
  CATEGORY:=Independent
  TITLE:=lte-modem for $(BD_CFGID) board
  DEPENDS:=@(TARGET_ramips_mt7621_DEVICE_$(BD_CFGID)||TARGET_DEVICE_ramips_mt7621_DEVICE_$(BD_CFGID))
endef

define Package/lte-modem-$(BD_NAME)
  $(call Package/lte-modem-$(BD_NAME)/Default)
endef

define Package/lte-modem-$(BD_NAME)/description
  Standalone packages for $(BD_CFGID) board
endef

define Package/lte-modem-$(BD_NAME)/conffiles
endef

define Package/lte-modem-$(BD_NAME)/install
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/lte-modem.init $(1)/etc/init.d/lte-modem
endef

define Build/Configure
  true
endef

define Build/Prepare
  true
endef

define Build/Compile
  true
endef

$(eval $(call BuildPackage,lte-modem-$(BD_NAME)))
