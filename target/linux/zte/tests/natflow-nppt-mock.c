// SPDX-License-Identifier: GPL-2.0-only
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef uint16_t __be16;
typedef uint32_t __be32;

#define U16_MAX UINT16_MAX
#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_FCS_LEN 4
#define ETH_P_IP 0x0800
#define ETH_P_8021Q 0x8100
#define ETH_P_PPP_SES 0x8864
#define ETH_MIN_MTU 68
#define PPPOE_SES_HLEN 8
#define PPP_IP 0x21
#define VLAN_HLEN 4
#define VLAN_VID_MASK 4095
#define BIT(n) (1U << (n))
#define ZX279133_FAST_DATA_SIZE 32
#define be32_to_cpu(value) ntohl(value)
#define put_unaligned(value, ptr) do { \
	__typeof__(*(ptr)) tmp = (value); \
	memcpy(ptr, &tmp, sizeof(tmp)); \
} while (0)
#define ETH_DATA_LEN 1500
#define ZX279133_FAST_STAT_DEPTH 4
#define HZ 100
#define READ_ONCE(x) (x)
#define EXPORT_SYMBOL_GPL(x)
#define kzalloc_obj(obj, flags) calloc(1, sizeof(obj))
#define kfree free
#define DEFINE_SPINLOCK(x) int x __attribute__((unused))
#define DEFINE_MUTEX(x) int x __attribute__((unused))
#define spin_lock_bh(x) ((void)0)
#define spin_unlock_bh(x) ((void)0)
#define mutex_lock(x) ((void)0)
#define mutex_unlock(x) ((void)0)
#define local_bh_disable() ((void)0)
#define local_bh_enable() ((void)0)
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define queue_delayed_work(queue, work, delay) ((void)0)
#define DECLARE_DELAYED_WORK(name, fn) int name __attribute__((unused))
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)

static unsigned long jiffies = HZ;
static unsigned int warnings;

#define dev_warn_ratelimited(dev, ...) (warnings++)
#define LIST_HEAD(name) struct list_head name = { &name, &name }
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

struct list_head {
	struct list_head *next, *prev;
};

static void list_add_tail(struct list_head *entry, struct list_head *head)
{
	entry->next = head;
	entry->prev = head->prev;
	entry->prev->next = entry;
	head->prev = entry;
}

static void list_del(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
}

static void list_splice_tail_init(struct list_head *source, struct list_head *dest)
{
	while (source->next != source) {
		struct list_head *entry = source->next;

		list_del(entry);
		list_add_tail(entry, dest);
	}
}

#define list_for_each_entry_safe(it, next_it, head, member) \
	for (struct list_head *node = (head)->next, *next_node; \
	     node != (head) && (next_node = node->next, \
	     it = container_of(node, __typeof__(*it), member), \
	     next_it = next_node == (head) ? NULL : container_of(next_node, __typeof__(*it), member), \
	     (void)next_it, true); node = next_node)

struct work_struct;
struct kref { int refs; };
struct zte_nppt { bool locked, lan_vlan, wan_vlan; };
struct net_device_ops;
struct net_device {
	bool wan, vlan, stopped;
	int refs, mtu, type;
	const struct net_device_ops *netdev_ops;
	struct net_device *macvlan_lower;
	u8 dev_addr[ETH_ALEN];
};
struct ethhdr { u8 h_dest[6], h_source[6]; __be16 h_proto; };

#define netif_running(dev) (!(dev)->stopped)
#define netif_carrier_ok(dev) (true)
#define netif_is_macvlan(dev) ((dev)->macvlan_lower != NULL)
#define get_macvlan_real_dev(dev) ((dev)->macvlan_lower)
#define net_eq(a, b) (true)
#define is_vlan_dev(dev) ((dev)->vlan)
#define is_valid_ether_addr(address) ((address)[0] == 2)
#define dev_hold(dev) (++(dev)->refs)
#define dev_put(dev) (--(dev)->refs)
#define kref_get(ref) (++(ref)->refs)
#define kref_put(ref, release) assert(--(ref)->refs >= 1)

