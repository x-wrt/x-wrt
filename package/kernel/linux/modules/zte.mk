# SPDX-License-Identifier: GPL-2.0-only
# ZX279133 clocks, PHYs, XPCS and DSA core are provided by the ZTE target.
define KernelPackage/zte-npu
  SUBMENU:=Network Devices
  TITLE:=ZTE ZX279133 NPPT Ethernet data plane
  DEPENDS:=@TARGET_zte
  KCONFIG:=CONFIG_ZTE_ZX279133_ETH \
	CONFIG_ZTE_ZX279133_LAN_CONDUIT
  FILES:=$(LINUX_DIR)/drivers/net/ethernet/zte/npu.ko \
	$(LINUX_DIR)/drivers/net/ethernet/zte/zx279133-lan-conduit.ko
  AUTOLOAD:=$(call AutoLoad,30,npu zx279133-lan-conduit,1)
endef

define KernelPackage/zte-npu/description
 Base NPPT MAC, DMA, PPU and hardware services. Normal software forwarding
 uses these drivers. The ZTE target embeds the PPU microcode with CONFIG_EXTRA_FIRMWARE.
endef

$(eval $(call KernelPackage,zte-npu))

define KernelPackage/zte-rtl8372n
  SUBMENU:=Network Devices
  TITLE:=ZTE SR1010 RTL8372N DSA switch
  DEPENDS:=@TARGET_zte +kmod-zte-npu
  KCONFIG:=CONFIG_ZTE_ZX279133_RTL8372N
  FILES:=$(LINUX_DIR)/drivers/net/ethernet/zte/zx279133-rtl8372n.ko
  AUTOLOAD:=$(call AutoLoad,35,zx279133-rtl8372n,1)
endef

$(eval $(call KernelPackage,zte-rtl8372n))
