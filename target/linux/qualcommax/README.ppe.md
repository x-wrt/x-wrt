# Native PPE flow offload

The IPQ807x and IPQ60xx targets already enable the `qca_ppe` DSA switch and
`qca_edma` conduit drivers. Their bridge/VLAN, ACL and scheduling support alone
does not offload routed connections. This change connects the netfilter hardware
flowtable to the flow, host, nexthop, L3 interface and public-address tables:

```
fw4/nft flowtable -> DSA user port -> qca_edma ndo_setup_tc(TC_SETUP_FT)
                 -> qca_ppe flow block -> PPE tables
```

It requires no NSS firmware or ECM. The nft path uses the standard netfilter
interface. A separate versioned API now connects natflow as described below;
the MTK/Airoha private `ndo_flow_offload` ABI is unchanged.

## Natflow control plane

Build the matching natflow feed patch with this kernel. Kbuild detects
`include/linux/qca_ppe_natflow.h`; the client enables API v1 only with PPE,
natflow PATH and netfilter ingress support. No external device or MTK ABI is
required. The first version admits IPv4 TCP/UDP on two native user ports of the
same PPE, with complete Ethernet headers and no explicit VLAN/PPPoE
encapsulation, IPv6, L2 bridge forwarding, conntrack zones or software token
control. Unsupported flows continue through the existing software path.

The atomic submit copies both tuples and Ethernet headers, holds both devices,
and queues an ordered worker. The worker uses the same entry/resource functions
as nft, rolls back both directions on error, and reports READY separately from
queue acceptance. The client retains the conntrack and validates the software
slots, per-slot publication generations, path generation and policy before
installation and on every poll. The driver does not retain per-CPU or stack
request pointers and does not require software hashes to match hardware slots.

An active pair is polled every second. Real packet/byte deltas, or the hardware's
coarse maximum-age indication, establish activity; an inactive poll does not
refresh the software entries. After 20 seconds without observed activity the
pair is retired, before natflow's 25-second software timeout. This is an
activity estimate when only age advances, not a substitute for real counters.
The PPE credits bridge/VLAN counters; natflow credits conntrack/fakeuser counters
and rates, avoiding duplicate netdevice accounting. Per-type hardware counter
behavior still needs device validation.

Native nft hardware flowtable blocks and queued/active natflow requests are
mutually exclusive on each PPE. For a natflow test, disable both standard
firewall flow-offload settings and restart the firewall first, then enable
natflow's `hwnat=1` and `disabled=0` through `/dev/natflow_ctl`. The companion
natflow change makes `hwnat=0` and `disabled=1` quiesce submissions and drain
requests before returning. Module exit drains all callbacks before freeing
software slots. Driver removal also drains pending requests before DSA user
devices are unregistered.

Debugfs `offload` additionally reports `natflow_requests`, `natflow_installed`,
`natflow_failed` and `nft_blocks`. `natflow_installed` counts successful pairs;
the shared `offloaded` counter counts individual direction entries.

## Native nft scope

- Both ingress and egress must resolve to user ports of the same PPE switch.
- IPv4 routing, SNAT or DNAT, and IPv6 routing; TCP, UDP and GRE flowtable rules.
- Bridge VLANs, standalone 802.1Q VLAN interfaces and PPPoE, including a tagged
  PPPoE WAN. Sixteen session entries are shared and match the server MAC too.
- Each direction is a separate hardware rule. Hardware aging, delayed stats,
  and teardown of a flowtable or routing domain release the shared resources.
- IPQ8074 exposes 4096 flow slots; IPQ6018 exposes 2048. IPv6 uses two slots.
  Host, nexthop, MY_MAC, VSI and public-IP resources can limit capacity first.

Wi-Fi/external-switch paths, stacked VLANs, IPv6 NAT, and a rule needing SNAT and
DNAT together stay in software. IPQ50xx is outside this driver implementation.
Fragments bypass flow lookup; FIN/SYN/RST, expired TTL, IP options, extension
headers and malformed headers are directed to the CPU for normal processing.
No CPU-to-Wi-Fi forwarding, NSS or new shaper datapath is imported here.

The hardware chooses hash buckets and slots using key-mode operations. Both
hash blocks are explicitly configured for XOR (mode 1); the driver reads back
the assigned flow slot and host index. This is different from MediaTek's
software-computed hash and adjacent-slot probing. No Qualcomm CRC polynomial
or software reproduction of its slot selection is needed for this path.

## Enable and inspect native nft offload

Build an IPQ807x/IPQ60xx image with firewall4 and `kmod-nft-offload`. The existing
subtarget configuration enables PPE/EDMA; no change to the workspace's selected
build target is required. On the device, enable standard hardware flow offload:

