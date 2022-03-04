// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018 MediaTek Inc.
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#include <linux/kernel.h>
#include <linux/delay.h>

#include "mt753x.h"
#include "mt753x_regs.h"

int mt753x_irq_enable(struct gsw_mt753x *gsw)
{
	u32 val;
	int i, ret;

	/* Record initial PHY link status */
	for (i = 0; i < MT753X_NUM_PHYS; i++) {
		ret = gsw->mii_read(gsw, i, MII_BMSR);
		if (ret < 0)
			return ret;
		val = ret;
		if (val & BMSR_LSTATUS)
			gsw->phy_link_sts |= BIT(i);
	}

	val = BIT(MT753X_NUM_PHYS) - 1;

	ret = mt753x_reg_write(gsw, SYS_INT_EN, val);
	if (ret < 0)
		return ret;

	if (gsw->model != MT7531) {
		ret = mt753x_reg_read_checked(gsw, MT7530_TOP_SIG_CTRL, &val);
		if (ret < 0)
			return ret;
		val |= TOP_SIG_CTRL_NORMAL;
		ret = mt753x_reg_write(gsw, MT7530_TOP_SIG_CTRL, val);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static void display_port_link_status(struct gsw_mt753x *gsw, u32 port)
{
	u32 pmsr, speed_bits;
	const char *speed;
	int ret;

	mutex_lock(&gsw->reg_mutex);
	ret = mt753x_reg_read_checked(gsw, PMSR(port), &pmsr);
	mutex_unlock(&gsw->reg_mutex);
	if (ret < 0)
		return;

	speed_bits = (pmsr & MAC_SPD_STS_M) >> MAC_SPD_STS_S;

	switch (speed_bits) {
	case MAC_SPD_10:
		speed = "10Mbps";
		break;
	case MAC_SPD_100:
		speed = "100Mbps";
		break;
	case MAC_SPD_1000:
		speed = "1Gbps";
		break;
	case MAC_SPD_2500:
		speed = "2.5Gbps";
		break;
	}

	if (pmsr & MAC_LNK_STS) {
		dev_info(gsw->dev, "Port %d Link is Up - %s/%s\n",
		         port, speed, (pmsr & MAC_DPX_STS) ? "Full" : "Half");
	} else {
		dev_info(gsw->dev, "Port %d Link is Down\n", port);
	}
}

void mt753x_irq_worker(struct work_struct *work)
{
	struct gsw_mt753x *gsw;
	u32 sts, physts, laststs;
	int i, ret;

	gsw = container_of(work, struct gsw_mt753x, irq_worker);

	mutex_lock(&gsw->reg_mutex);
	ret = mt753x_reg_read_checked(gsw, SYS_INT_STS, &sts);
	if (!ret)
		ret = mt753x_reg_write(gsw, SYS_INT_STS, sts);
	mutex_unlock(&gsw->reg_mutex);
	if (ret < 0)
		goto out;

	/* Check for changed PHY link status */
	for (i = 0; i < MT753X_NUM_PHYS; i++) {
		if (!(sts & PHY_LC_INT(i)))
			continue;

		laststs = gsw->phy_link_sts & BIT(i);
		mutex_lock(&gsw->reg_mutex);
		ret = gsw->mii_read(gsw, i, MII_BMSR);
		mutex_unlock(&gsw->reg_mutex);
		if (ret < 0)
			continue;
		physts = !!(ret & BMSR_LSTATUS);
		physts <<= i;

		if (physts ^ laststs) {
			gsw->phy_link_sts ^= BIT(i);
			display_port_link_status(gsw, i);
		}
	}

out:
	enable_irq(gsw->irq);
}
