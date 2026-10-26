/* SPDX-License-Identifier: GPL-2.0-or-later OR MIT */
#ifndef _QCA_PPE_FLOW_H
#define _QCA_PPE_FLOW_H

#include <linux/qca_ppe_natflow.h>
#include "qca_ppe.h"

struct ppe_natflow;
struct ppe_natflow_pair;

struct ppe_flow_data {
	struct ethhdr eth;
	u16 addr_type;
	u8 l4proto;

	__be32 v4_src, v4_dst, v4_src_new, v4_dst_new;
	struct in6_addr v6_src, v6_dst;
	__be16 sport, dport, sport_new, dport_new;

	u16 vlan_id;
	bool vlan_valid;
	u16 ivid;
	u16 pppoe_sid;
	bool pppoe_valid;

	struct net_device *odev;
	/* The profile the entry is given; filled only where the flowtable
	 * reports a priority, and zero means DSCP still decides.
	 */
	u8 priority;
};

struct ppe_flow_entry {
	struct rhash_head node;
	struct list_head list;
	struct flow_block *block;
	struct ppe_natflow_pair *pair;
	unsigned long cookie;
	u32 index;
	u32 words[PPE_FLOW_ENTRY_WORDS_V6];
	u8 nwords;
	u32 hwords[PPE_HOST_ENTRY_WORDS_V6];
	u8 nhwords;
	u8 profile;
	u8 quiet;
	bool sparse;
	u8 src_if;
	u32 host_index;
	int nexthop;
	int my_mac;
	int l3_if;
	int eg_l3_if;
	int pub_ip;
	int uplink;
	int in_uplink;
	int session;
	int xdev;
	unsigned long ct;
	u16 vid;
	u8 xport;
	u8 dscp;
	u8 iport;
	u8 oport;
	u16 ivid;
	u16 ovid;
	u64 packets;
	u64 bytes;
	u64 unread_packets;
	u64 unread_bytes;
	unsigned long last_used;
};

/* All common entry operations require flow_lock, then vlan_lock. */
struct ppe_flow_entry *ppe_flow_create(struct qca_ppe_priv *priv,
				      struct ppe_flow_data *data, int iport,
				      unsigned long cookie,
				      struct flow_block *block);
void ppe_flow_free(struct qca_ppe_priv *priv, struct ppe_flow_entry *entry);
int ppe_flow_poll(struct qca_ppe_priv *priv, struct ppe_flow_entry *entry,
		  u32 *packets, u64 *bytes);
int ppe_flow_src_if(struct qca_ppe_priv *priv, int iport, u16 vid);

int ppe_natflow_init(struct qca_ppe_priv *priv);
void ppe_natflow_exit(struct qca_ppe_priv *priv);
void ppe_natflow_invalidate(struct qca_ppe_priv *priv,
			     struct ppe_natflow_pair *pair);
int ppe_natflow_ft_get(struct qca_ppe_priv *priv);
void ppe_natflow_ft_put(struct qca_ppe_priv *priv);

#endif
