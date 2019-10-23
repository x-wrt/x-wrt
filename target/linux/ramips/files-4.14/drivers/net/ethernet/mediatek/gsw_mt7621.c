/*   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 2 of the License
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   Copyright (C) 2009-2015 John Crispin <blogic@openwrt.org>
 *   Copyright (C) 2009-2015 Felix Fietkau <nbd@nbd.name>
 *   Copyright (C) 2013-2015 Michael Lee <igvtee@gmail.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>

#include <ralink_regs.h>

#include "mtk_eth_soc.h"
#include "gsw_mt7620.h"

void mtk_switch_w32(struct mt7620_gsw *gsw, u32 val, unsigned reg)
{
	iowrite32(val, gsw->base + reg);
}

u32 mtk_switch_r32(struct mt7620_gsw *gsw, unsigned reg)
{
	return ioread32(gsw->base + reg);
}

static irqreturn_t gsw_interrupt_mt7621(int irq, void *_priv)
{
	struct fe_priv *priv = (struct fe_priv *)_priv;
	struct mt7620_gsw *gsw = (struct mt7620_gsw *)priv->soc->swpriv;
	u32 reg, i;

	reg = mt7530_mdio_r32(gsw, 0x700c);
	mt7530_mdio_w32(gsw, 0x700c, reg);

	for (i = 0; i < 6; i++) {
		if (i == 5 && gsw->p5_intf_sel != P5_INTF_SEL_GMAC5)
			continue;

		if (reg & BIT(i)) {
			unsigned int link;

			link = mt7530_mdio_r32(gsw,
					       0x3008 + (i * 0x100)) & 0x1;

			if (link != priv->link[i]) {
				priv->link[i] = link;
				if (link)
					netdev_info(priv->netdev,
						    "port %d link up\n", i);
				else
					netdev_info(priv->netdev,
						    "port %d link down\n", i);
			}
		}
	}

	mt7620_handle_carrier(priv);

	return IRQ_HANDLED;
}

/* HACK: detect at803x and force at8033 at 1gbit full-duplex */
static void mt7530_detect_at8033(struct mt7620_gsw *gsw, u32 phy_addr)
{
	u32 reg;

	if (gsw->p5_intf_sel == P5_INTF_SEL_GMAC5) {
		/* read phy identifier 1 */
		reg = _mt7620_mii_read(gsw, phy_addr, 0x02);
		if (reg < 0 || reg != 0x004d)
			return;

		/* read phy identifier 2 */
		reg = _mt7620_mii_read(gsw, phy_addr, 0x03);
		if (reg < 0 || reg != 0xd074)
			return;

		/* read chip configure register */
		reg = _mt7620_mii_read(gsw, phy_addr, 0x1f);
		if ((reg & 0x000f) == 0x0002)
			dev_info(gsw->dev,"phy %d at803x mode BX1000, 1Gbit\n",
				 phy_addr);
		else
			dev_info(gsw->dev,
				 "phy %d at803x mode unknown: 0x1F=0x%x\n",
				 phy_addr, reg);

		/* force PHY and port 5 MAC 1gbit, full-duplex */
		_mt7620_mii_write(gsw, phy_addr, 0x00, 0x0140);
		mt7530_mdio_w32(gsw, 0x3500, 0x7e33b);
	}
}

static int mt7530_parse_ports(struct mt7620_gsw *gsw, struct device_node *np)
{
	struct device_node *ports, *port;
	int err, phy_addr = -ENODEV;
	u32 reg;

	gsw->p5_interface = PHY_INTERFACE_MODE_NA;
	gsw->p5_intf_sel = P5_DISABLED;

	ports = of_get_child_by_name(np, "ports");
	if (!ports)
		return phy_addr;

	for_each_available_child_of_node(ports, port) {
		err = of_property_read_u32(port, "reg", &reg);
		if (err)
			continue;

		if (reg != 5)
			break;

		gsw->p5_interface = of_get_phy_mode(port);
		gsw->p5_intf_sel = P5_INTF_SEL_GMAC5;

		err = of_property_read_u32(port, "phy-address", &phy_addr);
		if (err)
			phy_addr = 5;

		if (phy_addr == 0)
			gsw->p5_intf_sel = P5_INTF_SEL_PHY_P0;
		if (phy_addr == 4)
			gsw->p5_intf_sel = P5_INTF_SEL_PHY_P4;
		break;
	}

	return phy_addr;
}

