# AN7581 / AN7583 natflow 与 NPU 无线转发

此实现接通 natflow 与 mt76 已有的 Airoha NPU 队列接口。支持范围是
AN7581、AN7583 与 MT7992/MT7996 的单链路 AP 客户端。代码和设备树已做
编译、补丁回放及模拟回归验证，尚未完成实机 PCIe、固件握手和流量验证。

## 转发范围

以下为代码允许安装的路径；实际安装还取决于连接学习、客户端授权、
FOE 槽位及硬件初始化是否成功。连接首包和未命中的包仍通过 CPU 学习。

STA 与双无线流的回退是本轮实现限制，不代表硬件已被证实无法支持。
现有 mac80211/mt76 转发路径能解析 STA 的对端 AP 和 WDMA/BSS/WCID；
AN758x NPU 固件的 STA 处理仍需验证。后续可先扩展 STA ↔ 有线的三地址
路由/NAT，再补齐 AP ↔ STA 的双无线 PPE 入口与绑定。STA 的透明桥接
另需验证四地址/WDS 配合。原有 WED/NPU 的 STA forward-path 行为继续
保留；下表描述的是此次新增 natflow 后端的选择范围。

| 方向 | `hwnat_wed_disabled=0` | `hwnat_wed_disabled=1` |
| --- | --- | --- |
| 原生有线/DSA → AP 客户端 | PPE → CDM4 → NPU → MT7992/MT7996 | PPE → CPU/extdev → mt76 |
| AP 客户端 → 原生有线/DSA | mt76/HW-RRO → NPU → PPE → GDM/交换机 | mt76 → CPU/extdev → PPE → GDM/交换机 |
| PON ↔ AP 客户端 | 复用 GDM2/PON 映射，符合现有 PON 规则时允许上述双向路径 | 无线端使用 CPU/extdev |
| 有线 ↔ 有线，PON ↔ 有线 | 沿用现有 PPE/PON 实现 | 沿用现有 PPE/PON 实现 |
| AP ↔ AP（同射频或不同射频） | 本轮不安装 NPU 双无线流，保留软件转发 | 软件/extdev 转发 |
| AP ↔ STA 上联接口 | 软件/extdev 转发 | 软件/extdev 转发 |
| STA 上联接口 ↔ 有线/PON | 软件/extdev 转发 | 软件/extdev 转发 |
| STA、Mesh、MLO、4 地址客户端及不匹配的无线硬件 | 保留软件/extdev 转发 | 软件/extdev 转发 |

PON 上行仍须完成光链路注册、GEM/T-CONT/LLID 配置和转发映射。需要按
P-bit 逐包选择不同 GEM/T-CONT 的映射仍回退软件。本轮没有扩展 PON 注册
协议或取消这些限制。IPv4 TCP/UDP NAT、IPv6 TCP/UDP 路由及现有桥接学习
条件沿用 natflow；广播、多播和其他协议不在新增直转范围。

## 开关与撤流

运行时使用已有控制接口：

```sh
# 允许驱动按实际能力选择 NPU 直转
echo hwnat_wed_disabled=0 > /dev/natflow_ctl
# 强制无线使用 extdev，并同步撤销已安装的 NPU 无线流
echo hwnat_wed_disabled=1 > /dev/natflow_ctl
```

启动脚本仍从 `natflow.main.hwnat_wed` 读取设置，值为 `1` 对应上面的
`hwnat_wed_disabled=0`。`natflow.main.enabled` 与 `natflow.main.hwnat`
也须按现有规则启用。开关只决定转发方式；NPU 队列初始化和无线缓冲区
预留仍由设备配置决定。

设为 `0` 不保证每个连接直转：无线设备必须属于已登记的 NPU 实例，
客户端已授权，且不处于 MLO、4 地址或无线复位状态。能力检查失败时
沿用 extdev。需要软件加密的单播密钥会阻止对应客户端直转；不支持的
默认数据密钥会阻止整个 AP 接口直转，直到删除并重建该接口。管理帧的
软件密钥不影响普通数据转发。模式切换同步撤销 NPU 无线流，并更新软件
学习代数；模块重载和原生接口重新上线时同步后端模式，避免残留上次
模块的开关状态。

