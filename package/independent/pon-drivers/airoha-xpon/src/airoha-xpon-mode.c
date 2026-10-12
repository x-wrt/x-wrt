// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "airoha-xpon-private.h"
#include "airoha-xpon-oam.h"
#include "airoha-xpon-omci.h"

static const struct airoha_xpon_mode_info airoha_xpon_modes[] = {
	{
		.mode = AIROHA_XPON_MODE_GPON,
		.name = "gpon",
		.frontend_mode = AIROHA_PON_FRONTEND_MODE_GPON,
		.control_protocol = AIROHA_XPON_CONTROL_OMCI,
	},
	{
		.mode = AIROHA_XPON_MODE_XGPON,
		.name = "xgpon",
		.frontend_mode = AIROHA_PON_FRONTEND_MODE_XGPON,
		.control_protocol = AIROHA_XPON_CONTROL_OMCI,
		.mac_supported = true,
	},
	{
		.mode = AIROHA_XPON_MODE_XGSPON,
		.name = "xgspon",
		.frontend_mode = AIROHA_PON_FRONTEND_MODE_XGSPON,
		.control_protocol = AIROHA_XPON_CONTROL_OMCI,
		.mac_supported = true,
	},
	{
		.mode = AIROHA_XPON_MODE_EPON_1G,
		.name = "epon-1g",
		.frontend_mode = AIROHA_PON_FRONTEND_MODE_EPON_1G,
		.control_protocol = AIROHA_XPON_CONTROL_OAM,
		.mac_supported = true,
	},
	{
		.mode = AIROHA_XPON_MODE_EPON_10G_1G,
		.name = "epon-10g-1g",
		.frontend_mode = AIROHA_PON_FRONTEND_MODE_EPON_10G_1G,
		.control_protocol = AIROHA_XPON_CONTROL_OAM,
		.mac_supported = true,
	},
	{
		.mode = AIROHA_XPON_MODE_EPON_10G_10G,
		.name = "epon-10g-10g",
		.frontend_mode = AIROHA_PON_FRONTEND_MODE_EPON_10G_10G,
		.control_protocol = AIROHA_XPON_CONTROL_OAM,
		.mac_supported = true,
	},
};

const struct airoha_xpon_mode_info *
airoha_xpon_mode_info(enum airoha_xpon_mode mode)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(airoha_xpon_modes); i++)
		if (airoha_xpon_modes[i].mode == mode)
			return &airoha_xpon_modes[i];
	return NULL;
}

const char *airoha_xpon_mode_name(enum airoha_xpon_mode mode)
{
	const struct airoha_xpon_mode_info *info = airoha_xpon_mode_info(mode);

	return info ? info->name : "unknown";
}

bool airoha_xpon_pcs_profile_matches(enum airoha_xpon_mode mode,
                                     enum airoha_pcs_pon_mode profile)
{
	switch (mode) {
	case AIROHA_XPON_MODE_GPON:
		return profile == AIROHA_PCS_PON_MODE_GPON;
	case AIROHA_XPON_MODE_XGPON:
		return profile == AIROHA_PCS_PON_MODE_XGPON;
	case AIROHA_XPON_MODE_XGSPON:
		return profile == AIROHA_PCS_PON_MODE_XGSPON;
	case AIROHA_XPON_MODE_EPON_1G:
		return profile == AIROHA_PCS_PON_MODE_EPON_1G;
	case AIROHA_XPON_MODE_EPON_10G_1G:
		return profile == AIROHA_PCS_PON_MODE_EPON_10G_1G;
	case AIROHA_XPON_MODE_EPON_10G_10G:
		return profile == AIROHA_PCS_PON_MODE_EPON_10G_10G;
	default:
		return false;
	}
}

int airoha_xpon_mode_parse(const char *name, enum airoha_xpon_mode *mode)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(airoha_xpon_modes); i++) {
		if (!strcmp(airoha_xpon_modes[i].name, name)) {
			*mode = airoha_xpon_modes[i].mode;
			return 0;
		}
	}
	return -EINVAL;
}

int airoha_xpon_mode_get_default(struct device *dev,
                                 enum airoha_xpon_mode *mode)
{
	struct device_node *chosen;
	const char *bootargs, *name;
	char *args, *cursor, *param, *value;
	int ret = 0;

	*mode = AIROHA_XPON_MODE_XGPON;
	if (!of_property_read_string(dev->of_node, "airoha,default-line-mode",
	                             &name)) {
		ret = airoha_xpon_mode_parse(name, mode);
		if (ret)
			return dev_err_probe(
				dev, ret,
				"invalid airoha,default-line-mode value\n");
	}

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return 0;
	ret = of_property_read_string(chosen, "bootargs", &bootargs);
	of_node_put(chosen);
	if (ret)
		return 0;

	args = kstrdup(bootargs, GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	cursor = args;
	while (*cursor) {
		cursor = next_arg(cursor, &param, &value);
		if (strcmp(param, "xpon.mode"))
			continue;
		if (!value) {
			ret = -EINVAL;
			break;
		}
		ret = airoha_xpon_mode_parse(value, mode);
		if (ret)
			break;
	}
	kfree(args);
	if (ret)
		return dev_err_probe(
			dev, ret, "invalid xpon.mode in kernel command line\n");
	return 0;
}

int airoha_xpon_create_control_rtnl(struct airoha_xpon *xpon)
{
	ASSERT_RTNL();
	xpon->omci = airoha_xpon_omci_create_rtnl(xpon->dev, xpon->pon_netdev);
	if (IS_ERR(xpon->omci)) {
		int ret = PTR_ERR(xpon->omci);

		xpon->omci = NULL;
		return ret;
	}

	xpon->oam = airoha_xpon_oam_create_rtnl(xpon->dev, xpon->pon_netdev);
	if (IS_ERR(xpon->oam)) {
		int ret = PTR_ERR(xpon->oam);

		xpon->oam = NULL;
		airoha_xpon_omci_destroy_rtnl(xpon->omci);
		xpon->omci = NULL;
		return ret;
	}

	return 0;
}

int airoha_xpon_set_mode_locked(struct airoha_xpon *xpon,
                                enum airoha_xpon_mode mode)
{
	const struct airoha_xpon_mode_info *next;

	ASSERT_RTNL();
	lockdep_assert_held(&xpon->state_lock);
	next = airoha_xpon_mode_info(mode);
	if (!next)
		return -EINVAL;
	if (mode == xpon->configured_mode)
		return 0;
	xpon->configured_mode = mode;
	if (netif_running(xpon->pon_netdev) ||
	    xpon->lifecycle != AIROHA_XPON_STOPPED) {
		dev_info(
			xpon->dev,
			"PON line mode %s will take effect on the next interface activation\n",
			next->name);
		return 0;
	}

	dev_info(xpon->dev, "PON line mode set to %s\n", next->name);
	return 0;
}
