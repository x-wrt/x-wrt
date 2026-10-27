// SPDX-License-Identifier: GPL-2.0-or-later
/* An asynchronous, bidirectional natflow front end to the shared PPE tables. */
#include <linux/etherdevice.h>
#include <linux/export.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include <net/dsa.h>
#include <net/flow_offload.h>
#include <net/net_namespace.h>

#include "qca_ppe_flow.h"

struct ppe_natflow_pair {
	struct list_head list;
	struct qca_ppe_natflow_request req;
	struct qca_ppe_natflow_path path[2];
	struct ppe_flow_entry *entry[2];
	int uplink[2];
	int session[2];
	unsigned long last_hit;
	bool invalid;
};

struct ppe_natflow {
	struct list_head instances;
	struct qca_ppe_priv *priv;
	struct workqueue_struct *wq;
	struct work_struct add;
	struct delayed_work poll;
	spinlock_t lock;
	struct list_head pending;
	/* Only the ordered worker changes this list. Entries themselves are
	 * also invalidated under flow_lock by switchdev/netdev teardown.
	 */
	struct list_head pairs;
	unsigned int count;
	unsigned int ft_blocks;
	bool stopping;
	u32 installed;
	u32 failed;
};

static LIST_HEAD(ppe_natflow_instances);
static DEFINE_MUTEX(ppe_natflow_instances_lock);

static int ppe_natflow_notify(struct ppe_natflow_pair *pair,
			       enum qca_ppe_natflow_event event,
			       const struct qca_ppe_natflow_stats *stats)
{
	int ret;

	local_bh_disable();
	rcu_read_lock();
	ret = pair->req.notify(pair->req.context, event, stats);
	rcu_read_unlock();
	local_bh_enable();
	return ret;
}

/* No callbacks while the PPE locks are held. A control-plane purge removes
 * both directions now, but leaves the request for the worker to release.
 */
static void ppe_natflow_delete(struct ppe_natflow *nf,
			       struct ppe_natflow_pair *pair)
{
	int d;

	lockdep_assert_held(&nf->priv->flow_lock);
	lockdep_assert_held(&nf->priv->vlan_lock);
	pair->invalid = true;
	for (d = 0; d < 2; d++) {
		struct ppe_flow_entry *entry = pair->entry[d];

		if (!entry)
			continue;
		pair->entry[d] = NULL;
		entry->pair = NULL;
		ppe_flow_free(nf->priv, entry);
	}
	for (d = 0; d < 2; d++) {
		ppe_flow_path_put(nf->priv, pair->uplink[d], pair->session[d]);
		pair->uplink[d] = -1;
		pair->session[d] = -1;
	}
}

void ppe_natflow_invalidate(struct qca_ppe_priv *priv,
			     struct ppe_natflow_pair *pair)
{
	struct ppe_natflow *nf = priv->natflow;

	ppe_natflow_delete(nf, pair);
	if (!READ_ONCE(nf->stopping))
		mod_delayed_work(nf->wq, &nf->poll, 0);
}

/* Logical PPP/VLAN devices are not retained in the learned physical paths.
 * Conservatively withdraw all client entries when one changes, without
 * waiting or invoking client callbacks from a netdev notifier under RTNL.
 */
void ppe_natflow_purge(struct qca_ppe_priv *priv)
{
	struct ppe_flow_entry *entry;

	lockdep_assert_held(&priv->flow_lock);
	lockdep_assert_held(&priv->vlan_lock);
restart:
	list_for_each_entry(entry, &priv->flow_list, list) {
		if (entry->pair) {
			ppe_natflow_invalidate(priv, entry->pair);
			goto restart;
		}
	}
}

static void ppe_natflow_release(struct ppe_natflow *nf,
				struct ppe_natflow_pair *pair, int error)
{
	struct qca_ppe_natflow_stats stats = { .error = error };

	mutex_lock(&nf->priv->flow_lock);
	mutex_lock(&nf->priv->vlan_lock);
	ppe_natflow_delete(nf, pair);
	mutex_unlock(&nf->priv->vlan_lock);
	mutex_unlock(&nf->priv->flow_lock);
	ppe_natflow_notify(pair, QCA_PPE_NATFLOW_RELEASE, &stats);
	dev_put(pair->req.dev[0]);
	dev_put(pair->req.dev[1]);
	spin_lock_bh(&nf->lock);
	nf->count--;
	spin_unlock_bh(&nf->lock);
	kfree(pair);
}

