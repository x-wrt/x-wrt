// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define CONFIG_NET_MEDIATEK_SOC_WED 1
#define IS_ENABLED(value) (value)
#define FLOW_OFFLOAD_PATH_EXTDEV (1U << 0)
#define FLOW_OFFLOAD_PATH_WED_DIS (1U << 1)
#define FLOW_OFFLOAD_PATH_VLAN (1U << 2)
#define FLOW_OFFLOAD_PATH_PPPOE (1U << 3)
#define FLOW_OFFLOAD_PATH_BRIDGE (1U << 4)
#define MTK_DSA_USER_PORT_MAX 32
#define MTK_GMAC3_ID 2
#define MTK_PPE_PKT_TYPE_IPV4_HNAPT 0
#define MTK_PPE_PKT_TYPE_IPV6_ROUTE_5T 5
enum { DEV_PATH_MTK_WDMA };
enum dsa_tag_protocol { DSA_TAG_PROTO_NONE, DSA_TAG_PROTO_MTK };
enum {
	PSE_ADMA_PORT, PSE_GDM1_PORT, PSE_GDM2_PORT, PSE_GDM3_PORT,
	PSE_WDMA0_PORT, PSE_WDMA1_PORT, PSE_WDMA2_PORT, PSE_PPE0_PORT,
};
struct mtk_mac { int dsa_queue_base, dsa_port_rank[32]; };
struct net_device_ops { bool ndo_flow_offload, ndo_fill_forward_path; };
struct net_device {
	const struct net_device_ops *netdev_ops;
	int rx_index, tx_index;
	u16 wcid;
	struct mtk_mac mac;
};
struct net_device_path {
	int type;
	struct { u8 wdma_idx, queue, bss, amsdu; u16 wcid; } mtk_wdma;
};
struct net_device_path_stack { int num_paths; struct net_device_path path[1]; };
struct net_device_path_ctx { struct net_device *dev; u8 daddr[6]; bool mtk_wdma_rx; };
struct mtk_wdma_info { int wdma_idx; u8 queue, bss, amsdu; u16 wcid; };
struct mtk_soc_data { int ppe_num, version; };
struct mtk_eth { const struct mtk_soc_data *soc; struct net_device *netdev[3]; };
struct mtk_foe_entry { int port, wdma_index, wcid, bss, queue, amsdu; };
struct mtk_ppe_wed_flow { int tx_index, rx_index; u16 tx_wcid, rx_wcid; };
typedef struct {
	int l4proto;
	struct { u32 s_addr; } src_v4, dst_v4;
	struct { u32 s6_addr32[4]; } src_v6, dst_v6;
	u16 src_port, dst_port;
} flow_offload_tuple_t;
typedef struct {
	struct net_device *dev;
	u32 flags;
	u8 eth_src[6], eth_dest[6];
	u16 vlan_id, pppoe_sid;
} flow_offload_hw_path_t;
static int rx_queries, tx_queries, enabled_wed;
static void ether_addr_copy(u8 *dest, const u8 *src) { memcpy(dest, src, 6); }
static void *netdev_priv(struct net_device *dev) { return &dev->mac; }
static bool mtk_is_netsys_v2_or_greater(struct mtk_eth *eth)
{
	return eth->soc->version >= 2;
}
static int dev_fill_forward_path(struct net_device_path_ctx *ctx,
				 struct net_device_path_stack *stack)
{
	struct net_device *dev = ctx->dev;
	int index;

	if (ctx->mtk_wdma_rx) {
		rx_queries++;
		index = dev->rx_index;
	} else {
		tx_queries++;
		index = dev->tx_index;
	}
	if (index < 0)
		return -ENODEV;
	stack->num_paths = 1;
	stack->path[0] = (struct net_device_path) {
		.type = DEV_PATH_MTK_WDMA,
		.mtk_wdma = { .wdma_idx = index, .wcid = dev->wcid,
			      .bss = 9, .queue = 0, .amsdu = 1 },
	};
	ctx->dev = NULL;
	return 0;
}
static int mtk_offload_get_dsa_info(flow_offload_hw_path_t *path, u16 *port,
				    u16 *vid, enum dsa_tag_protocol *proto)
{
	*port = 0xffff;
	*vid = 0;
	*proto = DSA_TAG_PROTO_NONE;
	return 0;
}
static bool mtk_offload_dsa_uses_vlan(enum dsa_tag_protocol proto) { return false; }
static int mtk_wed_flow_add_nolock(int index) { enabled_wed = index; return 0; }
static void mtk_foe_entry_prepare(struct mtk_eth *eth, struct mtk_foe_entry *entry,
				  int type, int proto, int port, u8 *src, u8 *dest)
{
	*entry = (struct mtk_foe_entry) { .port = port, .wdma_index = -1 };
}
static void mtk_foe_entry_set_wdma(struct mtk_eth *eth, struct mtk_foe_entry *entry,
				   int index, int queue, int bss, int wcid, bool amsdu)
{
	entry->wdma_index = index;
	entry->queue = queue;
	entry->bss = bss;
	entry->wcid = wcid;
	entry->amsdu = amsdu;
}
static void mtk_foe_entry_set_pse_port(struct mtk_eth *eth,
				       struct mtk_foe_entry *entry, int port)
{
	entry->port = port;
}
static void mtk_foe_entry_set_queue(struct mtk_eth *eth,
				    struct mtk_foe_entry *entry, int queue)
{
	entry->queue = queue;
}
#define mtk_foe_entry_set_ipv4_tuple(...) ((void)0)
#define mtk_foe_entry_set_ipv6_tuple(...) ((void)0)
#define mtk_foe_entry_clear_queue(...) ((void)0)
#define mtk_foe_entry_set_vlan(...) ((void)0)
#define mtk_foe_entry_set_dsa(...) ((void)0)
#define mtk_foe_entry_set_pppoe(...) ((void)0)
#define mtk_foe_entry_clear_ttl(...) ((void)0)