```sh
uci set firewall.@defaults[0].flow_offloading='1'
uci set firewall.@defaults[0].flow_offloading_hw='1'
uci commit firewall
/etc/init.d/firewall restart
```

Verify `nft list ruleset` includes a flowtable with
`flags offload` and the intended ports. With debugfs mounted, PPE diagnostics
live below `/sys/kernel/debug/<PPE platform device>/`, commonly
`/sys/kernel/debug/3a000000.ppe/` (use the actual device name):

- `offload`: successful additions, live entries, reinstallations, refusals by
  reason, active uplinks, `faulted` and `op_errors`.
- `flows`: hardware flow entries with type, host index, age and hit counters.
- Existing `ingress`, `egress` and `drop_code` files help diagnose exceptions.

A command timeout or failed register access in the flow operation engine
latches `faulted` and attempts to disable global flow lookup before side-table
resources are released. Further flow operations fail until the PPE is reset
and reprobed; reboot is the straightforward recovery. A table-full/missing-entry
result is an ordinary refusal and does not latch this fault.

## Validation and remaining device checks

Host validation on Linux 6.18.55 with this tree's generic and qualcommax patches:

- The complete qualcommax target patch stack applies. The two modified driver
  build patches and two added netfilter fixes also pass a zero-fuzz reverse/apply
  round trip. Existing unrelated DSA patch context still requires fuzz.
- `qca_ppe.o` (all component objects) and `qca_edma.o` compile with x86 COMPILE_TEST,
  with NF_FLOW_TABLE enabled and disabled. The changed netfilter objects compile
  with it enabled. The PPE, EDMA and changed netfilter objects also cross-compile
  for ARM64 with `aarch64-linux-gnu-gcc`, with NF_FLOW_TABLE modular. These are
  object builds, not a complete firmware build or a module-load test.
- `python3 target/linux/qualcommax/tests/ppe-flow.py` compiles the actual driver's
  command functions with ASan/UBSan against a register mock. It checks consuming
  result reads, command ID wrap, bounds, failures/timeouts and counter carry.
  Requires Python 3 and a C compiler with these sanitizers.
- `python3 target/linux/qualcommax/tests/ppe-natflow.py --natflow /path/to/natflow`
  runs the actual worker, shared release and client code against deterministic
  host mocks with ASan/UBSan. It covers pending/ready admission, SNAT/DNAT tuple
  direction, pair rollback, duplicate ownership, software-slot ABA, policy and
  device invalidation, retry, counters, idle expiry, nft exclusion and teardown.
  It does not emulate concurrent execution, MMIO forwarding or device traffic.
- The matching natflow `20261018` package archive matches its declared SHA256;
  the feed patch applies with zero fuzz and reproduces the local client sources.
  Its PATH/URLLOGGER/DPI configuration and DKMS source copy cross-compile for
  ARM64. API-disabled and legacy MTK fake-ABI client builds also pass; symbol
  checks confirm which backend was compiled. This checks client compatibility,
  not the MediaTek drivers or hardware.

No device boot, register readback, NAT correctness or throughput test has been
performed locally. Verify both directions of TCP/UDP IPv4 NAT and IPv6 routing,
then VLAN/PPPoE, connection aging, firewall reload, port down, MTU/MAC changes and
unsupported-path fallback. Confirm offload counters and actual hardware traffic
alongside CPU reduction; a successful rule-add counter alone is insufficient.

## Provenance

The wired implementation is selected from OpenWrt
[PR #24806](https://github.com/openwrt/openwrt/pull/24806), at head
`c7924dcd74a6bd30abd5f935209528c071ddd287`:

```
1152a41775ca  record VSI 0 for a port outside a bridge
3844ee698bd8  PPE flow table access and debugfs
882844eb2319  netfilter flowtable hardware offload
46e3391b905f  GRE flowtable flows
59c1ffad8b37  ingress VLAN to VSI
114c4091f14d  slow-path exceptions to CPU
aaa6875d7361  all sixteen PPPoE session entries
3945d5a8ed3b  standalone/non-filtering bridge VLAN routing
f3aac46ea7bc  explicit PPE/EDMA build dependencies
1b90dab6216e  advertise netfilter VLAN match keys
be75fbfa9b26  use VLAN ID instead of full TCI in flow tuples
```

The netfilter fixes are target-scoped here to avoid changing other targets'
flowtable implementations. Local additions check operation errors and returned
indices, latch uncertain commands with lookup disabled, bound counter retries,
and add the mock tests above. Upstream's reported device measurements apply to
the upstream series, not to this selected backport.
