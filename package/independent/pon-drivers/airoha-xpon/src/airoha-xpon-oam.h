/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_OAM_H_
#define _AIROHA_XPON_OAM_H_

#include <linux/types.h>

struct airoha_pon_ctrl_rx_info;
struct airoha_xpon_oam;
struct device;
struct net_device;
struct sk_buff;

struct airoha_xpon_oam *
airoha_xpon_oam_create_rtnl(struct device *dev, struct net_device *pon_netdev);
void airoha_xpon_oam_destroy_rtnl(struct airoha_xpon_oam *oam);
void airoha_xpon_oam_set_link(struct airoha_xpon_oam *oam, bool ready, u16 llid,
                              u8 channel);
void airoha_xpon_oam_receive(struct airoha_xpon_oam *oam, struct sk_buff *skb,
                             const struct airoha_pon_ctrl_rx_info *info);
void airoha_xpon_oam_wake_tx(struct airoha_xpon_oam *oam);

#endif
