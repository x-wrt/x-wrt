include $(TOPDIR)/rules.mk

PKG_NAME:=xs2184-2210
PKG_VERSION:=1.0.1
PKG_RELEASE:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

ifeq ($(CONFIG_BIG_ENDIAN),y)
	TARGET_CFLAGS+= -DHAVE_BIG_ENDIAN=1
endif
	TARGET_CFLAGS+= -D_GNU_SOURCE

define Package/xs2184-2210
	SECTION:=Applications
	CATEGORY:=Independent
	TITLE:=XinSheng PoE/PSE i2c driver
	URL:=http://www.tt-cool.com/xs2184
	DEPENDS:= +i2c-tools +libuci @TARGET_mediatek_filogic
endef

define Package/xs2184-2210/description
  This package helps to build l1profile on demand & install wifi l2 profile.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./files $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR)/files \
		$(TARGET_CONFIGURE_OPTS) \
		TARGET=Linux \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define Package/xs2184-2210/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/files/xs2184 $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/xs2184.init $(1)/etc/init.d/xs2184
	$(INSTALL_DIR) $(1)/etc/config/
	$(INSTALL_DATA) ./files/xs2184.config $(1)/etc/config/xs2184
endef

$(eval $(call BuildPackage,xs2184-2210))
