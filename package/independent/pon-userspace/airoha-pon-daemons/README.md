# airoha-pond

`airoha-pond` provides OMCI for GPON-family modes and OAM for EPON-family modes.

`airoha-pond` 为 GPON 系列提供 OMCI，为 EPON 系列提供 OAM。

## Configuration / 配置

Configuration is stored in `/etc/config/pon`.

配置文件为 `/etc/config/pon`。

```uci
config xpon 'line0'
	option device 'pon0'
	option mode 'xgpon'
	option serial_number 'ABCD12345678'
	option registration_id ''

config omci 'line0_omci'
	option line 'line0'
	option device 'omci0'
	option omcc_version '0xb0'
	option disable_enhanced_security '0'
	option alloc_id_timeout '30'
	option vendor_id 'ABCD'
	option equipment_id ''
	option hardware_version ''
	option software_version ''
	option operator_id 'CTC'
	option loid ''
	option loid_password ''

config oam 'line0_oam'
	option line 'line0'
	option device 'oam0'
	option operator 'ctc'
	option ctc_oui '111111'
	option ctc_versions '21 30'
	option vendor_id ''
	option model ''
	option equipment_id ''
	option hardware_version ''
	option software_version ''
	option firmware_version ''
	option chipset_id ''
	option loid ''
	option loid_password ''
	option ge_ports '1'
```

`mode` accepts `xgpon`, `xgspon`, `epon-10g-1g` and `epon-10g-10g`. An empty value keeps the current driver mode. `omcc_version` accepts `0xb0` and `0x86`.

`mode` 支持 `xgpon`、`xgspon`、`epon-10g-1g` 和 `epon-10g-10g`。空值使用驱动当前模式。`omcc_version` 支持 `0xb0` 和 `0x86`。

When `mode` is empty, the agent uses `ponctl --device DEVICE mode effective`:
the active line mode takes precedence over a pending configuration; before the
line starts, the configured kernel mode is used. This also applies when running
the agent directly. The agent brings up the matching OMCI or OAM control
interface. An unavailable or unknown mode is an error and is retried by procd.

`mode` 为空时，代理通过 `ponctl` 读取内核有效模式：线路运行期间使用活动模式，
线路启动前使用内核配置模式，并启动对应的 OMCI 或 OAM 控制接口。直接运行代理
也遵循相同行为；模式查询失败时报告错误，由 procd 重试。

`alloc_id_timeout` is the number of seconds an OMCI data path may wait for PLOAM to assign its Alloc-ID before the data path state becomes `alloc-id-timeout`. The kernel keeps the request, so a later assignment still applies it. `0` disables the timeout.

`alloc_id_timeout` 是 OMCI 数据通道等待 PLOAM 分配 Alloc-ID 的秒数，超时后数据通道状态变为 `alloc-id-timeout`。内核仍保留该请求，之后分配到时照常生效。`0` 表示不超时。

## CLI

```sh
/etc/init.d/airoha-pond restart

pondctl status --line line0
pondctl datapath --line line0
pondctl events --line line0
pondctl events --line line0 --after 42
```

Run one line in the foreground / 前台运行指定线路：

```sh
airoha-pond --line line0
```
