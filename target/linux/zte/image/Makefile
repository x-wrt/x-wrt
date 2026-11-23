# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

# Linux 7.3's dtc no longer implements this legacy graph check.
DTC_WARN_FLAGS := $(filter-out -Wno-graph_child_address,$(DTC_WARN_FLAGS))

KERNEL_LOADADDR := 0x80200000
KERNEL_ENTRY := 0x80200000

SR1010_OPENSSL := $(if $(wildcard $(STAGING_DIR_HOST)/bin/openssl),$(STAGING_DIR_HOST)/bin/openssl,openssl)

define Build/sr1010-firmware
	python3 $(PLATFORM_DIR)/mkfw/sr1010-mkfw.py \
		"$(IMAGE_KERNEL)" "$(IMAGE_ROOTFS)" "$@" \
		--openssl "$(SR1010_OPENSSL)" \
		-v "$(VERSION_DIST) $(VERSION_NUMBER)"
endef

define Device/Default
  PROFILES := Default
  KERNEL_NAME := Image
  KERNEL = kernel-bin | lzma | fit lzma $$(KDIR)/image-$$(firstword $$(DEVICE_DTS)).dtb
  KERNEL_INITRAMFS = kernel-bin | gzip | fit gzip $$(KDIR)/image-$$(firstword $$(DEVICE_DTS)).dtb
  KERNEL_SUFFIX := .itb
  KERNEL_INITRAMFS_SUFFIX := .itb
  IMAGES :=
endef

define Device/zte_zxslc-sr1010
  DEVICE_VENDOR := ZTE
  DEVICE_MODEL := ZXSLC SR1010
  DEVICE_DTS := zx279133-sr1010-openwrt
  DEVICE_DTS_DIR := $(TOPDIR)/target/linux/zte/dts
  SUPPORTED_DEVICES := zte,zx279133-sr1010
  DEVICE_PACKAGES := zte-fwslot kmod-gpio-button-hotplug kmod-zte-npu kmod-zte-rtl8372n \
	kmod-usb-storage kmod-usb-storage-uas \
	block-mount luci luci-app-zte-fwslot
  IMAGES := sysupgrade.bin
  IMAGE/sysupgrade.bin := sr1010-firmware
endef
TARGET_DEVICES += zte_zxslc-sr1010

$(eval $(call BuildImage))