enum zx279133_flow_port { ZX279133_FLOW_PORT_LAN, ZX279133_FLOW_PORT_WAN };

struct zx279133_flow_tuple {
	__be32 src_addr, dst_addr;
	__be16 src_port, dst_port;
};

/* NPPT VLAN POLICY */

struct zx279133_flow_data {
	struct ethhdr eth;
	struct zx279133_flow_tuple tuple;
	struct zx279133_vlan_plan vlan;
};

struct zx279133_flow_spec {
	enum zx279133_flow_port ingress, egress;
	u16 gemport, pppoe_sid, mtu;
	u8 ip_proto;
	struct zx279133_flow_tuple xlate;
	struct zx279133_flow_data data;
};

struct zx279133_flow_counters { u64 bytes, packets; };
struct zx279133_flow_entry {
	bool retired, pppoe, wan_vlan;
	struct zx279133_flow_counters stats;
};

/* NPPT ENCODING */
struct zx279133_flow_offload {
	struct kref ref;
	struct zte_nppt *np;
	struct zx279133_flow_entry *flows[64];
	bool detached;
};

static struct zte_nppt np = { .lan_vlan = true, .wan_vlan = true };
static struct zx279133_flow_offload offload = { .ref = { 1 }, .np = &np };
static unsigned int installed, removed;
static bool fail_reply, fail_delete, fail_write;
static struct zx279133_flow_spec programmed[2];
static u8 actions[2][ZX279133_FAST_DATA_SIZE];

static int zte_nppt_begin(struct zte_nppt *hw)
{
	assert(!hw->locked);
	hw->locked = true;
	return 0;
}

static void zte_nppt_end(struct zte_nppt *hw)
{
	assert(hw->locked);
	hw->locked = false;
}

static int zte_nppt_flow_ready(struct zte_nppt *hw)
{
	assert(hw->locked);
	return 0;
}

static bool zte_nppt_is_wan(struct zte_nppt *hw, struct net_device *dev)
{
	return dev->wan;
}

static int zte_nppt_dev_port(struct zte_nppt *hw, struct net_device *dev, u16 *port)
{
	*port = dev->wan ? 1 : 205;
	return dev->wan ? ZX279133_FLOW_PORT_WAN : ZX279133_FLOW_PORT_LAN;
}

static bool zx279133_vlan_enabled(struct zte_nppt *hw)
{
	return hw->lan_vlan;
}

static bool zx279133_wan_vlan_enabled(struct zte_nppt *hw)
{
	return hw->wan_vlan;
}

static int zx279133_flow_install(struct zx279133_flow_offload *o,
				 const struct zx279133_flow_spec *spec, unsigned long *cookie)
{
	bool wan = spec->ingress == ZX279133_FLOW_PORT_WAN;
	unsigned long id;

	assert(o->np->locked && spec->gemport);
	assert(spec->ingress != spec->egress && spec->ip_proto == IPPROTO_TCP);
	assert(spec->data.tuple.src_port == htons(wan ? 443 : 1234));
	assert(spec->xlate.dst_port == htons(wan ? 1234 : 443));
	assert(spec->xlate.src_port == htons(wan ? 443 : 5678));
	if (wan && fail_reply)
		return -ENOSPC;
	for (id = 1; id < 64 && o->flows[id]; id++)
		;
	assert(id < 64 && !*cookie);
	*cookie = id;
	o->flows[id] = calloc(1, sizeof(*o->flows[id]));
	programmed[wan] = *spec;
	o->flows[id]->pppoe = spec->pppoe_sid != 0;
	o->flows[id]->wan_vlan = wan ? spec->data.vlan.tagged : spec->data.vlan.tci != 0;
	zx279133_fast_response_build(actions[wan], &spec->data, &spec->xlate,
				     spec->ingress, spec->gemport, 1, id,
				     spec->pppoe_sid != 0, 1);
	assert(actions[wan][29] & BIT(4));
	installed++;
	return wan && fail_write ? -EIO : 0;
}

