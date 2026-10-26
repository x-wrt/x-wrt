// SPDX-License-Identifier: GPL-2.0-or-later
/* Deterministic host scheduling and hardware mocks around production sources. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <arpa/inet.h>

typedef uint8_t u8;
typedef uint16_t u16, __be16;
typedef uint32_t u32, __be32;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
#define HZ 100
#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_P_IP 0x800
#define ARPHRD_ETHER 1
#define GFP_KERNEL 0
#define GFP_ATOMIC 0
#define FLOW_DISSECTOR_KEY_IPV4_ADDRS 1
#define PPE_FLOW_AGE_MAX 3
#define EXPORT_SYMBOL_GPL(name)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define container_of(p, t, f) ((t *)((char *)(p) - offsetof(t, f)))
#define kzalloc(n, flags) calloc(1, n)
#define kfree(p) free(p)
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
static unsigned long jiffies;

struct ethhdr { u8 h_dest[6], h_source[6]; __be16 h_proto; };
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD(name) struct list_head name = { &name, &name }
#define INIT_LIST_HEAD(h) (*(h) = (struct list_head){ (h), (h) })
static bool list_empty(struct list_head *h) { return h->next == h; }
static void list_add_tail(struct list_head *n, struct list_head *h)
{ n->next = h; n->prev = h->prev; h->prev->next = n; h->prev = n; }
static void list_del(struct list_head *n)
{ n->prev->next = n->next; n->next->prev = n->prev; }
static void list_move_tail(struct list_head *n, struct list_head *h)
{ list_del(n); list_add_tail(n, h); }
#define list_add_rcu(n, h) list_add_tail(n, h)
#define list_del_rcu(n) list_del(n)
#define list_entry(p, t, m) container_of(p, t, m)
#define list_first_entry_or_null(h, t, m) \
	(list_empty(h) ? NULL : list_entry((h)->next, t, m))
#define list_for_each_entry(p, h, m) \
	for (p = list_entry((h)->next, __typeof__(*p), m); &p->m != (h); \
	     p = list_entry(p->m.next, __typeof__(*p), m))
#define list_for_each_entry_rcu(p, h, m) list_for_each_entry(p, h, m)
#define list_for_each_entry_safe(p, tmp, h, m) \
	for (p = list_entry((h)->next, __typeof__(*p), m), \
	     tmp = list_entry(p->m.next, __typeof__(*p), m); &p->m != (h); \
	     p = tmp, tmp = list_entry(tmp->m.next, __typeof__(*tmp), m))
struct mutex { int held; };
#define DEFINE_MUTEX(name) struct mutex name
#define DEFINE_SPINLOCK(name) int name
#define spinlock_t int
#define spin_lock_init(lock) (*(lock) = 0)
#define spin_lock_bh(lock) assert(++*(lock) == 1)
#define spin_unlock_bh(lock) assert(--*(lock) == 0)
static void mutex_lock(struct mutex *m) { assert(++m->held == 1); }
static void mutex_unlock(struct mutex *m) { assert(--m->held == 0); }
#define lockdep_assert_held(m) assert((m)->held)
static int bh_depth;
#define local_bh_disable() (bh_depth++)
#define local_bh_enable() assert(--bh_depth >= 0)
#define rcu_read_lock() ((void)0)
#define rcu_read_unlock() ((void)0)
#define synchronize_rcu() ((void)0)

struct workqueue_struct;
struct work_struct {
	void (*fn)(struct work_struct *);
	struct workqueue_struct *queue;
	bool queued;
};
struct delayed_work { struct work_struct work; };
struct task { struct work_struct *work; unsigned long due; u64 order; };
struct workqueue_struct { struct task tasks[512]; int n; u64 next; };
#define WQ_MEM_RECLAIM 0
#define INIT_WORK(w, f) (*(w) = (struct work_struct){ .fn = f })
#define INIT_WORK_ONSTACK(w, f) INIT_WORK(w, f)
#define INIT_DELAYED_WORK(w, f) INIT_WORK(&(w)->work, f)
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
static struct workqueue_struct *alloc_ordered_workqueue(const char *n, int flags)
{ return calloc(1, sizeof(struct workqueue_struct)); }
static bool schedule(struct workqueue_struct *q, struct work_struct *w, unsigned long delay)
{
	if (w->queued) return false;
	assert(q->n < 512); w->queued = true; w->queue = q;
	q->tasks[q->n++] = (struct task){ w, jiffies + delay, q->next++ };
	return true;
}
#define queue_work(q, w) schedule(q, w, 0)
#define queue_delayed_work(q, w, d) schedule(q, &(w)->work, d)
static void cancel_delayed_work_sync(struct delayed_work *d)
{
	struct work_struct *w = &d->work;
	if (!w->queued) return;
	struct workqueue_struct *q = w->queue;
	for (int i = 0; i < q->n; i++) if (q->tasks[i].work == w) {
		q->tasks[i] = q->tasks[--q->n]; w->queued = false; return;
	}
	abort();
}
static void mod_delayed_work(struct workqueue_struct *q, struct delayed_work *w, int d)
{ cancel_delayed_work_sync(w); schedule(q, &w->work, d); }
static bool run_one(struct workqueue_struct *q)
{
	int best = -1;
	for (int i = 0; i < q->n; i++) {
		if (time_after(q->tasks[i].due, jiffies)) continue;
		if (best < 0 || q->tasks[i].order < q->tasks[best].order) best = i;
	}
	if (best < 0) return false;
	struct work_struct *w = q->tasks[best].work;
	q->tasks[best] = q->tasks[--q->n]; w->queued = false; w->fn(w);
	return true;
}
static void run_ready(struct workqueue_struct *q)
{ int bound = 10000; while (run_one(q)) assert(--bound); }
static void flush_work(struct work_struct *w)
{ while (w->queued) assert(run_one(w->queue)); }
static void destroy_work_on_stack(struct work_struct *w) { assert(!w->queued); }
static void destroy_workqueue(struct workqueue_struct *q)
{ run_ready(q); assert(!q->n); free(q); }

struct ppe_natflow;
struct ppe_natflow_pair;
struct flow_block;
struct qca_ppe_priv;
struct dsa_switch { int unused; };
struct dsa_port { struct dsa_switch *ds; int index; };
struct net_device { struct dsa_port dp; int refs, type; bool running, carrier; };
static int init_net;
#define dev_net(dev) (&init_net)
#define dsa_port_from_netdev(dev) (&(dev)->dp)
#define dsa_port_is_user(dp) ((dp)->index > 0)
#define netif_running(dev) ((dev)->running)
#define netif_carrier_ok(dev) ((dev)->carrier)
static void dev_hold(struct net_device *d) { d->refs++; }
static void dev_put(struct net_device *d) { assert(--d->refs >= 1); }
static bool is_valid_ether_addr(const u8 *p) { return !(p[0] & 1) && memcmp(p, "\0\0\0\0\0\0", 6); }
struct ppe_data { u32 num_flow_entries; };
struct qca_ppe_priv {
	struct dsa_switch ds;
	struct ppe_natflow *natflow;
	struct mutex flow_lock, vlan_lock;
	struct list_head flow_list;
	int flow_table;
	u8 port_vsi[8];
	u8 pvid_vsi[8];
	const struct ppe_data *data;
};
struct ppe_flow_entry {
	struct ppe_natflow_pair *pair;
	struct list_head list;
	void *block;
	int node;
	u8 src_if;
	int age;
	u32 packets;
	u64 bytes;
};
static int ppe_flow_ht_params;
static int ppe_flow_src_if(struct qca_ppe_priv *p, int port, u16 vid)
{ return p->pvid_vsi[port] ?: p->port_vsi[port]; }
#define rhashtable_remove_fast(h, n, p) ((void)(p))
static int hardware_entries, create_calls, fail_create;
static void ppe_flow_entry_destroy(struct qca_ppe_priv *p, struct ppe_flow_entry *e)
{ assert(--hardware_entries >= 0); }
void ppe_flow_free(struct qca_ppe_priv *, struct ppe_flow_entry *);
void ppe_natflow_invalidate(struct qca_ppe_priv *, struct ppe_natflow_pair *);
static int ppe_flow_poll(struct qca_ppe_priv *p, struct ppe_flow_entry *e, u32 *pkts, u64 *bytes)
{ *pkts = e->packets; *bytes = e->bytes; e->packets = 0; e->bytes = 0; return e->age; }
struct seq_file { int unused; };
#define seq_printf(s, ...) ((void)0)

/* PRODUCTION */

