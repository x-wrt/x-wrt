// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018 MediaTek Inc.
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/reset.h>
#include <linux/hrtimer.h>
#include <linux/mii.h>
#include <linux/of_mdio.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/of_net.h>
#include <linux/of_irq.h>
#include <linux/phy.h>
#include <linux/platform_device.h>

#include "mt753x.h"
#include "mt753x_swconfig.h"
#include "mt753x_regs.h"
#include "mt753x_nl.h"
#include "mt7530.h"
#include "mt7531.h"

static u32 mt753x_id;
struct list_head mt753x_devs;
static DEFINE_MUTEX(mt753x_devs_lock);

static struct mt753x_sw_id *mt753x_sw_ids[] = {
	&mt7530_id,
	&mt7531_id,
};

int mt753x_reg_read_checked(struct gsw_mt753x *gsw, u32 reg, u32 *value)
{
	int high, low, ret;

	mutex_lock(&gsw->host_bus->mdio_lock);

	ret = gsw->host_bus->write(gsw->host_bus, gsw->smi_addr, 0x1f,
	                           (reg & MT753X_REG_PAGE_ADDR_M) >> MT753X_REG_PAGE_ADDR_S);
	if (ret < 0)
		goto out;

	low = gsw->host_bus->read(gsw->host_bus, gsw->smi_addr,
	                          (reg & MT753X_REG_ADDR_M) >> MT753X_REG_ADDR_S);
	if (low < 0) {
		ret = low;
		goto out;
	}

	high = gsw->host_bus->read(gsw->host_bus, gsw->smi_addr, 0x10);
	if (high < 0) {
		ret = high;
		goto out;
	}

	*value = ((u32)high << 16) | (low & 0xffff);
	ret = 0;
out:
	mutex_unlock(&gsw->host_bus->mdio_lock);

	return ret;
}

u32 mt753x_reg_read(struct gsw_mt753x *gsw, u32 reg)
{
	u32 value = ~0U;

	/* Legacy callers have no separate error return. */
	mt753x_reg_read_checked(gsw, reg, &value);
	return value;
}

int mt753x_reg_write(struct gsw_mt753x *gsw, u32 reg, u32 val)
{
	int ret;

	mutex_lock(&gsw->host_bus->mdio_lock);

	ret = gsw->host_bus->write(gsw->host_bus, gsw->smi_addr, 0x1f,
	                           (reg & MT753X_REG_PAGE_ADDR_M) >> MT753X_REG_PAGE_ADDR_S);
	if (ret < 0)
		goto out;

	ret = gsw->host_bus->write(gsw->host_bus, gsw->smi_addr,
	                           (reg & MT753X_REG_ADDR_M) >> MT753X_REG_ADDR_S, val & 0xffff);
	if (ret < 0)
		goto out;

	ret = gsw->host_bus->write(gsw->host_bus, gsw->smi_addr, 0x10, val >> 16);

out:
	mutex_unlock(&gsw->host_bus->mdio_lock);

	return ret;
}

/* Indirect MDIO clause 22/45 access */
static int mt753x_mii_rw(struct gsw_mt753x *gsw, int phy, int reg, u16 data,
                         u32 cmd, u32 st)
{
	ktime_t timeout;
	u32 val, timeout_us;
	int ret = 0;

	timeout_us = 100000;
	timeout = ktime_add_us(ktime_get(), timeout_us);
	while (1) {
		ret = mt753x_reg_read_checked(gsw, PHY_IAC, &val);
		if (ret < 0)
			return ret;

		if ((val & PHY_ACS_ST) == 0)
			break;

		if (ktime_compare(ktime_get(), timeout) > 0)
			return -ETIMEDOUT;
	}

	val = (st << MDIO_ST_S) |
	      ((cmd << MDIO_CMD_S) & MDIO_CMD_M) |
	      ((phy << MDIO_PHY_ADDR_S) & MDIO_PHY_ADDR_M) |
	      ((reg << MDIO_REG_ADDR_S) & MDIO_REG_ADDR_M);

	if (cmd == MDIO_CMD_WRITE || cmd == MDIO_CMD_ADDR)
		val |= data & MDIO_RW_DATA_M;

	ret = mt753x_reg_write(gsw, PHY_IAC, val | PHY_ACS_ST);
	if (ret < 0)
		return ret;

	timeout_us = 100000;
	timeout = ktime_add_us(ktime_get(), timeout_us);
	while (1) {
		ret = mt753x_reg_read_checked(gsw, PHY_IAC, &val);
		if (ret < 0)
			return ret;

		if ((val & PHY_ACS_ST) == 0)
			break;

		if (ktime_compare(ktime_get(), timeout) > 0)
			return -ETIMEDOUT;
	}

	if (cmd == MDIO_CMD_READ || cmd == MDIO_CMD_READ_C45) {
		ret = mt753x_reg_read_checked(gsw, PHY_IAC, &val);
		if (ret < 0)
			return ret;
		ret = val & MDIO_RW_DATA_M;
	}

	return ret;
}