static void ppe_natflow_poll_pairs(struct ppe_natflow *nf)
{
	struct qca_ppe_priv *priv = nf->priv;
	struct ppe_natflow_pair *pair, *tmp;

	list_for_each_entry_safe(pair, tmp, &nf->pairs, list) {
		struct qca_ppe_natflow_stats stats = {};
		int d, ret;

		ret = ppe_natflow_notify(pair, QCA_PPE_NATFLOW_CHECK, &stats);
		if (ret)
			goto retire;
		mutex_lock(&priv->flow_lock);
		mutex_lock(&priv->vlan_lock);
		if (pair->invalid) {
			ret = -ESTALE;
		} else {
			for (d = 0; d < 2; d++) {
				int age = ppe_flow_poll(priv, pair->entry[d],
						&stats.packets[d], &stats.bytes[d]);

				if (age < 0) {
					ret = age;
					break;
				}
				if (stats.packets[d] || stats.bytes[d] || age == PPE_FLOW_AGE_MAX)
					stats.active |= BIT(d);
			}
		}
		mutex_unlock(&priv->vlan_lock);
		mutex_unlock(&priv->flow_lock);
		if (ret)
			goto retire;
		if (stats.active)
			pair->last_hit = jiffies;
		/* Retire before natflow's 25s software timeout. An inactive poll
		 * only checks identity; it must never keep an idle flow alive.
		 */
		if (time_after(jiffies, pair->last_hit + 20 * HZ)) {
			ret = -ETIMEDOUT;
			goto retire;
		}
		ret = ppe_natflow_notify(pair, QCA_PPE_NATFLOW_STATS, &stats);
		if (!ret)
			continue;
retire:
		list_del(&pair->list);
		ppe_natflow_release(nf, pair, ret);
	}
}

static void ppe_natflow_poll_work(struct work_struct *work)
{
	struct ppe_natflow *nf = container_of(to_delayed_work(work),
					      struct ppe_natflow, poll);

	ppe_natflow_poll_pairs(nf);
	if (!READ_ONCE(nf->stopping) && !list_empty(&nf->pairs))
		queue_delayed_work(nf->wq, &nf->poll, HZ);
}

/* Translate direction d to the inverse of the opposite conntrack tuple. */
static void ppe_natflow_data(const struct ppe_natflow_pair *pair, int d,
			     struct ppe_flow_data *data)
{
	const struct qca_ppe_natflow_request *req = &pair->req;
	const struct qca_ppe_natflow_path *path = &pair->path[d];
	const struct qca_ppe_natflow_tuple *t = &req->tuple[d];
	const struct qca_ppe_natflow_tuple *r = &req->tuple[!d];

	*data = (struct ppe_flow_data) {
		.eth = req->eth[d],
		.addr_type = FLOW_DISSECTOR_KEY_IPV4_ADDRS,
		.l4proto = t->proto,
		.v4_src = t->src, .v4_dst = t->dst,
		.v4_src_new = r->dst, .v4_dst_new = r->src,
		.sport = t->sport, .dport = t->dport,
		.sport_new = r->dport, .dport_new = r->sport,
		.odev = req->dev[!d],
		.ivid = pair->path[!d].vlan_id,
		.vlan_valid = !!(path->flags & QCA_PPE_NATFLOW_VLAN),
		.vlan_id = path->vlan_id,
		.pppoe_valid = !!(path->flags & QCA_PPE_NATFLOW_PPPOE),
		.pppoe_sid = path->pppoe_sid,
		.mtu = path->mtu,
	};
}

static bool ppe_natflow_tuple_equal(const struct qca_ppe_natflow_tuple *a,
				    const struct qca_ppe_natflow_tuple *b)
{
	return a->src == b->src && a->dst == b->dst &&
	       a->sport == b->sport && a->dport == b->dport &&
	       a->proto == b->proto;
}

static int ppe_natflow_install(struct ppe_natflow *nf,
			       struct ppe_natflow_pair *pair)
{
	struct qca_ppe_natflow_stats stats = {};
	struct qca_ppe_priv *priv = nf->priv;
	int d, ret;