static int zx279133_flow_stats(struct zx279133_flow_offload *o, unsigned long cookie,
			       struct zx279133_flow_counters *stats)
{
	struct zx279133_flow_entry *entry = o->flows[cookie];

	assert(o->np->locked);
	if (!entry)
		return -ENOENT;
	*stats = entry->stats;
	entry->stats = (struct zx279133_flow_counters) {};
	return 0;
}

static int zx279133_flow_destroy(struct zx279133_flow_offload *o, unsigned long cookie,
				 struct zx279133_flow_counters *stats)
{
	int ret;

	if (fail_delete && o->flows[cookie])
		return -EIO;
	ret = zx279133_flow_stats(o, cookie, stats);
	if (!ret) {
		free(o->flows[cookie]);
		o->flows[cookie] = NULL;
		removed++;
	}
	return ret;
}

#define xa_load(array, i) (*(array))[i]
#define zx279133_flow_entry_remove(o, entry, cookie, stats, retire) (-ENOENT)

/* NPPT API */
/* NPPT WORKER */

static const struct zte_nppt_natflow_api *provider;
static unsigned int refs;

#define symbol_get(name) (provider ? (refs++, provider) : NULL)
#define symbol_put(name) do { assert(refs > 0); refs--; } while (0)

/* NATFLOW BINDING */

static void test_optional_binding(void)
{
	struct zte_nppt_natflow_api incompatible = zte_nppt_natflow_api;

	natflow_async_backend_bind();
	assert(!natflow_zte_api && !refs);
	natflow_async_backend_unbind();

	provider = &incompatible;
	incompatible.version++;
	natflow_async_backend_bind();
	assert(!natflow_zte_api && !refs);
	incompatible.version--;
	incompatible.flush = NULL;
	natflow_async_backend_bind();
	assert(!natflow_zte_api && !refs);

	provider = &zte_nppt_natflow_api;
	natflow_async_backend_bind();
	assert(natflow_zte_api == provider && refs == 1);
	natflow_async_backend_unbind();
	assert(!natflow_zte_api && !refs);
}

struct client {
	unsigned int ready, reports, releases;
	u64 packets[2], bytes[2];
	bool invalid;
};

static int notify(void *context, enum zte_nppt_natflow_event event,
		  const struct zte_nppt_natflow_stats *stats)
{
	struct client *user = context;
	int d;

	assert(!np.locked);
	if (event == ZTE_NPPT_NATFLOW_CHECK)
		return user->invalid ? -ESTALE : 0;
	if (event == ZTE_NPPT_NATFLOW_READY) {
		user->ready++;
		return 0;
	}
	for (d = 0; d < 2; d++) {
		user->packets[d] += stats->packets[d];
		user->bytes[d] += stats->bytes[d];
	}
	if (event == ZTE_NPPT_NATFLOW_RELEASE)
		user->releases++;
	else
		user->reports++;
	return 0;
}

#define ARPHRD_ETHER 1
#define ARPHRD_PPP 512
#define FF_ETH_P_8021Q 0
#define DEV_PATH_PPPOE 1

static struct net_device lan = { .refs = 1, .mtu = ETH_DATA_LEN, .type = ARPHRD_ETHER };
static struct net_device wan = { .wan = true, .refs = 1, .mtu = ETH_DATA_LEN, .type = ARPHRD_ETHER };

struct net_device_path_ctx {
	const struct net_device *dev;
};

struct net_device_path {
	int type;
	struct {
		u16 id;
		__be16 proto;
		u8 h_dest[6];
	} encap;
};

struct net_device_ops {
	int (*ndo_fill_forward_path)(struct net_device_path_ctx *, struct net_device_path *);
};

struct pppoe_hdr {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	u8 type:4, ver:4;
#else
	u8 ver:4, type:4;
#endif
	u8 code;
	__be16 sid, length;
} __attribute__((packed));

typedef struct {
	struct net_device *outdev;
	unsigned char l2_head[ETH_HLEN + PPPOE_SES_HLEN];
	u16 l2_head_len, mtu, vlan_tci;
	u8 vlan_proto;
	bool vlan_present;
} natflow_route_t;

typedef struct {
	natflow_route_t rroute[2];
	int ppp[2];
} natflow_t;

