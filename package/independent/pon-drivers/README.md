# WIP Airoha PON kernel drivers

PON MAC and optical frontend drivers for Airoha AN7581/AN7583.

Airoha AN7581/AN7583 的 PON MAC 与光前端驱动。

Work in progress. APIs may change substantially.

开发中，API 可能发生大幅变更。

## Optical frontends / 光前端

| Device / 器件 | Driver / 驱动 |
| --- | --- |
| EN7572 | `airoha-en7572` |
| GN28L95 | `airoha-paged-bosa` |
| UX3363 | `airoha-paged-bosa` |

## Protocols / 协议

| Mode / 模式 | Status / 状态 |
| --- | --- |
| GPON | ❌ |
| XG-PON | ✅️ |
| XGS-PON | ❓ |
| EPON 1G/1G | ❌ |
| 10G-EPON 10G/1G | ✅️ |
| 10G-EPON 10G/10G | ❓ |

- ✅ tested / 已测试
- ❓ untested / 未测试
- ❌ not yet implemented due to the lack of compatible devices / 我没有找到兼容设备，暂未实现