static void mt7530_setup_port5(struct mt7620_gsw *gsw, struct device_node *np)
{
	u8 tx_delay = 0;
	int val, phy_addr;

	phy_addr = mt7530_parse_ports(gsw, np);

	val = mt7530_mdio_r32(gsw, MT7530_MHWTRAP);

	val |= MHWTRAP_MANUAL | MHWTRAP_P5_MAC_SEL | MHWTRAP_P5_DIS;
	val &= ~MHWTRAP_P5_RGMII_MODE & ~MHWTRAP_PHY0_SEL;

	switch (gsw->p5_intf_sel) {
	case P5_INTF_SEL_PHY_P0:
		/* MT7530_P5_MODE_GPHY_P0: 2nd GMAC -> P5 -> P0 */
		val |= MHWTRAP_PHY0_SEL;
		/* fall through */
	case P5_INTF_SEL_PHY_P4:
		/* MT7530_P5_MODE_GPHY_P4: 2nd GMAC -> P5 -> P4 */
		val &= ~MHWTRAP_P5_MAC_SEL & ~MHWTRAP_P5_DIS;

		/* Setup the MAC by default for the cpu port */
		mt7530_mdio_w32(gsw, GSW_REG_PORT_PMCR(5), 0x56300);
		break;
	case P5_INTF_SEL_GMAC5:
		/* MT7530_P5_MODE_GMAC: P5 -> External phy or 2nd GMAC */
		val &= ~MHWTRAP_P5_DIS;
		mt7530_mdio_w32(gsw, GSW_REG_PORT_PMCR(5), 0x56300);
		break;
	case P5_DISABLED:
		gsw->p5_interface = PHY_INTERFACE_MODE_NA;
		break;
	default:
		dev_err(gsw->dev, "Unsupported p5_intf_sel %d\n",
			gsw->p5_intf_sel);
		goto unlock_exit;
	}

	/* Setup RGMII settings */
	if (phy_interface_mode_is_rgmii(gsw->p5_interface)) {
		val |= MHWTRAP_P5_RGMII_MODE;

		/* P5 RGMII RX Clock Control: delay setting for 1000M */
		mt7530_mdio_w32(gsw, MT7530_P5RGMIIRXCR, CSR_RGMII_EDGE_ALIGN);

		/* Don't set delay in DSA mode */
		if ((gsw->p5_intf_sel == P5_INTF_SEL_PHY_P0 ||
		     gsw->p5_intf_sel == P5_INTF_SEL_PHY_P4) &&
		    (gsw->p5_interface == PHY_INTERFACE_MODE_RGMII_TXID ||
		     gsw->p5_interface == PHY_INTERFACE_MODE_RGMII_ID))
			tx_delay = 4; /* n * 0.5 ns */

		/* P5 RGMII TX Clock Control: delay x */
		mt7530_mdio_w32(gsw, MT7530_P5RGMIITXCR,
				CSR_RGMII_TXC_CFG(0x10 + tx_delay));

		/* reduce P5 RGMII Tx driving, 8mA */
		mt7530_mdio_w32(gsw, MT7530_IO_DRV_CR,
				P5_IO_CLK_DRV(1) | P5_IO_DATA_DRV(1));
	}

	mt7530_mdio_w32(gsw, MT7530_MHWTRAP, val);

	dev_info(gsw->dev, "Setup P5, HWTRAP=0x%x, intf_sel=%s, phy-mode=%s\n",
		 val, p5_intf_modes(gsw->p5_intf_sel), phy_modes(gsw->p5_interface));

	/* HACK: Detect at8033 phy on ubnt erx-sfp */
	if (phy_addr == 7)
		mt7530_detect_at8033(gsw, phy_addr);

unlock_exit:
	return;
}