static struct ppe_flow_data encoded[4];
static struct ppe_flow_entry *ppe_flow_create(struct qca_ppe_priv *p,
	struct ppe_flow_data *data, int port, unsigned long cookie, struct flow_block *block)
{
	create_calls++;
	if (create_calls == fail_create) return ERR_PTR(-ENOSPC);
	encoded[(create_calls - 1) % 4] = *data;
	struct ppe_flow_entry *e = calloc(1, sizeof(*e));
	e->src_if = ppe_flow_src_if(p, port, 0); e->age = PPE_FLOW_AGE_MAX;
	list_add_tail(&e->list, &p->flow_list); hardware_entries++;
	return e;
}

#define NATFLOW_HAVE_QCA_PPE 1
#define NATFLOW_FASTNAT_TABLE_SIZE 16
#define NATFLOW_FASTNAT_TABLE_WAYS 2
#define NATFLOW_PATH_MAGIC_MASK 0xffff
#define NATFLOW_FF_TIMEOUT_LOW (25 * HZ)
#define TCP_CONNTRACK_ESTABLISHED 3
#define IP_CT_DIR_ORIGINAL 0
#define IP_CT_DIR_REPLY 1
#define IPS_DYING_BIT 0
#define IPS_NATFLOW_FF_STOP BIT(1)
#define NF_FF_BUSY_USE BIT(2)
#define NF_FF_TOKEN_CTRL BIT(3)
#define NF_FF_BRIDGE BIT(4)
#define AUTH_BLOCK 1
#define NFN_L3NUM_DEC(flags) AF_INET
#define NFN_PROTO_DEC(flags) (flags)
#define atomic_read_acquire(p) (*(p))
#define test_bit(bit, p) (!!(*(p) & BIT(bit)))
#define ulongmindiff(a, b) ((unsigned long)((a) - (b)))
typedef struct { __be32 ip; } mock_addr;
struct nf_conntrack_tuple {
	struct { mock_addr u3; union { __be16 all; } u; int l3num; } src;
	struct { mock_addr u3; union { __be16 all; } u; int protonum; } dst;
};
struct mock_route { int vlan_present, l2_head_len; struct net_device *outdev; u8 l2_head[14]; };
typedef struct { u32 status; u16 magic; struct mock_route rroute[2]; } natflow_t;
struct fakeuser_data_t { int auth_status; };
typedef struct { struct fakeuser_data_t data; } natflow_fakeuser_t;
struct nf_conn {
	struct { struct nf_conntrack_tuple tuple; } tuplehash[2];
	unsigned long status;
	struct { struct { int state; } tcp; } proto;
	int ct_general;
	natflow_t nf;
	natflow_fakeuser_t *user;
	u64 bytes[2];
};
typedef struct {
	__be32 saddr, daddr; __be16 source, dest; int flags;
	u16 magic; unsigned long jiffies;
} natflow_fastnat_node_t;
static natflow_fastnat_node_t nodes[16], *natflow_fast_nat_table = nodes;
static int disabled, hwnat = 1, natflow_path_magic = 7;
static u32 natflow_hash_v4(__be32 s, __be32 d, __be16 sp, __be16 dp)
{ return ntohl(s) & 14; }
static natflow_t *natflow_session_get(struct nf_conn *ct) { return &ct->nf; }
static natflow_fakeuser_t *natflow_user_get(struct nf_conn *ct) { return ct->user; }
static struct fakeuser_data_t *natflow_fakeuser_data(natflow_fakeuser_t *u) { return &u->data; }
static void nf_conntrack_get(int *ref) { (*ref)++; }
static void nf_ct_put(struct nf_conn *ct) { assert(--ct->ct_general >= 1); }
#define nf_ct_zone(ct) (ct)
#define nf_ct_zone_id(zone, dir) 0
static int natflow_offload_keepalive_ex(unsigned int, unsigned long,
	unsigned long, unsigned int *, unsigned int *, int, unsigned long, int,
	struct nf_conn *);