	ret = ppe_natflow_notify(pair, QCA_PPE_NATFLOW_CHECK, &stats);
	if (ret)
		return ret;
	mutex_lock(&priv->flow_lock);
	mutex_lock(&priv->vlan_lock);
	/* Both paths must classify before any rule acquires an ingress VSI.
	 * The seed references also outlive both entries during rollback.
	 */
	for (d = 0; d < 2; d++) {
		struct dsa_port *dp = dsa_port_from_netdev(pair->req.dev[d]);
		struct ppe_flow_data data;

		if (IS_ERR(dp) || dp->ds != &priv->ds ||
		    !netif_running(pair->req.dev[d]) ||
		    !netif_carrier_ok(pair->req.dev[d])) {
			ret = -ENODEV;
			goto unlock;
		}
		ppe_natflow_data(pair, !d, &data);
		ret = ppe_flow_path_get(priv, &data, dp->index,
					&pair->uplink[!d], &pair->session[!d]);
		if (ret)
			goto unlock;
	}
	for (d = 0; d < 2; d++) {
		struct ppe_flow_data data;
		struct ppe_flow_entry *entry;
		struct ppe_natflow_pair *other;
		struct dsa_port *dp = dsa_port_from_netdev(pair->req.dev[d]);
		const struct qca_ppe_natflow_tuple *t = &pair->req.tuple[d];
		int e, vsi;

		if (IS_ERR(dp) || dp->ds != &priv->ds ||
		    !netif_running(pair->req.dev[d]) ||
		    !netif_carrier_ok(pair->req.dev[d])) {
			ret = -ENODEV;
			break;
		}
		vsi = ppe_flow_src_if(priv, dp->index, pair->path[!d].vlan_id);
		if (vsi < 0) {
			ret = vsi;
			break;
		}
		/* KEY ADD can return an existing slot. Never let two owners
		 * acquire it, including PVID-classified ingress or two
		 * directions sharing the same key.
		 */
		if (d && pair->entry[0]->src_if == vsi &&
		    ppe_natflow_tuple_equal(t, &pair->req.tuple[0])) {
			ret = -EEXIST;
			break;
		}
		list_for_each_entry(other, &nf->pairs, list) {
			for (e = 0; e < 2; e++) {
				const struct qca_ppe_natflow_tuple *r = &other->req.tuple[e];

				if (other->entry[e] &&
				    other->entry[e]->src_if == vsi &&
				    ppe_natflow_tuple_equal(t, r)) {
					ret = -EEXIST;
					goto unlock;
				}
			}
		}
		ppe_natflow_data(pair, d, &data);
		entry = ppe_flow_create(priv, &data, dp->index, 0, NULL);
		if (IS_ERR(entry)) {
			ret = PTR_ERR(entry);
			break;
		}
		pair->entry[d] = entry;
		entry->pair = pair;
	}
unlock:
	if (ret)
		ppe_natflow_delete(nf, pair);
	mutex_unlock(&priv->vlan_lock);
	mutex_unlock(&priv->flow_lock);
	if (ret)
		return ret;
	pair->last_hit = jiffies;
	return ppe_natflow_notify(pair, QCA_PPE_NATFLOW_READY, &stats);
}

static void ppe_natflow_add_work(struct work_struct *work)
{
	struct ppe_natflow *nf = container_of(work, struct ppe_natflow, add);
	struct ppe_natflow_pair *pair;

	/* Retire invalid old identities before a new flow can reuse a key. */
	ppe_natflow_poll_pairs(nf);
	for (int n = 0; n < 64; n++) {
		int ret;

		spin_lock_bh(&nf->lock);
		pair = list_first_entry_or_null(&nf->pending,
						struct ppe_natflow_pair, list);
		if (pair)
			list_del(&pair->list);
		spin_unlock_bh(&nf->lock);
		if (!pair)
			break;
		ret = READ_ONCE(nf->stopping) ? -ESHUTDOWN :
			ppe_natflow_install(nf, pair);
		if (ret) {
			nf->failed++;
			ppe_natflow_release(nf, pair, ret);
		} else {
			nf->installed++;
			list_add_tail(&pair->list, &nf->pairs);
		}
	}
	spin_lock_bh(&nf->lock);
	if (!list_empty(&nf->pending))
		queue_work(nf->wq, &nf->add);
	spin_unlock_bh(&nf->lock);
	if (!READ_ONCE(nf->stopping) && !list_empty(&nf->pairs))
		queue_delayed_work(nf->wq, &nf->poll, HZ);
}

