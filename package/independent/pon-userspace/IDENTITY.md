# Board identity layout

Board identity fields are declared in `target/linux/airoha/base-files/etc/board.d/03_pon_data` and exported through `/etc/board.json`.

## Fields

XG2010G factory example:

```json
{
  "identity": {
    "targets": {
      "factory": {
        "fields": {
          "pon_sn":       { "kind": "pon-sn", "offset": 210, "size": 12, "encoding": "pon-ascii12" },
          "pon_mac":      { "kind": "mac", "offset": 108, "size": 17, "encoding": "mac-colon17" },
          "lan_base_mac": { "kind": "mac", "offset": 134, "size": 17, "encoding": "mac-colon17" }
        }
      }
    }
  }
}
```

Each identity target references a `pon_data` target. Field names may include a role or index when one target stores multiple MAC addresses or serial numbers.

## Encodings

| Encoding | Stored representation |
| --- | --- |
| `mac-binary6` | Six MAC bytes |
| `mac-hex12` | Twelve ASCII hexadecimal characters |
| `mac-colon17` | Seventeen ASCII characters with separators |
| `pon-ascii12` | Four vendor characters and eight ASCII hexadecimal characters |
| `pon-binary8` | Four vendor bytes and four serial bytes |
| `serial-text` | Fixed-size device serial field |

## Target codecs

Nokia RI example:

```json
{
  "identity": {
    "targets": {
      "ri": {
        "codec": "nokia-ri-v1",
        "fields": {
          "pon_sn":   { "kind": "pon-sn", "offset": 26, "size": 12, "encoding": "pon-ascii12", "vssn_offset": 88 },
          "board_mac": { "kind": "mac", "offset": 62, "size": 6, "encoding": "mac-binary6" }
        }
      }
    }
  }
}
```

`nokia-ri-v1` updates the raw VSSN copy and both 128-byte record checksums.

## Editor

```text
pon-board-identity list
pon-board-identity read TARGET FIELD
pon-board-identity write TARGET FIELD=VALUE ...
```

`write` updates one target, writes the complete logical image, and verifies its readback. The original image is saved as `/tmp/pon-board-identity.*.bin`. LuCI displays the identity editor when the board declares writable fields.
