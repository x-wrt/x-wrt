// SPDX-License-Identifier: GPL-2.0-only
/* The raw-PDU interface carries complete OAMPDUs in skb->data. */

#include <linux/airoha-eth.h>
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/if_arp.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>

#include "airoha-xpon-oam.h"

#define AIROHA_OAM_MAX_PDU_LEN      1500
#define AIROHA_OAM_CTX_LLID_MASK    GENMASK(15, 0)
#define AIROHA_OAM_CTX_CHANNEL_MASK GENMASK(20, 16)
#define AIROHA_OAM_CTX_READY        BIT(31)

struct airoha_xpon_oam {
	struct net_device *pon_netdev;
	struct net_device *netdev;
	u32 tx_context;
};

static int airoha_xpon_oam_iflink(const struct net_device *netdev)
{
	const struct airoha_xpon_oam *oam = netdev_priv(netdev);

	return READ_ONCE(oam->pon_netdev->ifindex);
}

static int airoha_xpon_oam_open(struct net_device *netdev)
{
	struct airoha_xpon_oam *oam = netdev_priv(netdev);

	netif_start_queue(netdev);
	if (READ_ONCE(oam->tx_context) & AIROHA_OAM_CTX_READY)
		netif_carrier_on(netdev);
	else
		netif_carrier_off(netdev);
	return 0;
}

static int airoha_xpon_oam_stop(struct net_device *netdev)
{
	netif_tx_disable(netdev);
	netif_carrier_off(netdev);
	return 0;
}

static netdev_tx_t airoha_xpon_oam_xmit(struct sk_buff *skb,
                                        struct net_device *netdev)
{
	struct airoha_xpon_oam *oam = netdev_priv(netdev);
	struct airoha_pon_ctrl_tx_info info = {
		.type = AIROHA_PON_CTRL_EPON_OAM,
	};
	u32 context = READ_ONCE(oam->tx_context);

	if (!(context & AIROHA_OAM_CTX_READY))
		goto drop;
	if (!skb->len || skb->len > AIROHA_OAM_MAX_PDU_LEN)
		goto drop;

	info.id = FIELD_GET(AIROHA_OAM_CTX_LLID_MASK, context);
	info.channel = FIELD_GET(AIROHA_OAM_CTX_CHANNEL_MASK, context);
	return airoha_eth_pon_xmit_control(oam->pon_netdev, netdev, skb, &info);

drop:
	netdev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops airoha_xpon_oam_netdev_ops = {
	.ndo_open = airoha_xpon_oam_open,
	.ndo_stop = airoha_xpon_oam_stop,
	.ndo_start_xmit = airoha_xpon_oam_xmit,
	.ndo_get_iflink = airoha_xpon_oam_iflink,
};

static void airoha_xpon_oam_setup(struct net_device *netdev)
{
	/* AF_PACKET sees the OAMPDU without a synthetic link-layer header. */
	netdev->type = ARPHRD_NONE;
	netdev->hard_header_len = 0;
	netdev->addr_len = 0;
	netdev->mtu = AIROHA_OAM_MAX_PDU_LEN;
	netdev->min_mtu = 1;
	netdev->max_mtu = AIROHA_OAM_MAX_PDU_LEN;
	netdev->tx_queue_len = 32;
	netdev->flags = IFF_POINTOPOINT | IFF_NOARP;
	netdev->netdev_ops = &airoha_xpon_oam_netdev_ops;
	netdev->needs_free_netdev = true;
}

struct airoha_xpon_oam *
airoha_xpon_oam_create_rtnl(struct device *dev, struct net_device *pon_netdev)
{
	struct airoha_xpon_oam *oam;
	struct net_device *netdev;
	int ret;

	ASSERT_RTNL();
	netdev = alloc_netdev(sizeof(*oam), "oam%d", NET_NAME_ENUM,
	                      airoha_xpon_oam_setup);
	if (!netdev)
		return ERR_PTR(-ENOMEM);

	oam = netdev_priv(netdev);
	oam->pon_netdev = pon_netdev;
	oam->netdev = netdev;
	SET_NETDEV_DEV(netdev, dev);
	dev_net_set(netdev, dev_net(pon_netdev));
	netif_carrier_off(netdev);

	ret = register_netdevice(netdev);
	if (ret) {
		free_netdev(netdev);
		return ERR_PTR(ret);
	}

	dev_info(
		dev,
		"OAM raw-PDU interface %s registered; carrier remains off until an LLID is active\n",
		netdev->name);
	return oam;
}

void airoha_xpon_oam_destroy_rtnl(struct airoha_xpon_oam *oam)
{
	if (!oam)
		return;

	ASSERT_RTNL();
	unregister_netdevice(oam->netdev);
}

void airoha_xpon_oam_set_link(struct airoha_xpon_oam *oam, bool ready, u16 llid,
                              u8 channel)
{
	u32 context;

	if (!oam)
		return;

	context = FIELD_PREP(AIROHA_OAM_CTX_LLID_MASK, llid) |
	          FIELD_PREP(AIROHA_OAM_CTX_CHANNEL_MASK, channel);
	if (ready)
		context |= AIROHA_OAM_CTX_READY;
	WRITE_ONCE(oam->tx_context, context);

	if (!ready) {
		netif_carrier_off(oam->netdev);
		netif_tx_disable(oam->netdev);
	} else if (netif_running(oam->netdev)) {
		netif_carrier_on(oam->netdev);
		netif_tx_wake_all_queues(oam->netdev);
	}
}

void airoha_xpon_oam_receive(struct airoha_xpon_oam *oam, struct sk_buff *skb,
                             const struct airoha_pon_ctrl_rx_info *info)
{
	u32 context = READ_ONCE(oam->tx_context);

	if (!(context & AIROHA_OAM_CTX_READY) || !netif_running(oam->netdev) ||
	    info->id != FIELD_GET(AIROHA_OAM_CTX_LLID_MASK, context) ||
	    info->channel != FIELD_GET(AIROHA_OAM_CTX_CHANNEL_MASK, context)) {
		oam->netdev->stats.rx_dropped++;
		dev_kfree_skb_any(skb);
		return;
	}

	skb->dev = oam->netdev;
	skb->protocol = 0;
	skb->pkt_type = PACKET_HOST;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	oam->netdev->stats.rx_packets++;
	oam->netdev->stats.rx_bytes += skb->len;
	netif_receive_skb(skb);
}

void airoha_xpon_oam_wake_tx(struct airoha_xpon_oam *oam)
{
	if (!oam || !netif_running(oam->netdev) ||
	    !(READ_ONCE(oam->tx_context) & AIROHA_OAM_CTX_READY))
		return;

	if (netif_queue_stopped(oam->netdev))
		netif_wake_queue(oam->netdev);
}
