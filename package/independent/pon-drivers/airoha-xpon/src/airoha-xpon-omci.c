// SPDX-License-Identifier: GPL-2.0-only
/* The raw-PDU interface starts at the transaction ID's high byte. */

#include <linux/airoha-eth.h>
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/if_arp.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>

#include "airoha-xpon-omci.h"

#define AIROHA_OMCI_BASELINE_WITHOUT_MIC_LEN 44
#define AIROHA_OMCI_BASELINE_WITH_MIC_LEN    48
#define AIROHA_OMCI_HEADER_LEN               10
#define AIROHA_OMCI_MAX_PDU_LEN              1980
#define AIROHA_OMCI_BASELINE_DEVICE_ID       0x0a
#define AIROHA_OMCI_EXTENDED_DEVICE_ID       0x0b

#define AIROHA_OMCI_CTX_OMCC_MASK GENMASK(15, 0)
#define AIROHA_OMCI_CTX_KEY_INDEX BIT(16)
#define AIROHA_OMCI_CTX_READY     BIT(31)

struct airoha_xpon_omci {
	struct device *parent;
	struct net_device *pon_netdev;
	struct net_device *netdev;

	/* A single context publishes OMCC and OIK for the same O5 epoch. */
	u32 tx_context;
};

static int airoha_xpon_omci_iflink(const struct net_device *netdev)
{
	const struct airoha_xpon_omci *omci = netdev_priv(netdev);

	/* iflink ties the OMCI netdev lifetime to its parent PON netdev. */
	return READ_ONCE(omci->pon_netdev->ifindex);
}

static int airoha_xpon_omci_open(struct net_device *netdev)
{
	struct airoha_xpon_omci *omci = netdev_priv(netdev);
	u32 context = READ_ONCE(omci->tx_context);

	netif_start_queue(netdev);
	if (context & AIROHA_OMCI_CTX_READY)
		netif_carrier_on(netdev);
	else
		netif_carrier_off(netdev);

	return 0;
}

static int airoha_xpon_omci_stop(struct net_device *netdev)
{
	netif_tx_disable(netdev);
	netif_carrier_off(netdev);
	return 0;
}

static int airoha_xpon_omci_normalize_tx(struct sk_buff *skb)
{
	u16 expected;
	u8 device_id;

	/* Baseline and extended OMCI share a ten-byte header. */
	if (skb->len > AIROHA_OMCI_MAX_PDU_LEN ||
	    !pskb_may_pull(skb, AIROHA_OMCI_HEADER_LEN))
		return -EMSGSIZE;

	device_id = skb->data[3];
	if (device_id == AIROHA_OMCI_BASELINE_DEVICE_ID) {
		if (skb->len == AIROHA_OMCI_BASELINE_WITH_MIC_LEN)
			return pskb_trim(skb,
			                 AIROHA_OMCI_BASELINE_WITHOUT_MIC_LEN);
		return skb->len == AIROHA_OMCI_BASELINE_WITHOUT_MIC_LEN ?
		               0 :
		               -EMSGSIZE;
	}

	if (device_id != AIROHA_OMCI_EXTENDED_DEVICE_ID)
		return -EPROTONOSUPPORT;

	/* Extended header bytes 8–9 carry the network-order content length. */
	expected = AIROHA_OMCI_HEADER_LEN + get_unaligned_be16(skb->data + 8);
	if (expected > AIROHA_OMCI_MAX_PDU_LEN)
		return -EMSGSIZE;
	if (skb->len == expected + 4)
		return pskb_trim(skb, expected);

	return skb->len == expected ? 0 : -EMSGSIZE;
}

static netdev_tx_t airoha_xpon_omci_xmit(struct sk_buff *skb,
                                         struct net_device *netdev)
{
	struct airoha_xpon_omci *omci = netdev_priv(netdev);
	struct airoha_pon_ctrl_tx_info info = {
		.type = AIROHA_PON_CTRL_OMCI,
	};
	u32 context = READ_ONCE(omci->tx_context);
	int ret;

	/* Recheck READY, OMCC ID, and OIK from one snapshot after TX queueing. */
	if (!(context & AIROHA_OMCI_CTX_READY))
		goto drop;

	ret = airoha_xpon_omci_normalize_tx(skb);
	if (ret)
		goto drop;