/* PRODUCTION */

static int checks;
#define CHECK(value) do { assert(value); checks++; } while (0)
int main(void)
{
	const struct net_device_ops gmac_ops = { .ndo_flow_offload = true };
	const struct net_device_ops wlan_ops = { .ndo_fill_forward_path = true };
	const struct net_device_ops ext_ops = {};
	struct net_device lan = { .netdev_ops = &gmac_ops };
	struct net_device switch_lan = { .netdev_ops = &gmac_ops };
	struct net_device wan = { .netdev_ops = &gmac_ops };
	struct net_device wlan = { .netdev_ops = &wlan_ops, .wcid = 73 };
	struct net_device other = { .netdev_ops = &wlan_ops, .wcid = 81,
		.rx_index = 2, .tx_index = 2 };
	struct net_device ext = { .netdev_ops = &ext_ops };
	struct mtk_soc_data soc = { .ppe_num = 3, .version = 3 };
	struct mtk_eth eth = { .soc = &soc, .netdev = { &lan, &switch_lan, &wan } };
	flow_offload_tuple_t tuple = { .l4proto = 6 };
	int (*prepare[])(struct mtk_eth *, struct mtk_foe_entry *,
		flow_offload_tuple_t *, flow_offload_tuple_t *,
		flow_offload_hw_path_t *, flow_offload_hw_path_t *, int *,
		struct mtk_ppe_wed_flow *) = { mtk_offload_prepare_v4, mtk_offload_prepare_v6 };
	int ports[] = { PSE_WDMA0_PORT, PSE_WDMA1_PORT, PSE_WDMA2_PORT };
	int family, rx, tx;

	for (family = 0; family < 2; family++) {
		/* Include different physical WED assignments; RX is not always WED0. */
		for (rx = 0; rx < 3; rx++) for (tx = 0; tx < 3; tx++) {
			flow_offload_hw_path_t src = { .dev = &wlan }, dest = { .dev = &wan };
			struct mtk_ppe_wed_flow wed = { .tx_index = -1, .rx_index = -1 };
			struct mtk_foe_entry entry;
			int ppe;

			wlan.rx_index = rx;
			wlan.tx_index = tx;
			rx_queries = tx_queries = 0;
			enabled_wed = -1;
			CHECK(!prepare[family](&eth, &entry, &tuple, &tuple, &src, &dest, &ppe, &wed));
			CHECK(ppe == rx && wed.rx_index == rx && wed.rx_wcid == wlan.wcid);
			CHECK(wed.tx_index == -1 && entry.port == PSE_GDM3_PORT);
			CHECK(rx_queries == 1 && !tx_queries && enabled_wed == -1);
			wed = (struct mtk_ppe_wed_flow) { .tx_index = -1, .rx_index = -1 };
			rx_queries = tx_queries = 0;
			CHECK(!prepare[family](&eth, &entry, &tuple, &tuple, &dest, &src, &ppe, &wed));
			CHECK(ppe == 2 && wed.tx_index == tx && wed.tx_wcid == wlan.wcid);
			CHECK(wed.rx_index == -1 && entry.port == ports[tx] && entry.wdma_index == tx);
			CHECK(entry.wcid == wlan.wcid && entry.bss == 9 && entry.amsdu == 1);
			CHECK(!rx_queries && tx_queries == 1 && enabled_wed == tx);
		}
		{
			flow_offload_hw_path_t src = { .dev = &wlan }, dest = { .dev = &other };
			struct mtk_ppe_wed_flow wed = { .tx_index = -1, .rx_index = -1 };
			struct mtk_foe_entry entry;
			int ppe;

			wlan.rx_index = 0;
			wlan.tx_index = 1;
			CHECK(!prepare[family](&eth, &entry, &tuple, &tuple, &src, &dest, &ppe, &wed));
			CHECK(ppe == 0 && wed.rx_index == 0 && wed.tx_index == 2);
			CHECK(wed.rx_wcid == wlan.wcid && wed.tx_wcid == other.wcid);
			dest.dev = &ext;
			CHECK(prepare[family](&eth, &entry, &tuple, &tuple, &src, &dest, &ppe, &wed) == -EOPNOTSUPP);
			dest.dev = &wan;
			wlan.rx_index = 3;
			CHECK(prepare[family](&eth, &entry, &tuple, &tuple, &src, &dest, &ppe, &wed) == -EOPNOTSUPP);
			src.flags = FLOW_OFFLOAD_PATH_WED_DIS;
			wed = (struct mtk_ppe_wed_flow) { .tx_index = -1, .rx_index = -1 };
			rx_queries = tx_queries = 0;
			CHECK(!prepare[family](&eth, &entry, &tuple, &tuple, &src, &dest, &ppe, &wed));
			CHECK(!rx_queries && !tx_queries && wed.rx_index == -1);
		}
	}
	printf("NATflow WED paths: %d checks passed\n", checks);
	return 0;
}
