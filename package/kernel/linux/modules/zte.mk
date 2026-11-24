# SPDX-License-Identifier: GPL-2.0-only
# ZX279133 clocks, PHYs, XPCS and DSA core are provided by the ZTE target.
PKG_CONFIG_DEPENDS += CONFIG_ZTE_NPU_VLAN_OFFLOAD

define KernelPackage/zte-npu
  SUBMENU:=Network Devices
  TITLE:=ZTE ZX279133 NPPT Ethernet data plane
  DEPENDS:=@TARGET_zte
  KCONFIG:=CONFIG_ZTE_ZX279133_ETH \
	CONFIG_ZTE_ZX279133_LAN_CONDUIT
  FILES:=$(LINUX_DIR)/drivers/net/ethernet/zte/npu.ko \
	$(LINUX_DIR)/drivers/net/ethernet/zte/zx279133-lan-conduit.ko
  MODPARAMS.npu:=vlan_offload=$(if $(CONFIG_ZTE_NPU_VLAN_OFFLOAD),1,0)
  AUTOLOAD:=$(call AutoLoad,30,npu zx279133-lan-conduit,1)
endef

define KernelPackage/zte-npu/description
 Base NPPT MAC, DMA, PPU and hardware services. Normal software forwarding
 does not require the optional zte-fnoffload package. The existing ZTE target
 embeds the PPU microcode with CONFIG_EXTRA_FIRMWARE.
endef

define KernelPackage/zte-npu/config
  config ZTE_NPU_VLAN_OFFLOAD
    bool "ZTE single-tag IPv4 LAN VLAN hardware offload"
    depends on TARGET_zte_zx279133 && PACKAGE_kmod-zte-npu && PACKAGE_kmod-zte-fnoffload
    default y
    help
      Enable the NPU VID key layout and matching NPPT VLAN pop/push
      rules together at boot. Supports one 802.1Q tag on IPv4 LAN traffic
      routed to a physical WAN, with the tag restored in the reply path.
      Requires the calibrated SR1010 microcode. Unsupported paths use software forwarding.
endef

$(eval $(call KernelPackage,zte-npu))

define KernelPackage/zte-fnoffload
  SUBMENU:=Network Devices
  TITLE:=ZTE ZX279133 NPPT hardware NAT for natflow
  DEPENDS:=@TARGET_zte +kmod-zte-npu
  KCONFIG:=CONFIG_ZTE_ZX279133_FNOFFLOAD
  FILES:=$(LINUX_DIR)/drivers/net/ethernet/zte/fnoffload.ko
  MODPARAMS.fnoffload:=vlan_offload=$(if $(CONFIG_ZTE_NPU_VLAN_OFFLOAD),1,0)
  AUTOLOAD:=$(call AutoLoad,45,fnoffload)
endef

define KernelPackage/zte-fnoffload/description
 Install paired IPv4 NAT rules submitted by natflow.
endef

$(eval $(call KernelPackage,zte-fnoffload))

define KernelPackage/zte-rtl8372n
  SUBMENU:=Network Devices
  TITLE:=ZTE SR1010 RTL8372N DSA switch
  DEPENDS:=@TARGET_zte +kmod-zte-npu
  KCONFIG:=CONFIG_ZTE_ZX279133_RTL8372N
  FILES:=$(LINUX_DIR)/drivers/net/ethernet/zte/zx279133-rtl8372n.ko
  AUTOLOAD:=$(call AutoLoad,35,zx279133-rtl8372n,1)
endef

$(eval $(call KernelPackage,zte-rtl8372n))