int mt753x_mii_read(struct gsw_mt753x *gsw, int phy, int reg)
{
	int val;

	if (phy < MT753X_NUM_PHYS)
		phy = (gsw->phy_base + phy) & MT753X_SMI_ADDR_MASK;

	mutex_lock(&gsw->mii_lock);
	val = mt753x_mii_rw(gsw, phy, reg, 0, MDIO_CMD_READ, MDIO_ST_C22);
	mutex_unlock(&gsw->mii_lock);

	return val;
}

int mt753x_mii_write(struct gsw_mt753x *gsw, int phy, int reg, u16 val)
{
	int ret;

	if (phy < MT753X_NUM_PHYS)
		phy = (gsw->phy_base + phy) & MT753X_SMI_ADDR_MASK;

	mutex_lock(&gsw->mii_lock);
	ret = mt753x_mii_rw(gsw, phy, reg, val, MDIO_CMD_WRITE, MDIO_ST_C22);
	mutex_unlock(&gsw->mii_lock);

	return ret;
}

int mt753x_mmd_read(struct gsw_mt753x *gsw, int addr, int devad, u16 reg)
{
	int val;

	if (addr < MT753X_NUM_PHYS)
		addr = (gsw->phy_base + addr) & MT753X_SMI_ADDR_MASK;

	mutex_lock(&gsw->mii_lock);
	val = mt753x_mii_rw(gsw, addr, devad, reg, MDIO_CMD_ADDR, MDIO_ST_C45);
	if (val < 0)
		goto out;
	val = mt753x_mii_rw(gsw, addr, devad, 0, MDIO_CMD_READ_C45,
	                    MDIO_ST_C45);
out:
	mutex_unlock(&gsw->mii_lock);

	return val;
}

int mt753x_mmd_write(struct gsw_mt753x *gsw, int addr, int devad, u16 reg,
                      u16 val)
{
	int ret;

	if (addr < MT753X_NUM_PHYS)
		addr = (gsw->phy_base + addr) & MT753X_SMI_ADDR_MASK;

	mutex_lock(&gsw->mii_lock);
	ret = mt753x_mii_rw(gsw, addr, devad, reg, MDIO_CMD_ADDR, MDIO_ST_C45);
	if (ret < 0)
		goto out;
	ret = mt753x_mii_rw(gsw, addr, devad, val, MDIO_CMD_WRITE, MDIO_ST_C45);
out:
	mutex_unlock(&gsw->mii_lock);

	return ret;
}

int mt753x_mmd_ind_read(struct gsw_mt753x *gsw, int addr, int devad, u16 reg)
{
	int val;

	if (addr < MT753X_NUM_PHYS)
		addr = (gsw->phy_base + addr) & MT753X_SMI_ADDR_MASK;

	mutex_lock(&gsw->mii_lock);

	val = mt753x_mii_rw(gsw, addr, MII_MMD_ACC_CTL_REG,
	                    (MMD_ADDR << MMD_CMD_S) |
	                    ((devad << MMD_DEVAD_S) & MMD_DEVAD_M),
	                    MDIO_CMD_WRITE, MDIO_ST_C22);
	if (val < 0)
		goto out;

	val = mt753x_mii_rw(gsw, addr, MII_MMD_ADDR_DATA_REG, reg,
	                    MDIO_CMD_WRITE, MDIO_ST_C22);
	if (val < 0)
		goto out;

	val = mt753x_mii_rw(gsw, addr, MII_MMD_ACC_CTL_REG,
	                    (MMD_DATA << MMD_CMD_S) |
	                    ((devad << MMD_DEVAD_S) & MMD_DEVAD_M),
	                    MDIO_CMD_WRITE, MDIO_ST_C22);
	if (val < 0)
		goto out;

	val = mt753x_mii_rw(gsw, addr, MII_MMD_ADDR_DATA_REG, 0,
	                    MDIO_CMD_READ, MDIO_ST_C22);

out:
	mutex_unlock(&gsw->mii_lock);

	return val;
}