struct natflow_async_backend_ticket {
	struct zte_nppt_natflow_path path[2];
	struct net_device *dev[2];
	struct ethhdr eth[2];
	int ppp_ifindex[2];
};

static bool ppp_connected = true;
static u16 live_sid = 101;
static struct net_device *ppp_lower = &wan;
static const u8 peer_mac[] = { 2, 1, 2, 3, 4, 5 };
static int ppp_path(struct net_device_path_ctx *ctx, struct net_device_path *node)
{
	if (!ppp_connected)
		return -ENODEV;
	ctx->dev = ppp_lower;
	node->type = DEV_PATH_PPPOE;
	node->encap.id = live_sid;
	node->encap.proto = htons(ETH_P_PPP_SES);
	memcpy(node->encap.h_dest, peer_mac, sizeof(peer_mac));
	return 0;
}

static const struct net_device_ops ppp_ops = { .ndo_fill_forward_path = ppp_path };
static struct net_device ppp = { .type = ARPHRD_PPP, .mtu = 1492, .netdev_ops = &ppp_ops };

static struct net_device *dev_get_by_index_rcu(void *net, int i)
{
	return i == 8 ? &ppp : NULL;
}

static int init_net;
#define ether_addr_equal(a, b) (!memcmp(a, b, 6))

/* NATFLOW PATH */

static struct zte_nppt_natflow_request request(struct client *user, bool reverse)
{
	struct zte_nppt_natflow_request req = { .context = user, .notify = notify };
	unsigned int up = reverse ? 1 : 0;

	req.dev[up] = &lan;
	req.dev[!up] = &wan;
	req.tuple[up] = (struct zte_nppt_natflow_tuple) {
		.sport = htons(1234), .dport = htons(443), .proto = IPPROTO_TCP,
	};
	req.tuple[!up] = (struct zte_nppt_natflow_tuple) {
		.sport = htons(443), .dport = htons(5678), .proto = IPPROTO_TCP,
	};
	req.eth[0] = req.eth[1] = (struct ethhdr) {
		.h_source = { 2 }, .h_dest = { 2 }, .h_proto = htons(ETH_P_IP),
	};
	req.path[0].mtu = req.path[1].mtu = ETH_DATA_LEN;
	return req;
}

static int submit(struct client *user, bool reverse)
{
	struct zte_nppt_natflow_request req = request(user, reverse);

	return zte_nppt_natflow_submit(&req);
}

static void sample(int direction, u64 bytes, u64 packets)
{
	struct zx279133_natflow_pair *pair;
	struct zx279133_flow_entry *entry;

	assert(zx279133_natflow_pairs.next != &zx279133_natflow_pairs);
	pair = container_of(zx279133_natflow_pairs.next, __typeof__(*pair), list);
	entry = offload.flows[pair->cookie[direction]];
	entry->stats = (struct zx279133_flow_counters) { zx279133_flow_ip_bytes(entry, packets, bytes), packets };
}

static void test_macvlan_pppoe(natflow_t *nf)
{
	struct net_device macvlan = {
		.macvlan_lower = &wan,
		.dev_addr = { 2, 3, 4, 5, 6, 7 },
	};
	struct net_device nested = { .macvlan_lower = &macvlan };
	struct natflow_async_backend_ticket ticket = { .dev = { &wan } };
	struct ethhdr eth;

	memcpy(&eth, nf->rroute[0].l2_head, ETH_HLEN);
	memcpy(eth.h_source, macvlan.dev_addr, ETH_ALEN);
	memcpy(nf->rroute[0].l2_head, &eth, ETH_HLEN);
	ppp_lower = &macvlan;
	assert(natflow_zte_path(nf, 0, &ticket.eth[0], &ticket.path[0], &ticket.ppp_ifindex[0]));
	assert(nf->rroute[0].outdev == &wan);
	assert(ether_addr_equal(ticket.eth[0].h_source, macvlan.dev_addr));
	assert(natflow_async_backend_path_valid(&ticket, nf, 0));
	live_sid++;
	assert(!natflow_async_backend_path_valid(&ticket, nf, 0));
	live_sid--;
	macvlan.dev_addr[ETH_ALEN - 1] ^= 1;
	assert(!natflow_async_backend_path_valid(&ticket, nf, 0));
	macvlan.dev_addr[ETH_ALEN - 1] ^= 1;
	macvlan.stopped = true;
	assert(!natflow_async_backend_path_valid(&ticket, nf, 0));
	macvlan.stopped = false;
	macvlan.macvlan_lower = &lan;
	assert(!natflow_async_backend_path_valid(&ticket, nf, 0));
	macvlan.macvlan_lower = &wan;
	memcpy(nested.dev_addr, macvlan.dev_addr, ETH_ALEN);
	ppp_lower = &nested;
	assert(!natflow_async_backend_path_valid(&ticket, nf, 0));
	ppp_lower = &wan;
}

