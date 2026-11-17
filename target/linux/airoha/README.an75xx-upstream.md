# AN75xx 硬件转发资料与上游对照

核对日期：2026-11-17。范围包括 AN7552/AN7563、AN7581、AN7583、AN7589，
并用 EN7523 旧版接口及 EcoNet 交换机补丁作对照。

## AN7563PT V1.4 资料的用途

已识别 `/tmp/AN7563PT Datasheet V1.4.pdf`，共 29 页，版本记录为
2023-12-15，SHA-256：
`0aaaddf4341a756f9ba1318ed52fa4de5314c9e00e5d32434d7e9e5d1385cedc`。

第 5–7 页确认了双 Cortex-A53、XPA、四个 GE PHY、两个 PCIe Gen2 x1
及最多两个用于 Wi-Fi offload 的 RISC-V 核。规格列出的最高 64K flows、
IPv4/IPv6、桥接与隧道能力可用于确定研发方向，但资料没有 FOE 布局、
NPU 邮箱、缓存一致性、寄存器位定义或初始化时序，不能据此写入寄存器。

当前 AN7563 实现使用 CPU 管理的 512 项 SRAM FOE，DRAM FOE 被明确关闭。
规格中的 64K 不能直接改成驱动流表大小；需要匹配的 DRAM 表格式、寻址、
老化与统计协议及实机验证。Wi-Fi packet buffers 与 DRAM FOE 是不同用途。
`CPU_TYPE:=cortex-a7` 是当前 ARM32 工具链配置，不能据此推断实际 CPU 型号；
现有 DTS 已使用 Cortex-A53/AArch32 描述。

## 可参考的进展