int mt753x_mmd_ind_write(struct gsw_mt753x *gsw, int addr, int devad, u16 reg,
                          u16 val)
{
	int ret;

	if (addr < MT753X_NUM_PHYS)
		addr = (gsw->phy_base + addr) & MT753X_SMI_ADDR_MASK;

	mutex_lock(&gsw->mii_lock);

	ret = mt753x_mii_rw(gsw, addr, MII_MMD_ACC_CTL_REG,
	                    (MMD_ADDR << MMD_CMD_S) |
	                    ((devad << MMD_DEVAD_S) & MMD_DEVAD_M),
	                    MDIO_CMD_WRITE, MDIO_ST_C22);
	if (ret < 0)
		goto out;

	ret = mt753x_mii_rw(gsw, addr, MII_MMD_ADDR_DATA_REG, reg,
	                    MDIO_CMD_WRITE, MDIO_ST_C22);
	if (ret < 0)
		goto out;

	ret = mt753x_mii_rw(gsw, addr, MII_MMD_ACC_CTL_REG,
	                    (MMD_DATA << MMD_CMD_S) |
	                    ((devad << MMD_DEVAD_S) & MMD_DEVAD_M),
	                    MDIO_CMD_WRITE, MDIO_ST_C22);
	if (ret < 0)
		goto out;

	ret = mt753x_mii_rw(gsw, addr, MII_MMD_ADDR_DATA_REG, val,
	              MDIO_CMD_WRITE, MDIO_ST_C22);

out:
	mutex_unlock(&gsw->mii_lock);

	return ret;
}

static inline int mt753x_get_duplex(const struct device_node *np)
{
	return of_property_read_bool(np, "full-duplex");
}

static void mt753x_load_port_cfg(struct gsw_mt753x *gsw)
{
	struct device_node *port_np;
	struct device_node *fixed_link_node;
	struct mt753x_port_cfg *port_cfg;
	u32 port;

	for_each_child_of_node(gsw->dev->of_node, port_np) {
		if (!of_device_is_compatible(port_np, "mediatek,mt753x-port"))
			continue;

		if (!of_device_is_available(port_np))
			continue;

		if (of_property_read_u32(port_np, "reg", &port))
			continue;

		switch (port) {
		case 5:
			port_cfg = &gsw->port5_cfg;
			break;
		case 6:
			port_cfg = &gsw->port6_cfg;
			break;
		default:
			continue;
		}

		if (port_cfg->enabled) {
			dev_info(gsw->dev, "duplicated node for port%d\n",
			         port_cfg->phy_mode);
			continue;
		}

		port_cfg->np = port_np;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,5,0)
		if (of_get_phy_mode(port_np, &port_cfg->phy_mode) < 0) {
#else
		port_cfg->phy_mode = of_get_phy_mode(port_np);
		if (port_cfg->phy_mode < 0) {
#endif
			dev_info(gsw->dev, "incorrect phy-mode %d\n", port);
			continue;
		}

		fixed_link_node = of_get_child_by_name(port_np, "fixed-link");
		if (fixed_link_node) {
			u32 speed;

			port_cfg->force_link = 1;
			port_cfg->duplex = mt753x_get_duplex(fixed_link_node);

			if (of_property_read_u32(fixed_link_node, "speed",
			                         &speed)) {
				of_node_put(fixed_link_node);
				continue;
			}

			of_node_put(fixed_link_node);

			switch (speed) {
			case 10:
				port_cfg->speed = MAC_SPD_10;
				break;
			case 100:
				port_cfg->speed = MAC_SPD_100;
				break;
			case 1000:
				port_cfg->speed = MAC_SPD_1000;
				break;
			case 2500:
				port_cfg->speed = MAC_SPD_2500;
				break;
			default:
				dev_info(gsw->dev, "incorrect speed %d\n",
				         speed);
				continue;
			}
		}

		port_cfg->enabled = 1;
	}
}

static void mt753x_add_gsw(struct gsw_mt753x *gsw)
{
	mutex_lock(&mt753x_devs_lock);
	gsw->id = mt753x_id++;
	INIT_LIST_HEAD(&gsw->list);
	list_add_tail(&gsw->list, &mt753x_devs);
	mutex_unlock(&mt753x_devs_lock);
}

static void mt753x_remove_gsw(struct gsw_mt753x *gsw)
{
	mutex_lock(&mt753x_devs_lock);
	list_del(&gsw->list);
	mutex_unlock(&mt753x_devs_lock);
}


struct gsw_mt753x *mt753x_get_gsw(u32 id)
{
	struct gsw_mt753x *dev;

	mutex_lock(&mt753x_devs_lock);

	list_for_each_entry(dev, &mt753x_devs, list) {
		if (dev->id == id)
			return dev;
	}