static void test_path_learning(void)
{
	natflow_t nf = { .rroute = { { .outdev = &wan, .mtu = 1500 } } };
	natflow_route_t *route = &nf.rroute[0];
	struct natflow_async_backend_ticket ticket = { .dev = { &wan } };
	struct ethhdr eth = { .h_proto = htons(ETH_P_IP) };
	struct pppoe_hdr ph = { .ver = 1, .type = 1, .sid = htons(101) };
	__be16 proto = htons(PPP_IP);

	memcpy(eth.h_dest, peer_mac, sizeof(peer_mac));
	route->vlan_present = true;
	route->vlan_tci = 100;
	route->l2_head_len = ETH_HLEN;
	memcpy(route->l2_head, &eth, ETH_HLEN);

	assert(natflow_zte_path(&nf, 0, &ticket.eth[0], &ticket.path[0], &ticket.ppp_ifindex[0]));
	assert(ticket.path[0].vlan_id == 100 && !ticket.path[0].pppoe_sid);
	assert(natflow_async_backend_path_valid(&ticket, &nf, 0));
	route->vlan_tci = 200;
	assert(!natflow_async_backend_path_valid(&ticket, &nf, 0));
	route->vlan_proto = 1;
	assert(!natflow_zte_path(&nf, 0, &ticket.eth[0], &ticket.path[0], &ticket.ppp_ifindex[0]));

	nf.ppp[0] = 8;
	route->vlan_proto = FF_ETH_P_8021Q;
	route->mtu = 1492;
	route->l2_head_len = ETH_HLEN + PPPOE_SES_HLEN;
	eth.h_proto = htons(ETH_P_PPP_SES);
	memcpy(route->l2_head, &eth, ETH_HLEN);
	memcpy(route->l2_head + ETH_HLEN, &ph, sizeof(ph));
	memcpy(route->l2_head + ETH_HLEN + sizeof(ph), &proto, sizeof(proto));
	assert(!natflow_zte_path(&nf, 0, &ticket.eth[0], &ticket.path[0], &ticket.ppp_ifindex[0]));

	route->vlan_present = false;
	assert(natflow_zte_path(&nf, 0, &ticket.eth[0], &ticket.path[0], &ticket.ppp_ifindex[0]));
	assert(ticket.path[0].pppoe_sid == 101 && ticket.ppp_ifindex[0] == 8);
	assert(ticket.eth[0].h_proto == htons(ETH_P_IP));
	assert(natflow_async_backend_path_valid(&ticket, &nf, 0));
	live_sid++;
	assert(!natflow_async_backend_path_valid(&ticket, &nf, 0));
	live_sid--;
	ppp_lower = &lan;
	assert(!natflow_async_backend_path_valid(&ticket, &nf, 0));
	ppp_lower = &wan;
	ppp_connected = false;
	assert(!natflow_async_backend_path_valid(&ticket, &nf, 0));
	ppp_connected = true;
	route->mtu++;
	assert(!natflow_async_backend_path_valid(&ticket, &nf, 0));
	route->mtu--;
	if (TEST_NATFLOW_MACVLAN_PPPOE)
		test_macvlan_pppoe(&nf);
}