	info.id = FIELD_GET(AIROHA_OMCI_CTX_OMCC_MASK, context);
	info.key_index = !!(context & AIROHA_OMCI_CTX_KEY_INDEX);
	ret = airoha_eth_pon_xmit_control(omci->pon_netdev, netdev, skb, &info);
	return ret;

drop:
	netdev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops airoha_xpon_omci_netdev_ops = {
	.ndo_open = airoha_xpon_omci_open,
	.ndo_stop = airoha_xpon_omci_stop,
	.ndo_start_xmit = airoha_xpon_omci_xmit,
	.ndo_get_iflink = airoha_xpon_omci_iflink,
};

static void airoha_xpon_omci_setup(struct net_device *netdev)
{
	/* AF_PACKET receives OMCI PDUs without a synthetic link-layer header. */
	netdev->type = ARPHRD_NONE;
	netdev->hard_header_len = 0;
	netdev->addr_len = 0;
	netdev->mtu = AIROHA_OMCI_MAX_PDU_LEN;
	netdev->min_mtu = AIROHA_OMCI_HEADER_LEN;
	netdev->max_mtu = AIROHA_OMCI_MAX_PDU_LEN;
	netdev->tx_queue_len = 32;
	netdev->flags = IFF_POINTOPOINT | IFF_NOARP;
	netdev->netdev_ops = &airoha_xpon_omci_netdev_ops;
	netdev->needs_free_netdev = true;
}

struct airoha_xpon_omci *
airoha_xpon_omci_create_rtnl(struct device *dev, struct net_device *pon_netdev)
{
	struct airoha_xpon_omci *omci;
	struct net_device *netdev;
	int ret;

	ASSERT_RTNL();
	netdev = alloc_netdev(sizeof(*omci), "omci%d", NET_NAME_ENUM,
	                      airoha_xpon_omci_setup);
	if (!netdev)
		return ERR_PTR(-ENOMEM);

	omci = netdev_priv(netdev);
	omci->parent = dev;
	omci->pon_netdev = pon_netdev;
	omci->netdev = netdev;
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
		"OMCI raw-PDU interface %s registered; carrier remains off until O5\n",
		netdev->name);
	return omci;
}

void airoha_xpon_omci_destroy_rtnl(struct airoha_xpon_omci *omci)
{
	if (!omci)
		return;

	ASSERT_RTNL();
	unregister_netdevice(omci->netdev);
}

void airoha_xpon_omci_set_link(struct airoha_xpon_omci *omci, bool ready,
                               u16 omcc_id, u8 key_index)
{
	u32 context;

	if (!omci)
		return;

	context = FIELD_PREP(AIROHA_OMCI_CTX_OMCC_MASK, omcc_id);
	if (key_index)
		context |= AIROHA_OMCI_CTX_KEY_INDEX;
	if (ready)
		context |= AIROHA_OMCI_CTX_READY;
	WRITE_ONCE(omci->tx_context, context);

	if (!ready) {
		netif_carrier_off(omci->netdev);
		netif_tx_disable(omci->netdev);
	} else if (netif_running(omci->netdev)) {
		netif_carrier_on(omci->netdev);
		netif_tx_wake_all_queues(omci->netdev);
	}
}

void airoha_xpon_omci_receive(struct airoha_xpon_omci *omci,
                              struct sk_buff *skb,
                              const struct airoha_pon_ctrl_rx_info *info)
{
	u32 context;
	u16 omcc_id;

	context = READ_ONCE(omci->tx_context);
	omcc_id = FIELD_GET(AIROHA_OMCI_CTX_OMCC_MASK, context);
	/* Keep the trailing MIC in the raw-PDU ABI for daemon-level validation. */
	if (!(context & AIROHA_OMCI_CTX_READY) ||
	    !netif_running(omci->netdev) || info->id != omcc_id) {
		omci->netdev->stats.rx_dropped++;
		dev_kfree_skb_any(skb);
		return;
	}

	skb->dev = omci->netdev;
	skb->protocol = 0;
	skb->pkt_type = PACKET_HOST;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);

	omci->netdev->stats.rx_packets++;
	omci->netdev->stats.rx_bytes += skb->len;
	netif_receive_skb(skb);
}

void airoha_xpon_omci_wake_tx(struct airoha_xpon_omci *omci)
{
	if (!omci || !netif_running(omci->netdev) ||
	    !(READ_ONCE(omci->tx_context) & AIROHA_OMCI_CTX_READY))
		return;

	if (netif_queue_stopped(omci->netdev))
		netif_wake_queue(omci->netdev);
}