/* PRODUCTION */

static int natflow_offload_keepalive_ex(unsigned int hash, unsigned long bytes,
	unsigned long packets, unsigned int *sbytes, unsigned int *spkts,
	int hw, unsigned long now, int netdev, struct nf_conn *expected_ct)
{
	struct natflow_qca_ticket *t = natflow_qca_slots[hash].owner;
	assert(t && !netdev && hw == 2 && bh_depth);
	assert(expected_ct == t->ct);
	int d = t->hash[1] == hash;
	t->ct->bytes[d] += bytes;
	nodes[t->hash[0]].jiffies = nodes[t->hash[1]].jiffies = now;
	return 0;
}

static struct qca_ppe_priv priv;
static const struct ppe_data chip = { .num_flow_entries = 8 };
static struct net_device lan, wan;

static void setup(void)
{
	assert(!hardware_entries);
	priv = (struct qca_ppe_priv){ .data = &chip };
	/* Ingress classification may select the PVID's VSI, not port_vsi. */
	priv.pvid_vsi[1] = priv.pvid_vsi[2] = 7;
	INIT_LIST_HEAD(&priv.flow_list);
	lan = (struct net_device){ .dp = { &priv.ds, 1 }, .refs = 1, .type = ARPHRD_ETHER, .running = true, .carrier = true };
	wan = lan; wan.dp.index = 2;
	create_calls = fail_create = 0; jiffies = HZ; disabled = 0; hwnat = 1;
	natflow_path_magic = 7; memset(nodes, 0, sizeof(nodes));
	natflow_qca_slots = calloc(16, sizeof(*natflow_qca_slots));
	assert(!ppe_natflow_init(&priv));
}