static void test_encap(void)
{
	struct client pppoe = {}, wan_tag = {}, lan_tag = {}, both_tags = {}, invalid = {};
	struct zte_nppt_natflow_request req = request(&pppoe, true);
	unsigned int before = installed;

	req.path[1].mtu = 1492;
	req.path[1].pppoe_sid = 101;
	assert(!zte_nppt_natflow_submit(&req));
	zx279133_natflow_poll(NULL);
	assert(pppoe.ready == 1 && installed == before + 2);
	assert(programmed[0].pppoe_sid == 101 && programmed[0].mtu == 1492);
	assert(programmed[1].pppoe_sid == 101 && programmed[1].mtu == 1500);
	assert(actions[0][16] == 4 && actions[1][16] == 4);
	assert((actions[0][29] & BIT(1)) && !(actions[1][29] & BIT(1)));
	sample(0, 126, 1);
	sample(1, 152, 2);
	assert(!zte_nppt_natflow_flush(false));
	assert(pppoe.releases == 1 && pppoe.bytes[0] == 100 && pppoe.bytes[1] == 100);

	req.context = &invalid;
	req.path[0].vlan_id = 200;
	assert(!zte_nppt_natflow_submit(&req));
	zx279133_natflow_poll(NULL);
	assert(!invalid.ready && invalid.releases == 1 && installed == before + 2);

	req = request(&invalid, false);
	req.path[0].pppoe_sid = 101;
	assert(!zte_nppt_natflow_submit(&req));
	zx279133_natflow_poll(NULL);
	assert(!invalid.ready && invalid.releases == 2 && installed == before + 2);
	req.path[0].pppoe_sid = 0;
	req.path[1].pppoe_sid = 101;
	req.path[1].mtu = 1492;
	assert(!zte_nppt_natflow_submit(&req));
	zx279133_natflow_poll(NULL);
	assert(!invalid.ready && invalid.releases == 3 && installed == before + 2);

	req = request(&wan_tag, false);
	req.path[0].vlan_id = 200;
	assert(!zte_nppt_natflow_submit(&req));
	zx279133_natflow_poll(NULL);
	assert(wan_tag.ready == 1 && installed == before + 4);
	assert(programmed[0].data.vlan.edit == ZX_VLAN_PUSH && programmed[0].data.vlan.tci == 200);
	assert(programmed[1].data.vlan.edit == ZX_VLAN_POP && programmed[1].data.vlan.vid == 200);
	sample(0, 122, 1);
	sample(1, 144, 2);
	assert(!zte_nppt_natflow_flush(false));
	assert(wan_tag.bytes[0] == 100 && wan_tag.bytes[1] == 100);

	np.wan_vlan = false;
	req.context = &invalid;
	assert(!zte_nppt_natflow_submit(&req));
	zx279133_natflow_poll(NULL);
	np.wan_vlan = true;
	assert(!invalid.ready && invalid.releases == 4 && installed == before + 4);

	req = request(&lan_tag, false);
	req.path[1].vlan_id = 100;
	assert(!zte_nppt_natflow_submit(&req));
	zx279133_natflow_poll(NULL);
	assert(lan_tag.ready == 1 && installed == before + 6);
	assert(programmed[0].data.vlan.edit == ZX_VLAN_POP && programmed[0].data.vlan.vid == 100);
	assert(programmed[1].data.vlan.edit == ZX_VLAN_PUSH && programmed[1].data.vlan.tci == 100);
	sample(0, 118, 1);
	sample(1, 136, 2);
	assert(!zte_nppt_natflow_flush(false));
	assert(lan_tag.bytes[0] == 100 && lan_tag.bytes[1] == 100);

	req.context = &both_tags;
	req.path[0].vlan_id = 200;
	assert(!zte_nppt_natflow_submit(&req));
	zx279133_natflow_poll(NULL);
	assert(both_tags.ready == 1 && installed == before + 8);
	assert(programmed[0].data.vlan.edit == ZX_VLAN_REPLACE);
	assert(programmed[0].data.vlan.vid == 100 && programmed[0].data.vlan.tci == 200);
	assert(programmed[1].data.vlan.edit == ZX_VLAN_REPLACE);
	assert(programmed[1].data.vlan.vid == 200 && programmed[1].data.vlan.tci == 100);
	assert(actions[0][30] >> 6 == ZX_VLAN_REPLACE && actions[0][24] == 200);
	assert(actions[1][30] >> 6 == ZX_VLAN_REPLACE && actions[1][24] == 100);
	assert(!zte_nppt_natflow_flush(false));
}