	mutex_unlock(&mt753x_devs_lock);

	return NULL;
}

struct gsw_mt753x *mt753x_get_first_gsw(void)
{
	struct gsw_mt753x *dev;

	mutex_lock(&mt753x_devs_lock);

	list_for_each_entry(dev, &mt753x_devs, list)
	return dev;

	mutex_unlock(&mt753x_devs_lock);

	return NULL;
}

void mt753x_put_gsw(void)
{
	mutex_unlock(&mt753x_devs_lock);
}

void mt753x_lock_gsw(void)
{
	mutex_lock(&mt753x_devs_lock);
}

static int mt753x_hw_reset(struct gsw_mt753x *gsw)
{
	struct device_node *np = gsw->dev->of_node;
	struct reset_control *rstc;
	int mcm;
	int ret = -EINVAL;

	mcm = of_property_read_bool(np, "mediatek,mcm");
	if (mcm) {
		rstc = devm_reset_control_get(gsw->dev, "mcm");
		if (IS_ERR(rstc)) {
			dev_err(gsw->dev, "Missing reset ctrl of switch\n");
			return PTR_ERR(rstc);
		}

		ret = reset_control_assert(rstc);
		if (ret)
			return ret;
		msleep(30);
		ret = reset_control_deassert(rstc);
		if (ret)
			return ret;

		gsw->reset_pin = -1;
		return 0;
	}

	gsw->reset_pin = of_get_named_gpio(np, "reset-gpios", 0);
	if (gsw->reset_pin < 0) {
		dev_err(gsw->dev, "Missing reset pin of switch\n");
		return gsw->reset_pin;
	}

	ret = devm_gpio_request_one(gsw->dev, gsw->reset_pin, GPIOF_OUT_INIT_HIGH, "mt753x-reset");
	if (ret) {
		dev_info(gsw->dev, "Failed to request gpio %d\n",
		         gsw->reset_pin);
		return ret;
	}

	gpio_set_value(gsw->reset_pin, 1);
	msleep(30);
	gpio_set_value(gsw->reset_pin, 0);
	msleep(500);

	return 0;
}

static irqreturn_t mt753x_irq_handler(int irq, void *dev)
{
	struct gsw_mt753x *gsw = dev;

	disable_irq_nosync(gsw->irq);

	schedule_work(&gsw->irq_worker);

	return IRQ_HANDLED;
}