static struct nf_conn connection(void)
{
	struct nf_conn ct = { .ct_general = 1 };
	ct.tuplehash[0].tuple = (struct nf_conntrack_tuple){
		.src = { .u3.ip = htonl(2), .u.all = htons(1000), .l3num = AF_INET },
		.dst = { .u3.ip = htonl(4), .u.all = htons(80), .protonum = IPPROTO_TCP } };
	ct.tuplehash[1].tuple = (struct nf_conntrack_tuple){
		.src = { .u3.ip = htonl(4), .u.all = htons(80), .l3num = AF_INET },
		.dst = { .u3.ip = htonl(6), .u.all = htons(4000), .protonum = IPPROTO_TCP } };
	ct.proto.tcp.state = TCP_CONNTRACK_ESTABLISHED; ct.nf.magic = natflow_path_magic;
	for (int d = 0; d < 2; d++) {
		struct ethhdr eth = { .h_source = {2,0,0,0,0,1}, .h_dest = {2,0,0,0,0,2}, .h_proto = htons(ETH_P_IP) };
		ct.nf.rroute[d].l2_head_len = 14;
		ct.nf.rroute[d].outdev = d ? &lan : &wan;
		memcpy(ct.nf.rroute[d].l2_head, &eth, 14);
		struct nf_conntrack_tuple *t = &ct.tuplehash[d].tuple;
		u32 h = natflow_hash_v4(t->src.u3.ip, t->dst.u3.ip, t->src.u.all, t->dst.u.all);
		nodes[h] = (natflow_fastnat_node_t){ t->src.u3.ip, t->dst.u3.ip,
			t->src.u.all, t->dst.u.all, t->dst.protonum, 7, jiffies };
	}
	return ct;
}

