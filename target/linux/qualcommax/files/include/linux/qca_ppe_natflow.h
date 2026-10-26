/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _LINUX_QCA_PPE_NATFLOW_H
#define _LINUX_QCA_PPE_NATFLOW_H

#include <linux/if_ether.h>
#include <linux/types.h>

#define QCA_PPE_NATFLOW_API_VERSION 2

#define QCA_PPE_NATFLOW_VLAN	0x01
#define QCA_PPE_NATFLOW_PPPOE	0x02

struct net_device;

/* Egress encapsulation for one direction. mtu is the inner IP MTU; VLAN and
 * PPPoE headers are additional. VID/SID are in host byte order. The Ethernet
 * header in the base request always describes the inner IPv4 protocol.
 */
struct qca_ppe_natflow_path {
	__be16 vlan_proto;
	u16 vlan_id;
	u16 pppoe_sid;
	u16 mtu;
	u8 flags;
};

struct qca_ppe_natflow_tuple {
	__be32 src, dst;
	__be16 sport, dport;
	u8 proto;
};

enum qca_ppe_natflow_event {
	QCA_PPE_NATFLOW_CHECK,
	QCA_PPE_NATFLOW_READY,
	QCA_PPE_NATFLOW_STATS,
	QCA_PPE_NATFLOW_RELEASE,
};

struct qca_ppe_natflow_stats {
	u64 bytes[2];
	u32 packets[2];
	u8 active;
	int error;
};

/* dev[d] is the ingress of tuple[d], hence the egress of tuple[!d].
 * eth[d] supplies egress MAC addresses and the inner Ethernet protocol.
 * All callbacks run with BH disabled and under RCU, without PPE mutexes.
 * CHECK/READY/STATS return nonzero when the request is no longer valid.
 * RELEASE is called exactly once after a successful submit, including errors.
 * The client owns context until RELEASE; it must validate a connection identity,
 * not merely a reused software hash slot. CHECK must not refresh an idle flow.
 */
struct qca_ppe_natflow_request {
	struct qca_ppe_natflow_tuple tuple[2];
	struct net_device *dev[2];
	struct ethhdr eth[2];
	void *owner;
	void *context;
	int (*notify)(void *context, enum qca_ppe_natflow_event event,
		      const struct qca_ppe_natflow_stats *stats);
};

/* Atomic-context entry point. Copies the request and takes device references.
 * Zero means queued, not installed. On error the client retains context.
 */
int qca_ppe_natflow_submit(const struct qca_ppe_natflow_request *request);

/* API v2: same ownership rules, with independently copied egress paths. Only
 * single 802.1Q and PPPoE carrying IPv4 are accepted. The v1 entry point and
 * base request layout remain unchanged for existing unencapsulated clients.
 */
int qca_ppe_natflow_submit_encap(const struct qca_ppe_natflow_request *request,
			       const struct qca_ppe_natflow_path path[2]);

/* Process context only. The client must first quiesce its submissions. Returns
 * only after this owner's pending/active requests and callbacks are drained.
 * Never call from notify, or while holding RTNL or a PPE/client spinlock.
 */
void qca_ppe_natflow_flush(void *owner);

#endif
