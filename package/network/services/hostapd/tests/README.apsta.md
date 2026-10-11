# AP+STA channel policy

| STA state | AP behavior |
| --- | --- |
| Initial startup, no uplink | Start with the configured channel and width |
| Connecting on another channel | Keep AP enabled, use CSA, then authenticate STA |
| Connected | Follow the uplink primary channel; try the configured width ceiling, then narrower legal widths |
| Link lost, scanning, authentication failure, reconnecting | Retain the last operating channel and width |
| Administratively disabled, interface down, or removed | Use CSA to restore the configured channel and width |
| `channel=auto`, then STA disabled | Restore the AP's initial automatic selection |
| CSA unsupported, rejected, or timed out | Keep the existing AP running; retry STA instead of tearing AP down |

Both the legacy shell and ucode wireless configuration paths are covered. Mesh
and adhoc retain their existing start/stop behavior. MLD STA notifications are
per radio; candidate usable links are prepared before authentication, and a
missing link does not reset its AP.

The configured width is an upper bound. Hardware capabilities, permitted
subchannels, and DFS availability can require a narrower width. A DFS target
requiring CAC cannot be adopted while continuously transmitting this AP.
Changes to SSID, encryption, hardware capabilities, or `wifi down` for the whole
radio are not STA-only operations and can still rebuild/stop the AP.

## Host regression

Run `sh package/network/services/hostapd/tests/run-apsta.sh` from the repository
root after exporting these paths:

- `HOSTAPD_TREE`: exact package source with `src/` copied in and all patches applied.
- `UCODE`: host ucode executable, matching the package's version.
- `UCODE_INCLUDE`, `UCODE_LIB_DIR`: ucode headers and shared library/module directory.
- `APSTA_CFLAGS`: compiler include flags for libubox, ubus, udebug and json-c headers.

The tests execute the complete production ucode scripts with mocked daemon,
ubus and timer adapters. Frequency calculations use the real C helper linked
against the expanded hostapd source, not a reimplementation. They cover cold
startup, authentication gating, retention, restoration, in-flight disable,
interface removal/recreation, reload classification, width fallback, 2.4 GHz
HT40, 6 GHz EHT, MLD notifications and CSA failure. They do not prove actual
driver CSA or end-to-end netifd behavior.

The C pending-work regression additionally uses the real prepare/cancel and
work-free code with mocked daemon edges. It checks readiness, timer callbacks,
cancellation, timeout, roaming, same-channel width changes and timer allocation
failure under AddressSanitizer/UBSan. The default flags target GCC; override
`APSTA_WORK_FLAGS` if a different compiler requires other sanitizer flags.

## Device acceptance

Use a supported radio/driver and an independently connected AP client. Observe
`iw dev`, `ubus call hostapd status` and wireless logs through each transition:

1. Boot with an unavailable STA SSID: AP starts on configured settings.
2. Connect STA on a different legal channel: AP switches by CSA, not AP teardown.
3. Remove the upstream signal, then cause failed authentication/retries: AP
   remains usable and retains its followed channel/width.
4. Disable/re-enable STA, bring only its netdev down/up, and remove/re-add its
   interface: disabling/removal restores configured settings; AP is not recreated.
5. Disable during CSA, change upstream channel, and exercise width fallback.
6. For MLD, repeat loss/disable for each radio and verify unused links retain
   their channel.

Record client traffic and AP netdev/hostapd BSS lifetimes. CSA and off-channel
scans can briefly interrupt airtime; retaining the AP interface does not prove
zero packet loss or guarantee every client remains associated. Hardware
acceptance is required before making a hitless claim.