static void finish(struct nf_conn *ct)
{
	disabled = 1; natflow_qca_stop();
	assert(ct->ct_general == 1 && !hardware_entries && !priv.natflow->count);
	assert(lan.refs == 1 && wan.refs == 1 && list_empty(&priv.flow_list));
	ppe_natflow_exit(&priv); free(natflow_qca_slots); natflow_qca_slots = NULL;
}

struct events { int ready, released, error; };
static int record(void *ctx, enum qca_ppe_natflow_event event,
		  const struct qca_ppe_natflow_stats *stats)
{
	struct events *e = ctx;
	assert(bh_depth && !priv.flow_lock.held && !priv.vlan_lock.held);
	if (event == QCA_PPE_NATFLOW_READY) e->ready++;
	if (event == QCA_PPE_NATFLOW_RELEASE) {
		e->released++; e->error = stats->error;
	}
	return 0;
}

int main(void)
{
	struct nf_conn ct;
	struct ppe_natflow_pair *pair;
	struct natflow_qca_ticket *ticket;

	setup(); ct = connection();
	natflow_qca_try(&ct, &ct.nf);
	assert(priv.natflow->count == 1 && !hardware_entries && ct.ct_general == 2);
	assert(ppe_natflow_ft_get(&priv) == -EBUSY);
	natflow_qca_try(&ct, &ct.nf); assert(priv.natflow->count == 1);
	run_ready(priv.natflow->wq); assert(hardware_entries == 2);
	ticket = natflow_qca_slots[2].owner; assert(ticket && ticket->ready);
	assert(encoded[0].v4_src == htonl(2) && encoded[0].v4_src_new == htonl(6));
	assert(encoded[0].sport == htons(1000) && encoded[0].sport_new == htons(4000));
	assert(encoded[1].v4_dst == htonl(6) && encoded[1].v4_dst_new == htonl(2));
	pair = list_first_entry_or_null(&priv.natflow->pairs, struct ppe_natflow_pair, list);
	pair->entry[0]->packets = 2; pair->entry[0]->bytes = 200;
	jiffies += HZ; run_ready(priv.natflow->wq); assert(ct.bytes[0] == 200);
	/* A duplicate KEY ADD must not acquire somebody else's hardware slot. */
	struct events other = {};
	struct qca_ppe_natflow_request req = pair->req;
	req.owner = &other; req.context = &other; req.notify = record;
	assert(!qca_ppe_natflow_submit(&req)); run_ready(priv.natflow->wq);
	assert(other.released == 1 && other.error == -EEXIST && hardware_entries == 2);
	/* Flush one owner without removing another owner's distinct connection. */
	other = (struct events){};
	req.tuple[0].src = htonl(10); req.tuple[1].dst = htonl(14);
	assert(!qca_ppe_natflow_submit(&req)); run_ready(priv.natflow->wq);
	assert(other.ready == 1 && hardware_entries == 4);
	qca_ppe_natflow_flush(&other);
	assert(other.released == 1 && hardware_entries == 2 && ct.ct_general == 2);
	jiffies += HZ; run_ready(priv.natflow->wq); assert(ct.bytes[0] == 200);
	/* Native routing-domain teardown invalidates both directions at once. */
	mutex_lock(&priv.flow_lock); mutex_lock(&priv.vlan_lock);
	ppe_flow_free(&priv, pair->entry[0]);
	mutex_unlock(&priv.vlan_lock); mutex_unlock(&priv.flow_lock);
	assert(!hardware_entries); run_ready(priv.natflow->wq); finish(&ct);

	setup(); ct = connection(); fail_create = 2;
	natflow_qca_try(&ct, &ct.nf); run_ready(priv.natflow->wq);
	assert(!hardware_entries && !priv.natflow->count && ct.ct_general == 1);
	assert(!natflow_qca_slots[2].owner && natflow_qca_slots[2].retry > jiffies);
	natflow_qca_try(&ct, &ct.nf); assert(!priv.natflow->count); finish(&ct);

	setup(); ct = connection(); natflow_qca_try(&ct, &ct.nf);
	/* ABA: same tuple and global magic, but a new publication generation. */
	natflow_qca_write_begin(2); natflow_qca_write_end();
	run_ready(priv.natflow->wq);
	assert(!hardware_entries && !priv.natflow->count && ct.ct_general == 1); finish(&ct);

	setup(); ct = connection(); natflow_qca_try(&ct, &ct.nf);
	ct.status |= BIT(IPS_DYING_BIT); run_ready(priv.natflow->wq);
	assert(!hardware_entries && ct.ct_general == 1); finish(&ct);

	setup(); ct = connection(); natflow_qca_try(&ct, &ct.nf);
	wan.carrier = false; run_ready(priv.natflow->wq);
	assert(!hardware_entries && ct.ct_general == 1); finish(&ct);

	setup(); ct = connection(); natflow_qca_try(&ct, &ct.nf);
	run_ready(priv.natflow->wq); ct.nf.status |= NF_FF_TOKEN_CTRL;
	jiffies += HZ; run_ready(priv.natflow->wq);
	assert(!hardware_entries && ct.ct_general == 1); finish(&ct);

	setup(); ct = connection(); natflow_qca_try(&ct, &ct.nf);
	run_ready(priv.natflow->wq); natflow_path_magic++;
	jiffies += HZ; run_ready(priv.natflow->wq);
	assert(!hardware_entries && ct.ct_general == 1); finish(&ct);

	setup(); ct = connection(); assert(!ppe_natflow_ft_get(&priv));
	natflow_qca_try(&ct, &ct.nf);
	assert(!priv.natflow->count && ct.ct_general == 1);
	ppe_natflow_ft_put(&priv); finish(&ct);

	setup(); ct = connection(); priv.natflow->stopping = true;
	assert(ppe_natflow_ft_get(&priv) == -ESHUTDOWN);
	finish(&ct);

	setup(); ct = connection();
	other = (struct events){};
	req.tuple[1].dst = req.tuple[1].src;
	req.tuple[1].dport = req.tuple[1].sport;
	req.tuple[0] = req.tuple[1];
	assert(!qca_ppe_natflow_submit(&req)); run_ready(priv.natflow->wq);
	assert(other.released == 1 && other.error == -EEXIST && !hardware_entries);
	finish(&ct);

	setup(); ct = connection(); natflow_qca_try(&ct, &ct.nf);
	run_ready(priv.natflow->wq);
	pair = list_first_entry_or_null(&priv.natflow->pairs, struct ppe_natflow_pair, list);
	pair->entry[0]->age = pair->entry[1]->age = 2;
	jiffies += 21 * HZ; run_ready(priv.natflow->wq);
	assert(!hardware_entries && ct.ct_general == 1); finish(&ct);

	setup(); ct = connection(); natflow_qca_try(&ct, &ct.nf);
	/* Device teardown also drains requests which have not reached hardware. */
	ppe_natflow_exit(&priv);
	assert(!priv.natflow && !hardware_entries && ct.ct_general == 1);
	assert(lan.refs == 1 && wan.refs == 1);
	assert(ppe_natflow_ft_get(&priv) == -ESHUTDOWN);
	ppe_natflow_ft_put(&priv);
	free(natflow_qca_slots); natflow_qca_slots = NULL;
	puts("natflow/PPE: async admission, SNAT/DNAT translation, pair rollback, identity ABA, policy invalidation, stats, idle expiry, nft exclusion and teardown PASS");
	return 0;
}