static int ppe_natflow_submit(const struct qca_ppe_natflow_request *req,
			      const struct qca_ppe_natflow_path path[2])
{
	struct ppe_natflow_pair *pair;
	struct ppe_natflow *nf;
	struct dsa_port *dp[2];
	int d, ret = -EOPNOTSUPP;

	if (!req->notify || !req->owner)
		return -EINVAL;
	for (d = 0; d < 2; d++) {
		const struct qca_ppe_natflow_tuple *t = &req->tuple[d];

		if (!req->dev[d] || dev_net(req->dev[d]) != &init_net ||
		    (t->proto != IPPROTO_TCP && t->proto != IPPROTO_UDP) ||
		    t->proto != req->tuple[!d].proto ||
		    !is_valid_ether_addr(req->eth[d].h_source) ||
		    !is_valid_ether_addr(req->eth[d].h_dest) ||
		    req->eth[d].h_proto != htons(ETH_P_IP))
			return -EOPNOTSUPP;
		dp[d] = dsa_port_from_netdev(req->dev[d]);
		if (IS_ERR(dp[d]) || !dsa_port_is_user(dp[d]))
			return -EOPNOTSUPP;
		if (path) {
			const struct qca_ppe_natflow_path *p = &path[d];

			if (p->flags & ~(QCA_PPE_NATFLOW_VLAN | QCA_PPE_NATFLOW_PPPOE) ||
			    p->mtu < 68)
				return -EOPNOTSUPP;
			if (p->flags & QCA_PPE_NATFLOW_VLAN) {
				if (p->vlan_proto != htons(ETH_P_8021Q) ||
				    !p->vlan_id || p->vlan_id >= VLAN_VID_MASK)
					return -EOPNOTSUPP;
			} else if (p->vlan_id || p->vlan_proto) {
				return -EINVAL;
			}
			if (p->flags & QCA_PPE_NATFLOW_PPPOE) {
				if (!p->pppoe_sid || p->pppoe_sid == 0xffff)
					return -EOPNOTSUPP;
			} else if (p->pppoe_sid) {
				return -EINVAL;
			}
		}
	}
	if (dp[0]->ds != dp[1]->ds)
		return -EOPNOTSUPP;
	pair = kzalloc(sizeof(*pair), GFP_ATOMIC);
	if (!pair)
		return -ENOMEM;
	pair->req = *req;
	if (path)
		memcpy(pair->path, path, sizeof(pair->path));
	for (d = 0; d < 2; d++) {
		pair->uplink[d] = -1;
		pair->session[d] = -1;
	}
	rcu_read_lock();
	list_for_each_entry_rcu(nf, &ppe_natflow_instances, instances) {
		if (&nf->priv->ds != dp[0]->ds)
			continue;
		spin_lock_bh(&nf->lock);
		if (nf->stopping)
			ret = -ESHUTDOWN;
		else if (nf->ft_blocks)
			ret = -EBUSY;
		else if (nf->count >= nf->priv->data->num_flow_entries / 2)
			ret = -ENOSPC;
		else {
			dev_hold(req->dev[0]);
			dev_hold(req->dev[1]);
			nf->count++;
			list_add_tail(&pair->list, &nf->pending);
			queue_work(nf->wq, &nf->add);
			ret = 0;
		}
		spin_unlock_bh(&nf->lock);
		break;
	}
	rcu_read_unlock();
	if (ret)
		kfree(pair);
	return ret;
}

int qca_ppe_natflow_submit(const struct qca_ppe_natflow_request *req)
{
	return ppe_natflow_submit(req, NULL);
}
EXPORT_SYMBOL_GPL(qca_ppe_natflow_submit);

int qca_ppe_natflow_submit_encap(const struct qca_ppe_natflow_request *req,
			       const struct qca_ppe_natflow_path path[2])
{
	if (!path)
		return -EINVAL;
	return ppe_natflow_submit(req, path);
}
EXPORT_SYMBOL_GPL(qca_ppe_natflow_submit_encap);

struct ppe_natflow_flush {
	struct work_struct work;
	struct ppe_natflow *nf;
	void *owner;
};

static void ppe_natflow_flush_work(struct work_struct *work)
{
	struct ppe_natflow_flush *flush = container_of(work,
						struct ppe_natflow_flush, work);
	struct ppe_natflow_pair *pair, *tmp;
	LIST_HEAD(pending);

	spin_lock_bh(&flush->nf->lock);
	list_for_each_entry_safe(pair, tmp, &flush->nf->pending, list)
		if (!flush->owner || pair->req.owner == flush->owner)
			list_move_tail(&pair->list, &pending);
	spin_unlock_bh(&flush->nf->lock);
	list_for_each_entry_safe(pair, tmp, &pending, list) {
		list_del(&pair->list);
		ppe_natflow_release(flush->nf, pair, -ECANCELED);
	}
	list_for_each_entry_safe(pair, tmp, &flush->nf->pairs, list) {
		if (flush->owner && pair->req.owner != flush->owner)
			continue;
		list_del(&pair->list);
		ppe_natflow_release(flush->nf, pair, -ECANCELED);
	}
}