| 来源 | 已核对状态 | 对当前树的意义 |
| --- | --- | --- |
| [Linux master，以太网](https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/airoha/airoha_eth.c)、[NPU](https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/airoha/airoha_npu.c) | 本次 master 为 7.4-rc3；两者 OF 表匹配 EN7581、AN7583，没有 AN7563 | AN758x 是共用框架参考，不能视为 AN7563 已有主线无线支持 |
| [OpenWrt PR 23960](https://github.com/openwrt/openwrt/pull/23960) | AN7563/AN7552 与 Xiaomi BE5000 支持，仍为 open、未合并 | 板级、PCIe、以太网与 DSA 实现参考 |
| [NPU DMA RFC，2026-10-24](https://lkml.iu.edu/2610.3/00453.html) | 提议 coherent 独占池，streaming 不经过受限池 | 本次 `942-01-*` 采用这项策略，保留本地地址检查 |
| [PCS/phylink v21，2026-11-16](https://lore.kernel.org/r/20261116163412.551438-1-ansuelsmth@gmail.com) | 投稿系列，包含 provider 生命周期与能力更新 | 本次 `737-11-*` 回移核心修复，`942-02-*` 补依赖；保留本地 AN7563/AN7583/PON 扩展 |
| [GE PHY calibration v5，2026-11-05](https://lore.kernel.org/r/20261105105504.1024-1-ansuelsmth@gmail.com) | 独立 Airoha PHY 驱动、软件校准及 EEE 设置 | 本次 `942-03-*` 回移并修复加锁错误，校准默认关闭 |
| [AN7589 DTS v2，2026-11-12](https://lists.openwall.net/linux-kernel/2026/11/12/3330)、[pinctrl v3，2026-11-16](https://lore.kernel.org/r/20261116174854.342912-1-john@phrozen.org) | 基础平台与外设支持阶段 | 尚不能作为 AN7563 PPE/NPU 寄存器实现的依据 |
| [EcoNet EN751221 DSA v5，2026-11-16](https://lists.openwall.net/netdev/2026/11/16/704) | MMIO/MDIO 级联、passthrough、TRGMII 校准 | 可参考交换机探测顺序与锁处理，芯片寄存器及拓扑需分别核对 |
| [BE5000 社区源码发布，2026-11-09](https://github.com/pabragin/xiaomi-be5000-openwrt/releases/tag/be5000-openwrt-20261109) | 提供 118 个源码补丁，含 MIT 许可的 AN7552 双核固件 | 是后续 AN7563 无线硬转发最直接的代码参考；不是 Linux/OpenWrt 主线合入 |

NPU 统计不可用时保留 PPE 的 v4 提案和 NPU passive devfreq RFC 也值得跟踪。
前者影响 NPU 管理流表的路径，本树的 SRAM-only 策略已经绕开该初始化；
后者还需要匹配的时钟/OPP/设备树，当前没有接入。

## 本次回移的边界

NPU V2 的 coherent 描述符、mailbox、RRO/TXD 必须在
`0x80000000..0xbfffffff`。共享 DTS 改为单个 `dma` coherent 池，无无线
配置 1 MiB，无线配置 16 MiB。streaming packet mapping 仍由 Wi-Fi DMA
使用，解除 restricted-dma-pool/SWIOTLB 搬运要求。池耗尽时分配失败。

PCS 核心通过 RCU 保护回调中的 phylink 引用，处理 provider 晚到和移除，
重算支持能力并保留用户禁止广告的速率；适配 6.18 的 PHY/SFP API。
没有覆盖本地 PON 与 AN7563 的模拟寄存器配置。

AN7581/AN7583 改用 `CONFIG_AIROHA_GE_SOC_PHY=y`。v5 原始校准代码存在
package 锁递归、TX offset 退出时再次加锁、未持 MDIO bus 锁却调用
`__phy_package_*` 等问题。本次改为整个初始化持一次 package 锁，每次
package MDIO 访问独立持 bus 锁；校准周期错误时清除触发位，检查端口
范围和端口 0，并让 REXT 使用端口 0。

`CONFIG_AIROHA_GE_SOC_PHY_CALIBRATION` 默认关闭，AN7581 保留启动程序
校准，AN7583 保留原有清除 BMCR_PDOWN 的行为。该实验选项还使用 v5
假定的 AN7581 PDIDR=2、MDI 5R 参数及上游模拟时序；逐板确认这些值和
冷启动/重启/恢复结果后，才能考虑默认开启。AN7563 不启用这套校准。

## AN7552 源码固件与现有后端的差异

已重建发布包中 18 个固件变更，并对照配套 kernel/mt76 补丁；不能只取
最初的 `0026`、`0027`。后续还有双核流水线、buffer ownership、PN cache、
CCMP-256/GCMP 与客户端身份检查修正。

| 接口 | 当前 AN758x / 旧版 EN7523 实现 | AN7552 源码方案 |
| --- | --- | --- |
| 核心与加载 | V2 最多 8 核；EN7523 V1 最多 4 核；各自 rv32/data 固件 | 双核，单个 `an7552.bin`，32 字节镜像头，format/ABI=1，最大 64 KiB |
| 控制协议 | `airoha_npu_*` mailbox/TLV 与 `WLAN_FUNC_*` | 独立 `an7552_npu_*`，共享 SRAM 控制块、runtime ABI、park/station quiesce 握手 |
| 缓冲区 | AN758x DTS 与 coherent 池，mt76 经 `npu->dev` 分配 | `protocol.h` 固定 host/NPU 地址、TDMA/RRO/TXFREE/PN/工作区，必须整套预留和检查别名 |
| PPE | 本树 CPU-direct SRAM 表，natflow 提供逻辑槽与撤流 | 主机管理 AN7563 PPE，固件提交/回收接收 buffer；并非固件管理一张 64K DRAM 表 |
| 无线状态 | 本树限制单链路 AP，WCID/key 变化时同步撤流 | 固件维护授权/身份与每 TID PN，mt76 保存/恢复 radio TX PN，双核阶段间再次检查身份 |

源码显示核 0 处理有线到无线的 TDMA/TXFREE，同时执行无线上传前半段的
PN/解析；核 1 执行上传后半段、PPE/Linux 投递及 RX buffer 回收。广播、
不支持的帧、异常和需要协议栈处理的流量仍回退 Linux。社区对 MLD 地址
及 cipher 的扩展不等于本树已具备 MLO 或四地址加速能力。

现有 natflow 用实例归属、客户端授权、真实学习 FOE 索引、双向撤流和
故障停机保护安装流；只增加 `airoha,an7552-npu` compatible，或者让
`an7552_npu_forward_path()` 返回 true，都不足以满足这些条件。

后续具体接入顺序：

1. 先加入独立固件包、匹配的 `an7552_npu` 驱动与 reserved-memory，验证
   双核运行 ABI、地址范围、所有队列的启动/停止和 park 超时路径。
2. 将配套 mt76 变更适配到当前 `mt76-20261107`，接入 DMA ownership、
   RX/TX PN 同步、station quiesce 及重置；不能按旧版 mt76 行号套用。
3. 为 AN7552 接入 natflow 的 owner/setup/flush/rx 回调，按 512 项 SRAM
   的真实索引与四路桶做延迟绑定，核对学习包、保活副本和回收信息。
4. 把客户端删除/去授权、换密钥、radio reset、模块卸载接到双向撤流；
   park、撤流或 PN 同步超时后禁止新直转，恢复前不能复用旧 WCID/buffer。
5. 先实测单链路 AP ↔ 有线 IPv4/IPv6 TCP/UDP、VLAN/PPPoE 与 CPU 回退，
   再扩展双无线、STA 和 MLO；每次检查交付、校验和、顺序、泄漏及复位。

社区 cookie 复用和 DSA 独立 SMAC 补丁也已对照。当前树已有引用计数式
SMAC 分配及写入超时处理。cookie 方案依赖其原生 flowtable 生命周期，
不能直接搬到本树 natflow 成对流管理中；需要单独复现 firewall reload
时残留 cookie，并确认所属 flow block 后再决定替换规则。

## 验证与复现

本次完成：干净 Linux 6.18.60 上 810 个补丁回放，未使用 fuzz；四个新增
内核补丁均零偏移。带 995 和去除 995 的基础驱动分别完成 ARM32/ARM64
相关对象编译，包含 Airoha/MediaTek PHY、NPU、PPE、phylink 和 PCS。
实验校准代码的开启/关闭两种构建均已覆盖。

28 个 AN75xx 板级/overlay 树编译通过，25 个 AN758x 最终设备树验证了
coherent 池类型、大小、窗口、属性和 NPU phandle。源码抽取的主机测试
通过 ASan/UBSan，覆盖校准周期的超时、MDIO 错误、触发位清理和包锁
释放，以及 4096 个 PCS 广告屏蔽转换。它不模拟内核 RCU 调度或 PHY 模拟电路。

```sh
python3 target/linux/airoha/tests/upstream-backports.py /path/to/prepared/linux
python3 target/linux/airoha/tests/npu-dma-dts.py /path/to/prepared/linux /path/to/dtc
```

社区固件的 10 项主机测试和 10 项流水线 mutation 检查全部通过，覆盖
PN/重放、buffer ownership、TXFREE、TX pool、PPE index cache、双核顺序、
背压、换密钥/客户端及 RRO 停止重启。使用发布包可复现：

```sh
python3 target/linux/airoha/tests/an7552-source-check.py \
  /path/to/be5000-openwrt-source-patches.tar.gz /path/to/new-scratch-dir
```

脚本固定校验发布源码 SHA-256：
`e2c5e730f55aa0de62168457043912d3f1354ff3f71a7b55c18ea99d35b8a044`。
本机内核支持 MAP_FIXED_NOREPLACE，但 libc 头文件较旧；脚本仅在缺少宏
时补该 Linux UAPI 常量，没有改动被测固件逻辑。

未验证 RISC-V 固件镜像构建：本机 LLVM 6 没有匹配的 RISC-V/lld 工具链。
上述通过结果来自原始源码的主机测试，不是 NPU 实机测试。本轮没有在
AN7563 镜像中启用 AN7552 NPU，也没有宣称已完成 AN7563 无线硬转发。