无线输入的 BIND 等待 NPU 上报学习包，使用实际 FOE 索引，校验 SRAM
边界与四路桶匹配，不从无线 netdev 猜测 PPE bank。保活副本用于更新
natflow 后由接收方释放，不再进入普通 RX 转发。

每个 QDMA 的 63 个 ACNT0 计数器仅用于统计。计数器用尽时，新流继续
安装且保留无线延迟绑定的学习索引，不为回收计数器撤销已有流。统计
读写超时后，仅停止该 QDMA 的计数器访问，直至设备重新探测；其他 QDMA
统计、PPE 转发和零流量增量的保活继续工作。撤流先确认 FOE 失效，再
释放软件计数器槽位，下一次分配时清零；统计故障不会触发 PPE 停机。

客户端去授权、释放 WCID、更新单播密钥、切换 4 地址及 NPU 停机/复位前，
先禁止该客户端的新直转，再撤销无线连接的两个方向。完整无线复位也
先停止 NPU 队列，在固件和客户端表重建后重新初始化 NPU。本轮采用粗粒度
撤流：一次客户端变化会撤销该 NPU 实例的所有 natflow 无线流，其他客户端
随后重新学习。如果硬件撤流超时，停止 PPE 加速并记录错误，须重新
探测设备或重启恢复，防止旧 WCID 被新客户端复用。

## 设备与内存

- AN7581 使用 multievb 基础设备树与 eMMC Kite/MT7992、Eagle/MT7996 overlay。
- AN7583 新增 `airoha_an7583-evb-emmc-kite`、
  `airoha_an7583-evb-emmc-eagle` 配置，使用 multievb 基础设备树与对应
  eMMC 无线 overlay，接入 Gen3 PCIe、对应无线固件包
  及 AN7583 NPU 固件。当前只描述一个 Gen3 主接口，未描述第二个 PCIe
  HIF2 接口；三频/双 HIF 的完整行为需要后续板级验证。
