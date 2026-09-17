# Tenda BE6L Pro

The `tenda_be6l-pro` profile uses AN7563 in AArch32 mode, with an
EN8811HN 2.5 GbE WAN PHY and three integrated Gigabit Ethernet LAN ports.
Wi-Fi uses the MT7991A/MT7992 2+3 antenna variant and the
`kmod-mt7992-23-firmware` package. Calibration is read from `Factory`,
offset 0, length 0x1e00; the vendor's default calibration binary is not
installed over the device's calibration data.

## Source references

Board details were checked against these files in `UGW6.0_HomeCoverage`:

- `targets/be6lpro/makefile.common`
- `targets/be6lpro/rcS`
- `targets/be6lpro/shared_software/diff/common/ReadMe.txt`
- `targets/be6lpro/shared_software/diff/common/4_1000_2/eth_to_port_config.txt`
- `targets/be6lpro/shared_software/diff/common/btnlt_2010/gpio_conf`
- `targets/be6lpro/image.mk` and `gen_image.sh`
- The `mtdparts` strings in `cfez_boot.bin`, `tuboot.bin` and `tcboot.bin`

The AN7563 platform reference is `UGW_main`:

- `comm_drv/ethernet/mtk/ae_wan/ae_wan_mac.c` and `ethtool_support.c`
- `bsp/kernel/an7563_v1.3.0_5.4.55/arch/arm/boot/dts/en7552.dtsi`
- `bsp/kernel/an7563_v1.3.0_5.4.55/arch/arm/mach-econet/sgmii/sgmii_cmd.c`
- `infra/lib/lib_drv/iof_drv/src/mediatek/iof_lib_drv.c`

The WAN uses GDM2 and the SoC's PON SerDes in Ethernet HSGMII mode;
`ae_wan.ko wan_sel=0` selects this path in the vendor firmware. This is
the internal connection to the copper EN8811HN PHY, not an optical port.
The PHY is at MDIO address 15 and uses GPIO13 for reset. LAN1/LAN2/LAN3
map to switch ports 2/3/4. GPIO6 releases Wi-Fi reset. The single button
is GPIO5; the status LED uses GPIO8 (green, active high) and GPIO9 (red,
active low). The OpenWrt button action is reset.

## Flash layout and images

The layout follows the product Bootloader binaries and `gen_image.sh`.
The platform `en7552_evb.dts` reference contains a different layout
(34 MiB firmware slots and CFG at 0x05100000); it must not replace the
product Bootloader's boundaries. All starts and sizes below are aligned
to the 128 KiB NAND eraseblock.

| Partition | Start | Size | Use |
| --- | --- | --- | --- |
| Bootloader | 0x00000000 | 0x00080000 | Read only |
| u-boot-env | 0x00080000 | 0x00080000 | Read only |
| Factory | 0x00100000 | 0x00400000 | Read only, Wi-Fi calibration |
| kernel | 0x00500000 | 0x00600000 | 6 MiB, God1 header and FIT kernel |
| ubi | 0x00b00000 | 0x05980000 | 89.5 MiB, rootfs and rootfs_data volumes |
| CFG | 0x06480000 | 0x00400000 | Read only, factory MAC/configuration |
| MISC2 | 0x06880000 | 0x00400000 | Read only |
| art | 0x06c80000 | 0x00380000 | Read only |

The kernel and UBI together replace KernelFS1, KernelFS2, CFM and
CFM_BACKUP. The BMT layer remains enabled. Space beyond `art`, including
the product signature area and BMT pool, is not allocated to UBI.

Packaging follows the Tenda BE12 Pro profile: `tenda-mkdualimageheader`
prepends the 16-byte `God1` header (flags, CRC32 and payload length) to
the FIT, then `sysupgrade-tar` adds the rootfs and image metadata. The
6 MiB limit includes the header. The FIT load/entry address is
0x80088000. `bootargs-override` removes the vendor's old root device and
partition command line so the new UBI rootfs can be selected.

An initramfs FIT is also generated for RAM boot and installation. The
sysupgrade image is an OpenWrt tar image, not the vendor's encrypted web
upgrade format. It uses the original kernel start and header format;
stock Bootloader slot selection/fallback and the first installation
still need validation on hardware. No Bootloader replacement is built
or flashed by this profile.

## Validation

Select **Airoha ARM / AN7563 / Tenda BE6L Pro** when configuring a build.
The existing BE5000 profile is not changed to select this device.

Validation covers the Bootloader partition boundaries, eraseblock alignment,
DTB compilation, God1 CRC/length, sysupgrade contents/metadata, and kernel
size rejection. An ARM32 kernel `Image` was linked with the merged generic
and AN7563 configuration using the available vendor GCC 10.2 toolchain.
Image rule tests use fixture payloads; a complete X-Wrt package/firmware
build and hardware tests have not been run.

Hardware validation must cover cold/warm boot, the RAM size reported by
the Bootloader (the DTS fallback is 512 MiB), all four physical ports,
2.5 GbE link negotiation, both Wi-Fi bands and calibration, LED/button
behavior, and upgrade/reboot with configuration persistence. AN7563 NPU
firmware/offload is not enabled by this board's device tree.