static int mt753x_probe(struct platform_device *pdev)
{
	struct gsw_mt753x *gsw;
	struct mt753x_sw_id *sw;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *mdio;
	struct mii_bus *mdio_bus;
	int ret = -EINVAL;
	struct chip_rev rev;
	struct mt753x_mapping *map;
	int i;

	mdio = of_parse_phandle(np, "mediatek,mdio", 0);
	if (!mdio)
		return -EINVAL;

	mdio_bus = of_mdio_find_bus(mdio);
	of_node_put(mdio);
	if (!mdio_bus)
		return -EPROBE_DEFER;

	gsw = devm_kzalloc(&pdev->dev, sizeof(struct gsw_mt753x), GFP_KERNEL);
	if (!gsw) {
		ret = -ENOMEM;
		goto put_bus;
	}

	gsw->host_bus = mdio_bus;
	gsw->dev = &pdev->dev;
	mutex_init(&gsw->mii_lock);
	mutex_init(&gsw->reg_mutex);

	/* Switch hard reset */
	ret = mt753x_hw_reset(gsw);
	if (ret)
		goto fail;

	/* Fetch the SMI address dirst */
	if (of_property_read_u32(np, "mediatek,smi-addr", &gsw->smi_addr))
		gsw->smi_addr = MT753X_DFL_SMI_ADDR;

	/* Get LAN/WAN port mapping */
	map = mt753x_find_mapping(np);
	if (map) {
		mt753x_apply_mapping(gsw, map);
		gsw->global_vlan_enable = 1;
		dev_info(gsw->dev, "LAN/WAN VLAN setting=%s\n", map->name);
	}

	/* Load MAC port configurations */
	mt753x_load_port_cfg(gsw);

	/* Check for valid switch and then initialize */
	for (i = 0; i < ARRAY_SIZE(mt753x_sw_ids); i++) {
		ret = mt753x_sw_ids[i]->detect(gsw, &rev);
		if (ret < 0)
			goto fail;
		if (!ret) {
			sw = mt753x_sw_ids[i];

			gsw->name = rev.name;
			gsw->model = sw->model;

			dev_info(gsw->dev, "Switch is MediaTek %s rev %d",
			         gsw->name, rev.rev);

			/* Initialize the switch */
			ret = sw->init(gsw);
			if (ret)
				goto fail;

			break;
		}
	}

	if (i >= ARRAY_SIZE(mt753x_sw_ids)) {
		dev_err(gsw->dev, "No mt753x switch found\n");
		ret = -EINVAL;
		goto fail;
	}

	INIT_WORK(&gsw->irq_worker, mt753x_irq_worker);

	gsw->irq = platform_get_irq(pdev, 0);
	if (gsw->irq == -EPROBE_DEFER) {
		ret = gsw->irq;
		goto fail;
	}
	if (gsw->irq >= 0) {
		ret = devm_request_irq(gsw->dev, gsw->irq, mt753x_irq_handler,
		                       0, dev_name(gsw->dev), gsw);
		if (ret) {
			dev_err(gsw->dev, "Failed to request irq %d\n",
			        gsw->irq);
			goto fail;
		}
		/* disable irq before hw init done */
		disable_irq(gsw->irq);
	}

#ifdef CONFIG_SWCONFIG
	ret = mt753x_swconfig_init(gsw);
	if (ret)
		goto fail_irq;
#endif

	if (sw->post_init) {
		ret = sw->post_init(gsw);
		if (ret)
			goto fail_swconfig;
	}

	if (gsw->irq >= 0) {
		ret = mt753x_irq_enable(gsw);
		if (ret < 0)
			goto fail_swconfig;
	}

	platform_set_drvdata(pdev, gsw);

	mt753x_add_gsw(gsw);

	if (gsw->irq >= 0)
		enable_irq(gsw->irq);

	return 0;

fail_swconfig:
#ifdef CONFIG_SWCONFIG
	mt753x_swconfig_destroy(gsw);
fail_irq:
#endif
	if (gsw->irq >= 0) {
		/* The IRQ was disabled before completing hardware initialization. */
		cancel_work_sync(&gsw->irq_worker);
		devm_free_irq(gsw->dev, gsw->irq, gsw);
	}
fail:
	mutex_destroy(&gsw->reg_mutex);
	mutex_destroy(&gsw->mii_lock);
	devm_kfree(&pdev->dev, gsw);
put_bus:
	put_device(&mdio_bus->dev);

	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,12,0)
static void mt753x_remove(struct platform_device *pdev)
#else
static int mt753x_remove(struct platform_device *pdev)
#endif
{
	struct gsw_mt753x *gsw = platform_get_drvdata(pdev);

	/* Drain Netlink users before tearing down the switch resources. */
	mt753x_remove_gsw(gsw);

	if (gsw->irq >= 0) {
		disable_irq(gsw->irq);
		cancel_work_sync(&gsw->irq_worker);
		devm_free_irq(gsw->dev, gsw->irq, gsw);
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6,0,0)
	if (gsw->reset_pin >= 0)
		devm_gpio_free(&pdev->dev, gsw->reset_pin);
#endif

#ifdef CONFIG_SWCONFIG
	mt753x_swconfig_destroy(gsw);
#endif
	mutex_destroy(&gsw->reg_mutex);
	mutex_destroy(&gsw->mii_lock);

	put_device(&gsw->host_bus->dev);

	platform_set_drvdata(pdev, NULL);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,12,0)
#else
	return 0;
#endif
}

static const struct of_device_id mt753x_ids[] = {
	{ .compatible = "mediatek,mt753x" },
	{ },
};

MODULE_DEVICE_TABLE(of, mt753x_ids);

static struct platform_driver mt753x_driver = {
	.probe = mt753x_probe,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,12,0) && LINUX_VERSION_CODE < KERNEL_VERSION(6,13,0)
	.remove_new = mt753x_remove,
#else
	.remove = mt753x_remove,
#endif
	.driver = {
		.name = "mt753x",
		.of_match_table = mt753x_ids,
	},
};

static int __init mt753x_init(void)
{
	int ret;

	INIT_LIST_HEAD(&mt753x_devs);
	ret = platform_driver_register(&mt753x_driver);
	if (ret)
		return ret;

	ret = mt753x_nl_init();
	if (ret)
		platform_driver_unregister(&mt753x_driver);

	return ret;
}
module_init(mt753x_init);

static void __exit mt753x_exit(void)
{
	mt753x_nl_exit();

	platform_driver_unregister(&mt753x_driver);
}
module_exit(mt753x_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Weijie Gao <weijie.gao@mediatek.com>");
MODULE_DESCRIPTION("Driver for MediaTek MT753x Gigabit Switch");
