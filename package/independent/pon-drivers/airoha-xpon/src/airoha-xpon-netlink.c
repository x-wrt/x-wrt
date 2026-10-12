// SPDX-License-Identifier: GPL-2.0-only

#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <net/genetlink.h>

#include "airoha-xpon-private.h"

static LIST_HEAD(airoha_xpon_instances);
static DEFINE_MUTEX(airoha_xpon_instances_lock);

static struct genl_family airoha_xpon_genl_family;

static struct airoha_xpon *airoha_xpon_find_locked(struct net *net, u32 ifindex)
{
	struct airoha_xpon *xpon;

	list_for_each_entry(xpon, &airoha_xpon_instances, netlink_node)
		if (dev_net(xpon->pon_netdev) == net &&
		    xpon->pon_netdev->ifindex == ifindex)
			return xpon;
	return NULL;
}

static int airoha_xpon_genl_get_line(struct sk_buff *skb,
                                     struct genl_info *info)
{
	struct airoha_xpon *xpon;
	struct sk_buff *reply;
	void *header;
	enum airoha_xpon_mode configured_mode;
	enum airoha_xpon_mode active_mode = 0;
	bool active_mode_valid;
	u32 ifindex;
	int ret;

	if (!info->attrs[AIROHA_XPON_ATTR_IFINDEX])
		return -EINVAL;
	ifindex = nla_get_u32(info->attrs[AIROHA_XPON_ATTR_IFINDEX]);

	mutex_lock(&airoha_xpon_instances_lock);
	xpon = airoha_xpon_find_locked(genl_info_net(info), ifindex);
	if (!xpon) {
		mutex_unlock(&airoha_xpon_instances_lock);
		return -ENODEV;
	}
	mutex_lock(&xpon->state_lock);
	configured_mode = xpon->configured_mode;
	active_mode = xpon->active_mode;
	active_mode_valid = xpon->active_mode_valid;
	mutex_unlock(&xpon->state_lock);
	mutex_unlock(&airoha_xpon_instances_lock);

	reply = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!reply)
		return -ENOMEM;
	header = genlmsg_put_reply(reply, info, &airoha_xpon_genl_family, 0,
	                           AIROHA_XPON_CMD_GET_LINE);
	if (!header) {
		nlmsg_free(reply);
		return -EMSGSIZE;
	}
	ret = nla_put_u32(reply, AIROHA_XPON_ATTR_IFINDEX, ifindex) ?:
	              nla_put_u8(reply, AIROHA_XPON_ATTR_MODE, configured_mode);
	if (!ret && active_mode_valid)
		ret = nla_put_u8(reply, AIROHA_XPON_ATTR_ACTIVE_MODE,
		                 active_mode);
	if (ret) {
		nlmsg_free(reply);
		return ret;
	}
	genlmsg_end(reply, header);
	return genlmsg_reply(reply, info);
}

static int airoha_xpon_genl_set_line(struct sk_buff *skb,
                                     struct genl_info *info)
{
	struct airoha_xpon *xpon;
	enum airoha_xpon_mode mode;
	u32 ifindex;
	int ret;

	if (!info->attrs[AIROHA_XPON_ATTR_IFINDEX] ||
	    !info->attrs[AIROHA_XPON_ATTR_MODE])
		return -EINVAL;
	ifindex = nla_get_u32(info->attrs[AIROHA_XPON_ATTR_IFINDEX]);
	mode = nla_get_u8(info->attrs[AIROHA_XPON_ATTR_MODE]);

	mutex_lock(&airoha_xpon_instances_lock);
	xpon = airoha_xpon_find_locked(genl_info_net(info), ifindex);
	if (!xpon) {
		ret = -ENODEV;
		goto out_unlock_instances;
	}

	rtnl_lock();
	mutex_lock(&xpon->state_lock);
	ret = airoha_xpon_set_mode_locked(xpon, mode);
	mutex_unlock(&xpon->state_lock);
	rtnl_unlock();

out_unlock_instances:
	mutex_unlock(&airoha_xpon_instances_lock);
	return ret;
}

/* The registry lock pins the provider while the state lock captures its snapshot. */
static int airoha_xpon_genl_get_status(struct sk_buff *skb,
                                       struct genl_info *info)
{
	struct airoha_xpon *xpon;
	struct sk_buff *reply;
	void *header;
	int ret;

	if (!info->attrs[AIROHA_XPON_ATTR_IFINDEX])
		return -EINVAL;
	reply = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!reply)
		return -ENOMEM;
	header = genlmsg_put_reply(reply, info, &airoha_xpon_genl_family, 0,
	                           AIROHA_XPON_CMD_GET_STATUS);
	if (!header) {
		nlmsg_free(reply);
		return -EMSGSIZE;
	}
	mutex_lock(&airoha_xpon_instances_lock);
	xpon = airoha_xpon_find_locked(
		genl_info_net(info),
		nla_get_u32(info->attrs[AIROHA_XPON_ATTR_IFINDEX]));
	ret = xpon ? airoha_xpon_put_status(reply, xpon) : -ENODEV;
	mutex_unlock(&airoha_xpon_instances_lock);
	if (ret) {
		nlmsg_free(reply);
		return ret;
	}
	genlmsg_end(reply, header);
	return genlmsg_reply(reply, info);
}

static const struct nla_policy airoha_xpon_genl_policy[AIROHA_XPON_ATTR_MAX +
                                                       1] = {
	[AIROHA_XPON_ATTR_IFINDEX] = { .type = NLA_U32 },
	[AIROHA_XPON_ATTR_MODE] = NLA_POLICY_RANGE(
		NLA_U8, AIROHA_XPON_MODE_GPON, AIROHA_XPON_MODE_EPON_10G_10G),
	[AIROHA_XPON_ATTR_ACTIVE_MODE] = { .type = NLA_U8 },
};

static const struct genl_small_ops airoha_xpon_genl_ops[] = {
	{
		.cmd = AIROHA_XPON_CMD_GET_STATUS,
		.doit = airoha_xpon_genl_get_status,
	},
	{
		.cmd = AIROHA_XPON_CMD_GET_LINE,
		.doit = airoha_xpon_genl_get_line,
	},
	{
		.cmd = AIROHA_XPON_CMD_SET_LINE,
		.flags = GENL_ADMIN_PERM,
		.doit = airoha_xpon_genl_set_line,
	},
};

static struct genl_family airoha_xpon_genl_family = {
	.name = AIROHA_XPON_GENL_NAME,
	.version = AIROHA_XPON_GENL_VERSION,
	.maxattr = AIROHA_XPON_ATTR_MAX,
	.policy = airoha_xpon_genl_policy,
	.netnsok = true,
	.module = THIS_MODULE,
	.small_ops = airoha_xpon_genl_ops,
	.n_small_ops = ARRAY_SIZE(airoha_xpon_genl_ops),
};

int airoha_xpon_netlink_init(void)
{
	return genl_register_family(&airoha_xpon_genl_family);
}

void airoha_xpon_netlink_exit(void)
{
	genl_unregister_family(&airoha_xpon_genl_family);
}

void airoha_xpon_netlink_register(struct airoha_xpon *xpon)
{
	mutex_lock(&airoha_xpon_instances_lock);
	list_add_tail(&xpon->netlink_node, &airoha_xpon_instances);
	mutex_unlock(&airoha_xpon_instances_lock);
}

void airoha_xpon_netlink_unregister(struct airoha_xpon *xpon)
{
	mutex_lock(&airoha_xpon_instances_lock);
	list_del_init(&xpon->netlink_node);
	mutex_unlock(&airoha_xpon_instances_lock);
}
