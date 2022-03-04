// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018 MediaTek Inc.
 */

#include <linux/errno.h>

#include "mt753x.h"
#include "mt753x_regs.h"

struct mt753x_mapping mt753x_def_mapping[] = {
	{
		.name = "llllw",
		.pvids = { 1, 1, 1, 1, 2, 2, 1 },
		.members = { 0, 0x4f, 0x30 },
		.etags = { 0, 0, 0 },
		.vids = { 0, 1, 2 },
	}, {
		.name = "wllll",
		.pvids = { 2, 1, 1, 1, 1, 2, 1 },
		.members = { 0, 0x5e, 0x21 },
		.etags = { 0, 0, 0 },
		.vids = { 0, 1, 2 },
	}, {
		.name = "lwlll",
		.pvids = { 1, 2, 1, 1, 1, 2, 1 },
		.members = { 0, 0x5d, 0x22 },
		.etags = { 0, 0, 0 },
		.vids = { 0, 1, 2 },
	},
};

int mt753x_vlan_ctrl(struct gsw_mt753x *gsw, u32 cmd, u32 val)
{
	int i, ret;

	ret = mt753x_reg_write(gsw, VTCR,
	                       VTCR_BUSY | ((cmd << VTCR_FUNC_S) & VTCR_FUNC_M) |
	                       (val & VTCR_VID_M));
	if (ret < 0)
		return ret;

	for (i = 0; i < 300; i++) {
		u32 val;

		ret = mt753x_reg_read_checked(gsw, VTCR, &val);
		if (ret < 0)
			return ret;

		if ((val & VTCR_BUSY) == 0)
			return 0;

		usleep_range(1000, 1100);
	}

	dev_info(gsw->dev, "vtcr timeout\n");
	return -ETIMEDOUT;
}

static int mt753x_write_vlan_entry(struct gsw_mt753x *gsw, int vlan, u16 vid,
                                    u8 ports, u8 etags)
{
	int port, ret;
	u32 val;

	/* vlan port membership */
	if (ports)
		ret = mt753x_reg_write(gsw, VAWD1,
		                       IVL_MAC | VTAG_EN | VENTRY_VALID |
		                       ((ports << PORT_MEM_S) & PORT_MEM_M));
	else
		ret = mt753x_reg_write(gsw, VAWD1, 0);

	if (ret < 0)
		return ret;

	/* egress mode */
	val = 0;
	for (port = 0; port < MT753X_NUM_PORTS; port++) {
		if (etags & BIT(port))
			val |= ETAG_CTRL_TAG << PORT_ETAG_S(port);
		else
			val |= ETAG_CTRL_UNTAG << PORT_ETAG_S(port);
	}
	ret = mt753x_reg_write(gsw, VAWD2, val);
	if (ret < 0)
		return ret;

	/* write to vlan table */
	return mt753x_vlan_ctrl(gsw, VTCR_WRITE_VLAN_ENTRY, vid);
}

