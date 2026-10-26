/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _LINUX_QCA_PPE_NATFLOW_H
#define _LINUX_QCA_PPE_NATFLOW_H

#include <linux/if_ether.h>
#include <linux/types.h>

#define QCA_PPE_NATFLOW_API_VERSION 1

struct net_device;

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
 * eth[d] is the complete egress Ethernet header for direction d.
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

/* Process context only. The client must first quiesce its submissions. Returns
 * only after this owner's pending/active requests and callbacks are drained.
 * Never call from notify, or while holding RTNL or a PPE/client spinlock.
 */
void qca_ppe_natflow_flush(void *owner);

#endif