static void ppe_natflow_flush(struct ppe_natflow *nf, void *owner)
{
	struct ppe_natflow_flush flush = { .nf = nf, .owner = owner };

	flush_work(&nf->add);
	INIT_WORK_ONSTACK(&flush.work, ppe_natflow_flush_work);
	queue_work(nf->wq, &flush.work);
	flush_work(&flush.work);
	destroy_work_on_stack(&flush.work);
}

void qca_ppe_natflow_flush(void *owner)
{
	struct ppe_natflow *nf;

	mutex_lock(&ppe_natflow_instances_lock);
	list_for_each_entry(nf, &ppe_natflow_instances, instances)
		ppe_natflow_flush(nf, owner);
	mutex_unlock(&ppe_natflow_instances_lock);
}
EXPORT_SYMBOL_GPL(qca_ppe_natflow_flush);

int ppe_natflow_ft_get(struct qca_ppe_priv *priv)
{
	struct ppe_natflow *nf;
	int ret = 0;

	/* Driver removal stops the API before unregistering the DSA ports.
	 * A flowtable bind/unbind can still arrive in that interval.
	 */
	mutex_lock(&priv->flow_lock);
	nf = priv->natflow;
	if (!nf) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	spin_lock_bh(&nf->lock);
	if (nf->stopping)
		ret = -ESHUTDOWN;
	else if (nf->count)
		ret = -EBUSY;
	else
		nf->ft_blocks++;
	spin_unlock_bh(&nf->lock);
unlock:
	mutex_unlock(&priv->flow_lock);
	return ret;
}

void ppe_natflow_ft_put(struct qca_ppe_priv *priv)
{
	struct ppe_natflow *nf;

	mutex_lock(&priv->flow_lock);
	nf = priv->natflow;
	if (nf) {
		spin_lock_bh(&nf->lock);
		nf->ft_blocks--;
		spin_unlock_bh(&nf->lock);
	}
	mutex_unlock(&priv->flow_lock);
}

int ppe_natflow_init(struct qca_ppe_priv *priv)
{
	struct ppe_natflow *nf;

	nf = kzalloc(sizeof(*nf), GFP_KERNEL);
	if (!nf)
		return -ENOMEM;
	nf->wq = alloc_ordered_workqueue("qca_ppe_natflow", WQ_MEM_RECLAIM);
	if (!nf->wq) {
		kfree(nf);
		return -ENOMEM;
	}
	nf->priv = priv;
	spin_lock_init(&nf->lock);
	INIT_LIST_HEAD(&nf->pending);
	INIT_LIST_HEAD(&nf->pairs);
	INIT_WORK(&nf->add, ppe_natflow_add_work);
	INIT_DELAYED_WORK(&nf->poll, ppe_natflow_poll_work);
	priv->natflow = nf;
	mutex_lock(&ppe_natflow_instances_lock);
	list_add_rcu(&nf->instances, &ppe_natflow_instances);
	mutex_unlock(&ppe_natflow_instances_lock);
	return 0;
}

void ppe_natflow_exit(struct qca_ppe_priv *priv)
{
	struct ppe_natflow *nf = priv->natflow;

	if (!nf)
		return;
	mutex_lock(&ppe_natflow_instances_lock);
	spin_lock_bh(&nf->lock);
	nf->stopping = true;
	spin_unlock_bh(&nf->lock);
	list_del_rcu(&nf->instances);
	synchronize_rcu();
	cancel_delayed_work_sync(&nf->poll);
	ppe_natflow_flush(nf, NULL);
	destroy_workqueue(nf->wq);
	mutex_lock(&priv->flow_lock);
	priv->natflow = NULL;
	mutex_unlock(&priv->flow_lock);
	mutex_unlock(&ppe_natflow_instances_lock);
	kfree(nf);
}

void ppe_natflow_show(struct qca_ppe_priv *priv, struct seq_file *s)
{
	struct ppe_natflow *nf = priv->natflow;

	if (!nf)
		return;
	spin_lock_bh(&nf->lock);
	seq_printf(s, "%-24s %u\n", "natflow_requests", nf->count);
	seq_printf(s, "%-24s %u\n", "nft_blocks", nf->ft_blocks);
	spin_unlock_bh(&nf->lock);
	seq_printf(s, "%-24s %u\n", "natflow_installed", READ_ONCE(nf->installed));
	seq_printf(s, "%-24s %u\n", "natflow_failed", READ_ONCE(nf->failed));
}
