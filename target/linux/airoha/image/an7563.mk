define Target/Description
	Build firmware images for Airoha AN7563 (ARMv8 Cortex-A53 running
	in AArch32 mode) based boards.
endef

# Standalone bl2.fip artifact. The U-Boot package installs it as
# $(STAGING_DIR_IMAGE)/an7563_<variant>-bl2.fip via Build/InstallDev.
define Build/an7563-preloader
  cat $(STAGING_DIR_IMAGE)/an7563_$1-bl2.fip >> $@
endef

# Bundled u-boot.fip artifact. For AN7563 the U-Boot package uses the
# legacy fip layout (FIP_LEGACY:=1), so the staged fip already contains
# BL2 + BL31 + U-Boot in a single FIP and is installed under the
# *-bl2-bl31-u-boot.fip name by Build/InstallDev.
define Build/an7563-bl2-bl31-uboot
  head -c $$((0x800)) /dev/zero > $@
  cat $(STAGING_DIR_IMAGE)/an7563_$1-bl2-bl31-u-boot.fip >> $@
  truncate -s $$((0x80000)) $@
endef

define Build/tenda-mkdualimageheader
	printf '%b' "\x47\x6f\x64\x31\x00\x00\x00\x00" >"$@.new"
	libdeflate-gzip -c "$@" | tail -c8 >>"$@.new"
	cat "$@" >>"$@.new"
	mv "$@.new" "$@"
endef

define Device/airoha_an7563-evb
  DEVICE_VENDOR := Airoha
  DEVICE_MODEL := AN7563 Evaluation Board
  DEVICE_DTS := an7563-evb
  KERNEL_LOADADDR := 0x80088000
  ARTIFACT/preloader.bin := an7563-preloader rfb
  ARTIFACT/bl2-bl31-uboot.bin := an7563-bl2-bl31-uboot rfb
  ARTIFACTS := preloader.bin bl2-bl31-uboot.bin
endef
TARGET_DEVICES += airoha_an7563-evb

define Device/tenda_be6l-pro
  DEVICE_VENDOR := Tenda
  DEVICE_MODEL := BE6L Pro
  DEVICE_DTS := an7563-tenda-be6l-pro
  DEVICE_PACKAGES := kmod-mt7992-23-firmware kmod-phy-airoha-en8811h \
	airoha-en8811h-firmware
  KERNEL_LOADADDR := 0x80088000
  KERNEL_SIZE := 6144k
  KERNEL_INITRAMFS := kernel-bin | lzma | \
	fit lzma $$(KDIR)/image-$$(firstword $$(DEVICE_DTS)).dtb with-initrd | pad-to 64k
  BLOCKSIZE := 128k
  PAGESIZE := 2048
  UBINIZE_OPTS := -E 5
  IMAGE/sysupgrade.bin := append-kernel | tenda-mkdualimageheader | \
	check-size $$$$(KERNEL_SIZE) | sysupgrade-tar kernel=$$$$@ | append-metadata
endef
TARGET_DEVICES += tenda_be6l-pro

define Device/xiaomi_be5000
  DEVICE_VENDOR := Xiaomi
  DEVICE_MODEL := BE5000
  DEVICE_DTS := an7563-xiaomi-be5000
  KERNEL_LOADADDR := 0x80088000
  ARTIFACT/preloader.bin := an7563-preloader xiaomi_be5000
  ARTIFACT/bl2-bl31-uboot.bin := an7563-bl2-bl31-uboot xiaomi_be5000
  ARTIFACTS := preloader.bin bl2-bl31-uboot.bin
endef
TARGET_DEVICES += xiaomi_be5000