int main(void)
{
	struct client active = {}, failed = {}, pending = {}, ignored = {};

	zx279133_natflow_attach(&offload);
	zx279133_natflow_poll(NULL);
	assert(!installed && !zx279133_natflow_count);

	/* Reversed original direction still installs the upstream first. */
	assert(!submit(&active, true));
	zx279133_natflow_poll(NULL);
	assert(active.ready == 1 && installed == 2);
	sample(0, 236, 2);
	sample(1, 354, 3);
	zx279133_natflow_poll(NULL);
	assert(active.reports == 1 && active.packets[0] == 2 && active.bytes[1] == 300);
	sample(1, 272, 4);
	active.invalid = true;
	zx279133_natflow_poll(NULL);
	assert(active.releases == 1 && active.packets[1] == 7 && active.bytes[1] == 500);
	assert(removed == 2 && offload.ref.refs == 1);

	/* No READY if the reply has no counter. Undo the upstream write. */
	fail_reply = true;
	assert(!submit(&failed, false));
	zx279133_natflow_poll(NULL);
	assert(failed.ready == 0 && failed.releases == 1 && installed == 3 && removed == 3);
	fail_reply = false;

	/* A failed hardware write can have published a cookie in either direction. */
	failed = (struct client) {};
	fail_write = true;
	assert(!submit(&failed, true));
	zx279133_natflow_poll(NULL);
	assert(!failed.ready && failed.releases == 1 && installed == removed);
	fail_write = false;

	/* Flushing queued work transfers no rules and releases each context once. */
	assert(!submit(&pending, false));
	assert(!submit(&pending, false));
	assert(submit(&ignored, false) == -ENOSPC);
	assert(!zte_nppt_natflow_flush(false));
	zx279133_natflow_poll(NULL);
	assert(pending.releases == 2 && !pending.ready && ignored.releases == 0);
	assert(offload.ref.refs == 1 && lan.refs == 1 && wan.refs == 1);

	/* Tagged and nonstandard-MTU paths cannot enter NPPT. */
	lan.vlan = true;
	assert(!submit(&ignored, false));
	zx279133_natflow_poll(NULL);
	lan.vlan = false;
	wan.mtu--;
	assert(!submit(&ignored, false));
	zx279133_natflow_poll(NULL);
	wan.mtu++;
	assert(ignored.releases == 2 && !ignored.ready);

	/* Flushing an invalidation error detaches the callback but keeps retrying. */
	active = (struct client) {};
	assert(!submit(&active, false));
	zx279133_natflow_poll(NULL);
	fail_delete = true;
	assert(zte_nppt_natflow_flush(false) == -EIO);
	assert(!active.releases && warnings == 1 && offload.ref.refs == 2);
	assert(zte_nppt_natflow_flush(true) == -EIO);
	assert(active.releases == 1 && warnings == 2 && offload.ref.refs == 2);
	fail_delete = false;
	zx279133_natflow_poll(NULL);
	assert(active.releases == 1 && offload.ref.refs == 1);

	active = (struct client) {};
	assert(!submit(&active, false));
	zx279133_natflow_poll(NULL);
	jiffies += ZX279133_NATFLOW_IDLE;
	zx279133_natflow_poll(NULL);
	assert(active.releases == 1 && !active.reports);

	test_optional_binding();
	test_path_learning();
	test_encap();

	zx279133_natflow_detach(&offload);
	assert(submit(&ignored, false) == -EAGAIN);
	assert(lan.refs == 1 && wan.refs == 1 && installed == removed);
	puts("NPPT optional binding, lifecycle, rollback, PPPoE/VLAN rules and directional counters passed");
	return 0;
}
