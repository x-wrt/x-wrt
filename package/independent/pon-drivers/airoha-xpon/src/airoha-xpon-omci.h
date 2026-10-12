/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_OMCI_H_
#define _AIROHA_XPON_OMCI_H_

#include <linux/types.h>

struct airoha_pon_ctrl_rx_info;
struct airoha_xpon_omci;
struct device;
struct net_device;
struct sk_buff;

struct airoha_xpon_omci *
airoha_xpon_omci_create_rtnl(struct device *dev, struct net_device *pon_netdev);
void airoha_xpon_omci_destroy_rtnl(struct airoha_xpon_omci *omci);

/*
 * The ONU publishes ready, omcc_id, and key_index together after O5, OMCC
 * activation, and OIK programming. A transmit epoch observes one context.
 */
void airoha_xpon_omci_set_link(struct airoha_xpon_omci *omci, bool ready,
                               u16 omcc_id, u8 key_index);

/* The airoha_eth NAPI callback transfers skb ownership to this function. */
void airoha_xpon_omci_receive(struct airoha_xpon_omci *omci,
                              struct sk_buff *skb,
                              const struct airoha_pon_ctrl_rx_info *info);
void airoha_xpon_omci_wake_tx(struct airoha_xpon_omci *omci);

#endif
