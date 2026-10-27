# Native PPE flow offload

The IPQ807x and IPQ60xx targets enable the `qca_ppe` DSA switch and `qca_edma`
conduit drivers. Their native netfilter hardware flowtable uses the flow, host,
nexthop, L3 interface and public-address tables:

```
fw4/nft flowtable -> DSA user port -> qca_edma ndo_setup_tc(TC_SETUP_FT)
                 -> qca_ppe flow block -> PPE tables
```

It requires no NSS firmware or ECM. The nft path uses the standard netfilter
interface. A separate versioned API now connects natflow as described below;
the MTK/Airoha private `ndo_flow_offload` ABI is unchanged.

## Natflow control plane

Build the matching natflow feed patch with this kernel. Kbuild detects
`include/linux/qca_ppe_natflow.h`; the client enables the versioned API only
with PPE, natflow PATH and netfilter ingress support. No external device or MTK ABI is
required. API v2 admits IPv4 TCP/UDP on two native user ports of the same PPE,
including one 802.1Q tag, PPPoE, or PPPoE over one 802.1Q tag. The client learns
the physical ports, tags and PPPoE headers from traffic and sends a copied path
and inner IP MTU per direction. API v1 and its original request layout remain
available for unencapsulated clients; a new client on an API v1 kernel keeps
that limited scope. Stacked VLANs, S-tags, IPv6, L2 bridge forwarding, conntrack
zones, software token control and external devices stay in software.
Flows that the native PPE path would hand to a CPU shaper also stay in software
for natflow; its API does not provide the native flowtable cookie required by
that path. Native nft shaper support remains available.

The atomic submit copies both tuples and Ethernet headers, holds both devices,
and queues an ordered worker. The worker uses the same entry/resource functions
as nft, rolls back both directions on error, and reports READY separately from
queue acceptance. The client retains the conntrack and validates the software
slots, per-slot publication generations, path generation and policy before
installation and on every poll. The driver does not retain per-CPU or stack
request pointers and does not require software hashes to match hardware slots.
Both ingress classifications and PPPoE sessions are prepared before either
rule is installed, including when the original direction arrives through
PPPoE. Seed references remain until both rules are removed, so a failure or
control-plane purge cannot free a path still used by its reverse direction.
The egress limit is the smaller of the physical frame limit and the learned IP
MTU plus the headers the PPE adds. Session IDs, peers, VLANs, devices and MTUs
are rechecked in client callbacks. PPP/VLAN device events conservatively remove
all installed natflow pairs immediately; queued requests are revalidated by
the worker. Link up/change also invalidates the client's learned generation.

An active pair is polled every second. Real packet/byte deltas, or the hardware's
coarse maximum-age indication, establish activity; an inactive poll does not
refresh the software entries. After 20 seconds without observed activity the
pair is retired, before natflow's 25-second software timeout. This is an
activity estimate when only age advances, not a substitute for real counters.
The native sparse-flow worker skips natflow entries so the pair worker alone
consumes their hardware counter deltas. The PPE credits bridge/VLAN counters;
natflow credits conntrack/fakeuser counters and rates, avoiding duplicate
netdevice accounting. Per-type hardware counter
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

- Ingress resolves to a user port of the PPE switch. Wired egress, VLAN and
  PPPoE paths share the native resource tables. Upstream also supports eligible
  Ethernet egress devices through the CPU port, including Wi-Fi forwarding,
  and can hand shaped flows to a software qdisc.
- IPv4 routing, SNAT or DNAT, and IPv6 routing; TCP, UDP and GRE flowtable rules.
- Bridge VLANs, standalone 802.1Q VLAN interfaces and PPPoE, including a tagged
  PPPoE WAN. Sixteen session entries are shared and match the server MAC too.
- Each direction is a separate hardware rule. Hardware aging, delayed stats,
  and teardown of a flowtable or routing domain release the shared resources.
- IPQ8074 exposes 4096 flow slots; IPQ6018 exposes 2048. IPv6 uses two slots.
  Host, nexthop, MY_MAC, VSI and public-IP resources can limit capacity first.

Stacked VLANs, IPv6 NAT, and a rule needing SNAT and DNAT together stay in
software. The natflow API retains its narrower wired-port scope above.
IPQ50xx is outside this driver implementation.
Fragments bypass flow lookup; FIN/SYN/RST, expired TTL, IP options, extension
headers and malformed headers are directed to the CPU for normal processing.
The native CPU-forwarding, QCA in-band tagging, QoS and sparse-flow priority
paths follow upstream OpenWrt. They require no NSS firmware.

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

- The complete generic and qualcommax kernel patch stack applies to the
  Linux 6.18.55 sources. Netfilter VLAN fixes come from the generic upstream
  stack; duplicate target-local copies are removed.
- `qca_ppe.o` (all component objects), `qca_edma.o` and the QCA tagger
  cross-compile for ARM64 with `aarch64-linux-gnu-gcc` and NF_FLOW_TABLE modular.
  These are object builds, not a complete firmware or module-load test.
- `python3 target/linux/qualcommax/tests/ppe-flow.py` compiles the actual driver's
  command functions with ASan/UBSan against a register mock. It checks consuming
  result reads, command ID wrap, bounds, failures/timeouts and counter carry.
  Requires Python 3 and a C compiler with these sanitizers.
- `python3 target/linux/qualcommax/tests/ppe-natflow.py --natflow /path/to/natflow`
  runs the actual worker, shared release and client code against deterministic
  host mocks with ASan/UBSan. It covers pending/ready admission, SNAT/DNAT tuple
  direction, pair rollback, duplicate ownership, software-slot ABA, policy and
  device invalidation, retry, counters, idle expiry, nft exclusion and teardown.
  Encapsulation cases also exercise the production path preparation, release
  and MTU calculation, including an inbound PPPoE original direction, tagged
  PPPoE, path changes, allocation failures and a full 16-entry session table.
  It does not emulate concurrent execution, MMIO forwarding or device traffic.

No device boot, register readback, NAT correctness or throughput test has been
performed locally. Verify both directions of TCP/UDP IPv4 NAT and IPv6 routing,
then VLAN/PPPoE, connection aging, firewall reload, port down, MTU/MAC changes and
unsupported-path fallback. Confirm offload counters and actual hardware traffic
alongside CPU reduction; a successful rule-add counter alone is insufficient.

## Provenance

The native flowtable implementation, CPU forwarding, QCA tagger and QoS paths
come from upstream OpenWrt, including the series developed in
[PR #24806](https://github.com/openwrt/openwrt/pull/24806). The generic upstream
stack also contains the VLAN match-key and VLAN-ID flowtable fixes.

Local additions check command errors and returned indices, latch uncertain
commands with lookup disabled, bound counter retries, and add the versioned
natflow API and host tests above. Natflow shares native entry/resource functions
while keeping pair lifetime, counter polling and control-plane ownership
separate. Upstream device measurements do not validate these local additions.