static void mt7621_hw_init(struct mt7620_gsw *gsw, struct device_node *np)
{
	u32 i;
	u32 val;

	/* wardware reset the switch */
	fe_reset(RST_CTRL_MCM);
	mdelay(10);

	/* reduce RGMII2 PAD driving strength */
	rt_sysc_m32(3 << 4, 0, SYSC_PAD_RGMII2_MDIO);

	/* gpio mux - RGMII1=Normal mode */
	rt_sysc_m32(BIT(14), 0, SYSC_GPIO_MODE);

	/* set GMAC1 RGMII mode */
	rt_sysc_m32(3 << 12, 0, SYSC_REG_CFG1);

	/* enable MDIO to control MT7530 */
	rt_sysc_m32(3 << 12, 0, SYSC_GPIO_MODE);

	/* turn off all PHYs */
	for (i = 0; i <= 4; i++) {
		val = _mt7620_mii_read(gsw, i, 0x0);
		val |= BIT(11);
		_mt7620_mii_write(gsw, i, 0x0, val);
	}

	/* reset the switch */
	mt7530_mdio_w32(gsw, 0x7000, 0x3);
	usleep_range(10, 20);

	if ((rt_sysc_r32(SYSC_REG_CHIP_REV_ID) & 0xFFFF) == 0x0101) {
		/* (GE1, Force 1000M/FD, FC ON, MAX_RX_LENGTH 1536) */
		mtk_switch_w32(gsw, 0x2305e30b, GSW_REG_MAC_P0_MCR);
		mt7530_mdio_w32(gsw, 0x3600, 0x5e30b);
	} else {
		/* (GE1, Force 1000M/FD, FC ON, MAX_RX_LENGTH 1536) */
		mtk_switch_w32(gsw, 0x2305e33b, GSW_REG_MAC_P0_MCR);
		mt7530_mdio_w32(gsw, 0x3600, 0x5e33b);
	}

	/* (GE2, Link down) */
	mtk_switch_w32(gsw, 0x8000, GSW_REG_MAC_P1_MCR);

	/* Set switch max RX frame length to 2k */
	mt7530_mdio_w32(gsw, GSW_REG_GMACCR, 0x3F0B);

	/* Enable Port 6, P5 as GMAC5, P5 disable */
	val = mt7530_mdio_r32(gsw, 0x7804);
	val &= ~BIT(8);
	val |= BIT(6) | BIT(13) | BIT(16);
	mt7530_mdio_w32(gsw, 0x7804, val);

	val = rt_sysc_r32(0x10);
	val = (val >> 6) & 0x7;
	if (val >= 6) {
		/* 25Mhz Xtal - do nothing */
	} else if (val >= 3) {
		/* 40Mhz */

		/* disable MT7530 core clock */
		_mt7620_mii_write(gsw, 0, 13, 0x1f);
		_mt7620_mii_write(gsw, 0, 14, 0x410);
		_mt7620_mii_write(gsw, 0, 13, 0x401f);
		_mt7620_mii_write(gsw, 0, 14, 0x0);

		/* disable MT7530 PLL */
		_mt7620_mii_write(gsw, 0, 13, 0x1f);
		_mt7620_mii_write(gsw, 0, 14, 0x40d);
		_mt7620_mii_write(gsw, 0, 13, 0x401f);
		_mt7620_mii_write(gsw, 0, 14, 0x2020);

		/* for MT7530 core clock = 500Mhz */
		_mt7620_mii_write(gsw, 0, 13, 0x1f);
		_mt7620_mii_write(gsw, 0, 14, 0x40e);
		_mt7620_mii_write(gsw, 0, 13, 0x401f);
		_mt7620_mii_write(gsw, 0, 14, 0x119);

		/* enable MT7530 PLL */
		_mt7620_mii_write(gsw, 0, 13, 0x1f);
		_mt7620_mii_write(gsw, 0, 14, 0x40d);
		_mt7620_mii_write(gsw, 0, 13, 0x401f);
		_mt7620_mii_write(gsw, 0, 14, 0x2820);

		usleep_range(20, 40);

		/* enable MT7530 core clock */
		_mt7620_mii_write(gsw, 0, 13, 0x1f);
		_mt7620_mii_write(gsw, 0, 14, 0x410);
		_mt7620_mii_write(gsw, 0, 13, 0x401f);
	} else {
		/* 20Mhz Xtal - TODO */
	}

	/* RGMII */
	_mt7620_mii_write(gsw, 0, 14, 0x1);

	/* set MT7530 central align */
	val = mt7530_mdio_r32(gsw, 0x7830);
	val &= ~BIT(0);
	val |= BIT(1);
	mt7530_mdio_w32(gsw, 0x7830, val);
	val = mt7530_mdio_r32(gsw, 0x7a40);
	val &= ~BIT(30);
	mt7530_mdio_w32(gsw, 0x7a40, val);
	mt7530_mdio_w32(gsw, 0x7a78, 0x855);

	/* lower Tx Driving*/
	mt7530_mdio_w32(gsw, 0x7a54, 0x44);
	mt7530_mdio_w32(gsw, 0x7a5c, 0x44);
	mt7530_mdio_w32(gsw, 0x7a64, 0x44);
	mt7530_mdio_w32(gsw, 0x7a6c, 0x44);
	mt7530_mdio_w32(gsw, 0x7a74, 0x44);
	mt7530_mdio_w32(gsw, 0x7a7c, 0x44);

	/* Setup port 5 */
	mt7530_setup_port5(gsw, np);

	/* turn on all PHYs */
	for (i = 0; i <= 4; i++) {
		val = _mt7620_mii_read(gsw, i, 0);
		val &= ~BIT(11);
		_mt7620_mii_write(gsw, i, 0, val);
	}

	/* enable irq */
	mt7530_mdio_w32(gsw, 0x7008, 0x1f);
	val = mt7530_mdio_r32(gsw, 0x7808);
	val |= 3 << 16;
	mt7530_mdio_w32(gsw, 0x7808, val);
}