- PCIe 主控制器匹配采用[上游 AN7583 实现](https://lists.infradead.org/pipermail/linux-mediatek/2025-September/098848.html)。
  板上 PERST 接线、端点枚举、EEPROM/校准数据和 AN7583 固件的无线消息
  协议仍需在实际 EVB 上验证；新增 profile 属于接入配置。
- `an758x-npu-wlan.dtsi` 共享原有无线缓冲区预留，只有无线配置包含它。
  Nokia MD/MF 等无无线设备不预留这些无线缓冲区。
- NPU V2 的 DMA 地址限定在 `0x80000000` 至 `0xbfffffff`。所有 AN7581/
  AN7583 配置另预留 1 MiB streaming 池与 1 MiB coherent 池；无线配置
  分别扩大到 64 MiB 与 8 MiB。两池由内核在该窗口内动态选址，原有
  firmware、无线缓冲区和 QDMA 预留继续保留。coherent 池供 mailbox 和
  连续描述符使用，支持 MT7992 的单次 512 KiB 分配；streaming 池供
  mt76 收发包映射使用。池缺失、未正确挂接或地址越界时拒绝启动 NPU，
  池耗尽时分配失败，不向不受约束的内存回退。PPE 传给 NPU 的外部
  缓冲区也须位于该窗口，否则拒绝对应 NPU 操作。
- natflow 保持 8192 个逻辑槽、四路桶的 ABI。AN7581 的物理 PPE bank
  映射沿用现有双 PPE 实现，AN7583 使用单 PPE。AN7563、AN7581、AN7583
  均由基础驱动明确使用 SRAM-only、CPU-direct PPE，不启用 DRAM FOE
  表或 NPU PPE 表控制与统计初始化；有无 995 均采用这项策略。NPU 无线
  队列接口继续保留，其 DRAM 缓冲区与 DRAM FOE 表用途不同。
- SRAM-only 模式清除 DRAM 流表使能、哈希、大小和基址，不分配 DRAM
  流表，也不向硬件提供流表 DMA 地址。CPU 操作 SRAM 仍需普通内存中的
  软件镜像：AN7583 为 640 KiB，AN7581 双 PPE 合计 1.25 MiB，AN7563 为
  40 KiB；镜像通过 MMIO 同步到 SRAM，不供硬件 DMA 访问。
- 无线输出方向没有 QDMA ACNT0 计数器，本轮保活只维护存活，不提供该
  方向精确硬件字节/包计数。依赖这些计数的限速或计费场景需要关闭无线
  直转，待接入相应 NPU 统计后再验证。

## 补丁与验证边界

基础驱动与 natflow 控制面按以下边界组织：

- `941-04-*` 保留通用的 IPv6 源 MAC 引用、FOE 槽位保护及 AN7563 SRAM
  配置。
- `941-06-*` 统一确定 AN7563、AN7581、AN7583 的 SRAM-only、CPU-direct
  策略，清除 DRAM 流表配置，并将 SRAM 软件镜像改为普通内存。这项
  基础策略不依赖 995；其他使用 DRAM 流表的平台继续使用 DMA 流表。
- `994-01-*` 至 `994-04-*` 提供 PON 数据与控制接口、PPE 出口分类、原生
  TC 撤流和光模块 provider 检查，不依赖 natflow。
- `995-0001-hwnat-add-natflow-flow-offload-support.patch` 集中提供 natflow
  ABI、收发包接入、成对撤流、ACNT0 统计和 NPU 无线控制面，沿用基础
  驱动已经确定的 SRAM-only 策略。

仅移除 995 并重新构建时，基础以太网/PON、原生 TC 和 NPU 无线队列
接口仍然保留，三个平台仍使用 CPU-direct SRAM 流表。mt76 根据
`AIROHA_PPE_NATFLOW_WLAN` 能力宏选择接口，无此宏时沿用原生 PPE 学习
回调。natflow 运行时开关不改变基础驱动的流表策略。

mt76 生命周期及接收处理已整合到 `mt76-20261016` 源码包；无线开关透传
在独立 feeds/x 仓库的 `natflow/patches/100-*`。启用 natflow 无线接入时，
内核、mt76 和 natflow 三者须配套使用。

此前拆分分别验证有、无 995 两套补丁零 fuzz/零偏移回放，以及 ARM32/
ARM64 的以太网、PPE、NPU、PON PCS 和路径相关对象编译；mt76 的 NPU
对象也分别编译通过。

本次 SRAM-only 策略整理验证了有、无 995 两套相关路径的零 fuzz 回放，
新基础策略补丁与刷新后的 995 均可零偏移正反向回放，PPE 对象也分别
通过 ARM64 编译。168 个主机模拟场景检查七款 SoC 的寄存器、分配及
NPU PPE 接管路径；其他四个平台的原生寄存器写入序列保持不变。

独立 WED 和内置 xPON V1 源码依赖的公共框架尚未齐备，Kconfig 通过
`depends on BROKEN` 保持这两个选项不可选，待接入匹配的 MediaTek WED
私有 API、optical frontend 和 xPON 框架后再启用。这不影响 AN758x 的
模块化 PON 控制器。PON 数据路径接口及 PCS 对象编译通过，不等同于
整个 PON 控制器已通过构建或实机验证。

当前验证包括相关内核对象的 ARM64/ARM32 编译、mt76 NPU 与无 NPU 对象
编译、natflow 编译、相关路径完整补丁栈回放、七个设备树编译，以及带
ASan/UBSan 的绑定/撤流和并发模式切换模拟。它们不证明硬件直转成功。

实机验收需观察双向 FOE BIND、实际 PPE 索引、NPU 队列及 CPU 占用；分别
测试 IPv4/IPv6 TCP/UDP、桥接、PPPoE/VLAN、PON 映射、开关切换中的已有
连接、断线重连、重设密钥和无线复位，并核对流量没有重复转发或残留。
