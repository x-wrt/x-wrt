# Native PPE flow offload

The IPQ807x and IPQ60xx targets already enable the `qca_ppe` DSA switch and
`qca_edma` conduit drivers. Their bridge/VLAN, ACL and scheduling support alone
does not offload routed connections. This change connects the netfilter hardware
flowtable to the flow, host, nexthop, L3 interface and public-address tables:

```
fw4/nft flowtable -> DSA user port -> qca_edma ndo_setup_tc(TC_SETUP_FT)
                 -> qca_ppe flow block -> PPE tables
```

It requires no NSS firmware or ECM. It uses the standard netfilter interface;
it does not implement natflow's private `ndo_flow_offload` ABI. Test without a
competing software fast-forwarding service taking packets ahead of netfilter.

## Scope

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

## Enable and inspect on a device

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