static const struct of_device_id mediatek_gsw_match[] = {
	{ .compatible = "mediatek,mt7621-gsw" },
	{},
};
MODULE_DEVICE_TABLE(of, mediatek_gsw_match);

int mtk_gsw_init(struct fe_priv *priv)
{
	struct device_node *np = priv->switch_np;
	struct platform_device *pdev = of_find_device_by_node(np);
	struct mt7620_gsw *gsw;

	if (!pdev)
		return -ENODEV;

	if (!of_device_is_compatible(np, mediatek_gsw_match->compatible))
		return -EINVAL;

	gsw = platform_get_drvdata(pdev);
	priv->soc->swpriv = gsw;

	if (gsw->irq) {
		request_irq(gsw->irq, gsw_interrupt_mt7621, 0,
			    "gsw", priv);
		disable_irq(gsw->irq);
	}

	mt7621_hw_init(gsw, np);

	if (gsw->irq)
		enable_irq(gsw->irq);

	return 0;
}

static int mt7621_gsw_probe(struct platform_device *pdev)
{
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct mt7620_gsw *gsw;

	gsw = devm_kzalloc(&pdev->dev, sizeof(struct mt7620_gsw), GFP_KERNEL);
	if (!gsw)
		return -ENOMEM;

	gsw->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(gsw->base))
		return PTR_ERR(gsw->base);

	gsw->dev = &pdev->dev;
	gsw->irq = platform_get_irq(pdev, 0);

	platform_set_drvdata(pdev, gsw);

	return 0;
}

static int mt7621_gsw_remove(struct platform_device *pdev)
{
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver gsw_driver = {
	.probe = mt7621_gsw_probe,
	.remove = mt7621_gsw_remove,
	.driver = {
		.name = "mt7621-gsw",
		.owner = THIS_MODULE,
		.of_match_table = mediatek_gsw_match,
	},
};

module_platform_driver(gsw_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("John Crispin <blogic@openwrt.org>");
MODULE_DESCRIPTION("GBit switch driver for Mediatek MT7621 SoC");
MODULE_VERSION(MTK_FE_DRV_VERSION);
