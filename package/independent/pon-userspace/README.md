# OpenWrt PON userspace

| Package / 软件包 | Version / 版本 | Purpose / 用途 |
| --- | --- | --- |
| `airoha-pond` | 0.3.0 | OMCI and OAM protocol agent / OMCI 与 OAM 协议代理 |
| `airoha-ponctl` | 0.2.0 | Line configuration and status / 线路配置与状态 |
| `airoha-pon-debug` | 0.1.0 | Capture and diagnostics / 抓包与诊断 |
| `luci-app-pon` | 0.1.0 | PON interface and board identity editor / PON 界面与板级身份编辑 |
| `luci-app-iptv` | 0.1.0 | IPTV bridge configuration / IPTV 桥接配置 |

Board identity layout: [IDENTITY.md](IDENTITY.md).

## FiberHome installation and factory data

HG5382A and HG5585F use the UBI layout described by
`target/linux/airoha/dts/an7581-fiberhome-ubi-parts.dtsi`. Follow the
[PonWrt installation procedure](https://github.com/pbs05/ponwrt#install):
[AN758x-Stock2UBI](https://github.com/pbs05/an758x-stock2ubi) prepares the flash
layout, and [AN758x U-Boot](https://github.com/pbs05/uboot-an758x) supplies the
boot chain and Web recovery.

Convert the device's original `factory` backup with
[FiberHome Factory](https://github.com/pbs05/fiberhome-factory) before restoring
it through U-Boot Web or LuCI's PON board data page. The converted image is
exactly 1 MiB and contains:

| Offset | Size | Data |
| --- | --- | --- |
| `0x0000` | `0x0800` | `APONCAL` calibration container converted from stock BDBOB |
| `0x1000` | `0x1000` | Wi-Fi EEPROM, when present |
| `0x2000` | `0x0006` | Binary base MAC |
| `0x2010` | `0x0008` | Binary PON serial number |
| `0x2020` | `0x0020` | Device serial number |

Raw stock JFFS2/UBIFS partition images and unconverted BDBOB files do not match
this layout. The conversion preserves calibration and identity, but does not
import the stock line mode or authentication settings. Configure those in
`/etc/config/pon` for the attached network; HG5382A variants support more than
one line mode, so the board name alone does not select EPON or XG-PON.

烽火原厂 `factory` 必须先通过上述工具转换为 1 MiB 统一镜像，再恢复到
`factory` 卷；原厂分区镜像或 BDBOB 文件不能直接代替转换结果。线路模式和
认证配置需另行设置，不能仅根据 HG5382A 型号固定选择 EPON 或 XG-PON。