int mt753x_apply_vlan_config(struct gsw_mt753x *gsw)
{
	int i, j, ret;
	u8 tag_ports;
	u8 untag_ports;
	bool is_mirror = false;

	/* set all ports as security mode */
	for (i = 0; i < MT753X_NUM_PORTS; i++) {
		ret = mt753x_reg_write(gsw, PCR(i),
		                       PORT_MATRIX_M | SECURITY_MODE);
		if (ret < 0)
			return ret;
	}

	/* check if a port is used in tag/untag vlan egress mode */
	tag_ports = 0;
	untag_ports = 0;

	for (i = 0; i < MT753X_NUM_VLANS; i++) {
		u8 member = gsw->vlan_entries[i].member;
		u8 etags = gsw->vlan_entries[i].etags;

		if (!member)
			continue;

		for (j = 0; j < MT753X_NUM_PORTS; j++) {
			if (!(member & BIT(j)))
				continue;

			if (etags & BIT(j))
				tag_ports |= 1u << j;
			else
				untag_ports |= 1u << j;
		}
	}

	/* set all untag-only ports as transparent and the rest as user port */
	for (i = 0; i < MT753X_NUM_PORTS; i++) {
		u32 pvc_mode = 0x8100 << STAG_VPID_S;

		if (untag_ports & BIT(i) && !(tag_ports & BIT(i)))
			pvc_mode = (0x8100 << STAG_VPID_S) |
			           (VA_TRANSPARENT_PORT << VLAN_ATTR_S);

		ret = mt753x_reg_write(gsw, PVC(i), pvc_mode);
		if (ret < 0)
			return ret;
	}

	/* first clear the switch vlan table */
	for (i = 0; i < MT753X_NUM_VLANS; i++) {
		ret = mt753x_write_vlan_entry(gsw, i, i, 0, 0);
		if (ret)
			return ret;
	}

	/* now program only vlans with members to avoid
	 * clobbering remapped entries in later iterations
	 */
	for (i = 0; i < MT753X_NUM_VLANS; i++) {
		u16 vid = gsw->vlan_entries[i].vid;
		u8 member = gsw->vlan_entries[i].member;
		u8 etags = gsw->vlan_entries[i].etags;

		if (member) {
			ret = mt753x_write_vlan_entry(gsw, i, vid, member, etags);
			if (ret)
				return ret;
		}
	}

	/* Port Default PVID */
	for (i = 0; i < MT753X_NUM_PORTS; i++) {
		int vlan = gsw->port_entries[i].pvid;
		u16 pvid = 0;
		u32 val;

		if (vlan < MT753X_NUM_VLANS && gsw->vlan_entries[vlan].member)
			pvid = gsw->vlan_entries[vlan].vid;

		ret = mt753x_reg_read_checked(gsw, PPBV1(i), &val);
		if (ret < 0)
			return ret;
		val &= ~GRP_PORT_VID_M;
		val |= pvid;
		ret = mt753x_reg_write(gsw, PPBV1(i), val);
		if (ret < 0)
			return ret;
	}

	/* FIXME: MT7530 only supports one monitor port, but MT7631 supports multi,
	 *        but here just supports one for now.
	 */

	/* set mirroring source port */
	for (i = 0; i < MT753X_NUM_PORTS; i++) {
		u32 val;

		ret = mt753x_reg_read_checked(gsw, PCR(i), &val);
		if (ret < 0)
			return ret;
		val &= ~(MIRROR_SRC_RX_BIT | MIRROR_SRC_TX_BIT);
		if (gsw->port_entries[i].mirror_rx) {
			val |= MIRROR_SRC_RX_BIT;
			is_mirror = true;
		}
		if (gsw->port_entries[i].mirror_tx) {
			val |= MIRROR_SRC_TX_BIT;
			is_mirror = true;
		}
		ret = mt753x_reg_write(gsw, PCR(i), val);
		if (ret < 0)
			return ret;
	}

	/* set mirroring monitor port */
	if (is_mirror) {
		u32 val;

		ret = mt753x_reg_read_checked(gsw, MT753X_MIRROR_REG(gsw), &val);
		if (ret < 0)
			return ret;
		val |= MT753X_MIRROR_EN(gsw);
		val &= ~MT753X_MIRROR_MASK(gsw);
		val |= MT753X_MIRROR_PORT_SET(gsw, gsw->mirror_dest_port);
		ret = mt753x_reg_write(gsw, MT753X_MIRROR_REG(gsw), val);
		if (ret < 0)
			return ret;
	} else {
		u32 val;

		ret = mt753x_reg_read_checked(gsw, MT753X_MIRROR_REG(gsw), &val);
		if (ret < 0)
			return ret;
		val &= ~MT753X_MIRROR_EN(gsw);
		val &= ~MT753X_MIRROR_MASK(gsw);
		ret = mt753x_reg_write(gsw, MT753X_MIRROR_REG(gsw), val);
		if (ret < 0)
			return ret;
	}

	return 0;
}

struct mt753x_mapping *mt753x_find_mapping(struct device_node *np)
{
	const char *map;
	int i;

	if (of_property_read_string(np, "mediatek,portmap", &map))
		return NULL;

	for (i = 0; i < ARRAY_SIZE(mt753x_def_mapping); i++)
		if (!strcmp(map, mt753x_def_mapping[i].name))
			return &mt753x_def_mapping[i];

	return NULL;
}

void mt753x_apply_mapping(struct gsw_mt753x *gsw, struct mt753x_mapping *map)
{
	int i = 0;

	for (i = 0; i < MT753X_NUM_PORTS; i++)
		gsw->port_entries[i].pvid = map->pvids[i];

	for (i = 0; i < MT753X_NUM_VLANS; i++) {
		gsw->vlan_entries[i].member = map->members[i];
		gsw->vlan_entries[i].etags = map->etags[i];
		gsw->vlan_entries[i].vid = map->vids[i];
	}
}
