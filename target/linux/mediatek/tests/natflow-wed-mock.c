// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
typedef uint16_t u16;
typedef int8_t s8;
#define BIT(n) (1U << (n))
#define MTK_PPE_ENTRIES 8
#define MTK_FOE_IB1_STATE (3U << 28)
#define MTK_FOE_STATE_BIND 2
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctz(mask))
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, next) ((value) = (next))
#define dma_rmb() ((void)0)
#define dma_wmb() ((void)0)
enum { CFG80211_FLOW_F_ETH_SA, CFG80211_FLOW_F_ETH_DA };
struct cfg80211_flow_info { u32 present; int id; };
struct mtk_flow_match {
	bool (*fn)(const struct cfg80211_flow_info *, bool, void *);
	void *arg;
};
struct mtk_ppe_wed_flow { s8 tx_index, rx_index; u16 tx_wcid, rx_wcid; };
struct mtk_foe_entry { u32 ib1; int id; };
struct mtk_eth;
struct mtk_ppe {
	struct mtk_eth *eth;
	struct mtk_ppe_wed_flow wed_flows[MTK_PPE_ENTRIES];
	struct mtk_foe_entry entries[MTK_PPE_ENTRIES];
	int mib_lock, clears;
};
struct mtk_soc_data { int ppe_num; };
struct mtk_eth { const struct mtk_soc_data *soc; struct mtk_ppe *ppe[3]; u32 now; };
static void spin_lock_bh(int *lock) { assert(!*lock); *lock = 1; }
static void spin_unlock_bh(int *lock) { assert(*lock); *lock = 0; }
static u32 mtk_get_ib1_ts_mask(struct mtk_eth *eth) { return 255; }
static u32 mtk_r32(struct mtk_eth *eth, u32 reg) { return eth->now; }
static struct mtk_foe_entry *mtk_foe_get_entry(struct mtk_ppe *ppe, int hash)
{
	assert(hash >= 0 && hash < MTK_PPE_ENTRIES);
	return &ppe->entries[hash];
}
static void mtk_ppe_cache_clear(struct mtk_ppe *ppe)
{
	assert(ppe->mib_lock);
	ppe->clears++;
}
static void mtk_foe_entry_get_flow(struct mtk_eth *eth, struct mtk_foe_entry *entry,
				 bool rx, struct cfg80211_flow_info *info)
{
	info->id = entry->id;
	info->present = BIT(CFG80211_FLOW_F_ETH_SA) | BIT(CFG80211_FLOW_F_ETH_DA);
}

/* PRODUCTION */

static int checks;
#define CHECK(value) do { assert(value); checks++; } while (0)
struct query { int id; bool rx; };
static bool match(const struct cfg80211_flow_info *info, bool rx, void *arg)
{
	struct query *query = arg;

	CHECK(info->present == (rx ? 0 : 3));
	return info->id == query->id && rx == query->rx;
}
static void bind(struct mtk_ppe *ppe, int slot, int id, int stamp,
		 int tx, int rx, int wcid)
{
	ppe->entries[slot] = (struct mtk_foe_entry) {
		.ib1 = (MTK_FOE_STATE_BIND << 28) | stamp, .id = id,
	};
	ppe->wed_flows[slot] = (struct mtk_ppe_wed_flow) {
		.tx_index = tx, .rx_index = rx, .tx_wcid = wcid, .rx_wcid = wcid,
	};
}
int main(void)
{
	struct mtk_soc_data soc = { .ppe_num = 3 };
	struct mtk_eth eth = { .soc = &soc, .now = 5 };
	struct mtk_ppe first = { .eth = &eth }, second = { .eth = &eth };
	struct query query = { .id = 10 };
	struct mtk_flow_match selected = { .fn = match, .arg = &query };
	struct mtk_flow_match all = {};

	eth.ppe[0] = &first;
	eth.ppe[2] = &second;
	bind(&first, 0, 10, 250, 0, -1, 7); /* timestamp wrap: 11 seconds */
	bind(&first, 1, 20, 4, 1, -1, 7);   /* same peer on a different WED */
	bind(&first, 2, 30, 3, 0, -1, 8);   /* different station */
	bind(&first, 3, 40, 1, -1, 0, 7);   /* uplink belongs to the RX WED */
	bind(&second, 0, 10, 2, 0, -1, 7);  /* matching flow in another PPE */
	CHECK(mtk_flow_offload_idle_wcid(&eth, 0, 7, &selected) == 3);
	CHECK(mtk_flow_offload_idle_wcid(&eth, 1, 7, &all) == 1);
	CHECK(mtk_flow_offload_idle_wcid(&eth, 0, 8, &all) == 2);
	CHECK(mtk_flow_offload_idle_wcid(&eth, 0, 9, &all) == -ENOENT);
	CHECK(!first.clears && !second.clears);
	mtk_flow_offload_unbind_wcid(&eth, 0, 7, &selected);
	CHECK(!FIELD_GET(MTK_FOE_IB1_STATE, first.entries[0].ib1));
	CHECK(!FIELD_GET(MTK_FOE_IB1_STATE, second.entries[0].ib1));
	CHECK(FIELD_GET(MTK_FOE_IB1_STATE, first.entries[1].ib1) == MTK_FOE_STATE_BIND);
	CHECK(FIELD_GET(MTK_FOE_IB1_STATE, first.entries[2].ib1) == MTK_FOE_STATE_BIND);
	CHECK(FIELD_GET(MTK_FOE_IB1_STATE, first.entries[3].ib1) == MTK_FOE_STATE_BIND);
	CHECK(first.clears == 1 && second.clears == 1);
	query = (struct query) { .id = 40, .rx = true };
	CHECK(mtk_flow_offload_idle_wcid(&eth, 0, 7, &selected) == 4);
	mtk_flow_offload_unbind_wcid(&eth, 0, 7, &selected);
	CHECK(!FIELD_GET(MTK_FOE_IB1_STATE, first.entries[3].ib1));
	CHECK(mtk_flow_offload_idle_wcid(&eth, 0, 7, &all) == -ENOENT);
	CHECK(first.clears == 2 && second.clears == 1);
	/* Slot reuse replaces both directions of ownership. */
	bind(&first, 0, 50, 5, 2, -1, 11);
	CHECK(mtk_flow_offload_idle_wcid(&eth, 0, 7, &all) == -ENOENT);
	CHECK(mtk_flow_offload_idle_wcid(&eth, 2, 11, &all) == 0);
	mtk_flow_offload_unbind_wcid(&eth, 2, 11, &all);
	CHECK(first.clears == 3 && !first.mib_lock && !second.mib_lock);
	printf("NATflow WED: %d checks passed\n", checks);
	return 0;
}
