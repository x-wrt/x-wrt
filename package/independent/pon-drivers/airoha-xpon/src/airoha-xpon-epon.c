// SPDX-License-Identifier: GPL-2.0-only

#include <linux/airoha-eth.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/netdevice.h>
#include <linux/pcs/pcs-airoha.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>

#include "airoha-xpon-epon.h"
#include "airoha-xpon-oam.h"
#include "airoha-xpon-private.h"

/* XEPON PCS occupies the 0x1000 subwindow of the PON PHY resource. */
#define AIROHA_XEPON_PCS_BASE                  0x1000
#define AIROHA_XEPON_PCS_TX_CTRL               (AIROHA_XEPON_PCS_BASE + 0x018)
#define AIROHA_XEPON_PCS_RX_CTRL               (AIROHA_XEPON_PCS_BASE + 0x01c)
#define AIROHA_XEPON_PCS_INT_STATUS            (AIROHA_XEPON_PCS_BASE + 0x020)
#define AIROHA_XEPON_PCS_INT_ENABLE            (AIROHA_XEPON_PCS_BASE + 0x024)
#define AIROHA_XEPON_PCS_LOGIC_RESET           (AIROHA_XEPON_PCS_BASE + 0x034)
#define AIROHA_XEPON_PCS_RX_SYNC_STATUS        (AIROHA_XEPON_PCS_BASE + 0x06c)
#define AIROHA_XEPON_PCS_SFP_STATUS            (AIROHA_XEPON_PCS_BASE + 0x224)
#define AIROHA_XEPON_PCS_ALL_CODEWORDS         (AIROHA_XEPON_PCS_BASE + 0x294)
#define AIROHA_XEPON_PCS_NO_ERROR_CODEWORDS    (AIROHA_XEPON_PCS_BASE + 0x298)
#define AIROHA_XEPON_PCS_CORRECTED_CODEWORDS   (AIROHA_XEPON_PCS_BASE + 0x29c)
#define AIROHA_XEPON_PCS_UNCORRECTED_CODEWORDS (AIROHA_XEPON_PCS_BASE + 0x2a0)
#define AIROHA_XEPON_PCS_ERROR_BYTES           (AIROHA_XEPON_PCS_BASE + 0x2a4)
#define AIROHA_XEPON_PCS_ERROR_BITS            (AIROHA_XEPON_PCS_BASE + 0x2a8)
#define AIROHA_XEPON_PCS_FEC_ERROR_SECONDS     (AIROHA_XEPON_PCS_BASE + 0x2ac)
#define AIROHA_XEPON_PCS_SYNC_OK_COUNT         (AIROHA_XEPON_PCS_BASE + 0x2b8)
#define AIROHA_XEPON_PCS_SYNC_LOSS_COUNT       (AIROHA_XEPON_PCS_BASE + 0x2bc)

/* PHY_CSR_DUMMY_REG_RX maps to PON PHY offset 0x290. */
#define AIROHA_XEPON_PHY_CSR_DUMMY_RX  0x290
#define AIROHA_XEPON_PHY_CONTINUOUS_TX BIT(21)

#define AIROHA_XEPON_PCS_RX_ENABLE         0x80810302
#define AIROHA_XEPON_PCS_RX_DISABLE        0x80810300
#define AIROHA_XEPON_PCS_SYNC_OK           BIT(31)
#define AIROHA_XEPON_PCS_INT_SYNC_OK       BIT(31)
#define AIROHA_XEPON_PCS_INT_SYNC_LOSS     BIT(30)
#define AIROHA_XEPON_PCS_INT_LASER_LOSS    BIT(24)
#define AIROHA_XEPON_PCS_INT_NO_LASER_LOSS BIT(5)
#define AIROHA_XEPON_PCS_SFP_RX_LOS        BIT(28)
#define AIROHA_XEPON_PCS_INT_RX_MASK                                     \
	(AIROHA_XEPON_PCS_INT_SYNC_OK | AIROHA_XEPON_PCS_INT_SYNC_LOSS | \
	 AIROHA_XEPON_PCS_INT_LASER_LOSS | AIROHA_XEPON_PCS_INT_NO_LASER_LOSS)

/* XEPON_1G pairs the 10G MAC with the 1.25G GEPON PHY block. */
#define AIROHA_GEPON_PHYSET10              0x124
#define AIROHA_GEPON_PHYSTA1               0x130
#define AIROHA_GEPON_XPON_SETTING          0x138
#define AIROHA_GEPON_ERROR_BYTES           0x238
#define AIROHA_GEPON_ERROR_CODEWORDS       0x23c
#define AIROHA_GEPON_UNCORRECTED_CODEWORDS 0x240
#define AIROHA_GEPON_ALL_CODEWORDS         0x244
#define AIROHA_GEPON_FEC_ERROR_SECONDS     0x248
#define AIROHA_GEPON_SYNC_LOSS_COUNT       0x258
#define AIROHA_GEPON_XPON_STA              0x5e0
#define AIROHA_GEPON_INT_ENABLE            0x5f0
#define AIROHA_GEPON_INT_CLEAR             0x5f4
#define AIROHA_GEPON_INT_STATUS            0x5f8
#define AIROHA_GEPON_GPON_MODE             BIT(31)
#define AIROHA_GEPON_PHY_READY             GENMASK(20, 18)
#define AIROHA_GEPON_PHY_READY_VALUE       6
#define AIROHA_GEPON_STA_LOS               BIT(0)
#define AIROHA_GEPON_INT_LOS               BIT(0)
#define AIROHA_GEPON_INT_LOF               BIT(1)
#define AIROHA_GEPON_INT_PHY_READY         BIT(5)
#define AIROHA_GEPON_INT_NO_LOS            BIT(8)
#define AIROHA_GEPON_INT_RX_MASK                       \
	(AIROHA_GEPON_INT_LOS | AIROHA_GEPON_INT_LOF | \
	 AIROHA_GEPON_INT_PHY_READY | AIROHA_GEPON_INT_NO_LOS)

/* epon_base starts at XEPON_MAC_BASE + 0x6000. */
#define AIROHA_EPON_GLB_CFG                0x000
#define AIROHA_EPON_GLB_CFG2               0x004
#define AIROHA_EPON_INT_STATUS             0x010
#define AIROHA_EPON_INT_ENABLE             0x014
#define AIROHA_EPON_INT_STATUS2            0x018
#define AIROHA_EPON_INT_ENABLE2            0x01c
#define AIROHA_EPON_INT_STATUS3            0x020
#define AIROHA_EPON_INT_ENABLE3            0x024
#define AIROHA_EPON_LLID_DISCOVERY_CTRL    0x07c
#define AIROHA_EPON_LLID0_DISCOVERY_STATUS 0x080
#define AIROHA_EPON_MAC_ADDR_CFG           0x104
#define AIROHA_EPON_MAC_ADDR_VALUE         0x108
#define AIROHA_EPON_REPORT_CFG             0x124
#define AIROHA_EPON_REPORT_BITMAP          0x134
#define AIROHA_EPON_SYNC_TIME              0x1c4
#define AIROHA_EPON_MPCP_TIMEOUT_INTERVAL  0x1d8
#define AIROHA_EPON_TRX_ADJUST_TIME1       0x1e8
#define AIROHA_EPON_TRX_ADJUST_TIME2       0x1ec
#define AIROHA_EPON_TRX_ADJUST_TIME3       0x1f0
#define AIROHA_EPON_TRX_ADJUST_TIME4       0x1f4
#define AIROHA_EPON_TX_FETCH_CFG           0x200
#define AIROHA_EPON_TX_CAL_CONST           0x204
#define AIROHA_EPON_RX_OAM_COUNT           0x5ac
#define AIROHA_EPON_RX_MPCP_COUNT          0x5b0
#define AIROHA_EPON_TX_MPCP_COUNT          0x5e4

#define AIROHA_EPON_GLB_STOP_MASK       (BIT(8) | BIT(9) | BIT(12) | BIT(13))
#define AIROHA_EPON_GLB_DISCOVERY_BURST BIT(23)
#define AIROHA_EPON_GLB2_U10G_TX_MODE   BIT(16)

#define AIROHA_EPON_MAC_ADDR_WRITE BIT(31)
#define AIROHA_EPON_MAC_ADDR_BUSY  BIT(16)
#define AIROHA_EPON_MAC_ADDR_DWORD BIT(0)

#define AIROHA_EPON_INT_DISCOVERY_GATE    BIT(0)
#define AIROHA_EPON_INT_LLID0_REGISTER    BIT(1)
#define AIROHA_EPON_INT_MPCP_TIMEOUT      BIT(14)
#define AIROHA_EPON_INT_REGISTER_REQ_DONE BIT(24)
#define AIROHA_EPON_INT_REGISTER_ACK_DONE BIT(25)
#define AIROHA_EPON_INT_LINE_MASK                                           \
	(AIROHA_EPON_INT_DISCOVERY_GATE | AIROHA_EPON_INT_LLID0_REGISTER |  \
	 AIROHA_EPON_INT_MPCP_TIMEOUT | AIROHA_EPON_INT_REGISTER_REQ_DONE | \
	 AIROHA_EPON_INT_REGISTER_ACK_DONE)

#define AIROHA_EPON_DISCOVERY_LLID           GENMASK(4, 0)
#define AIROHA_EPON_DISCOVERY_REQ_FLAG       BIT(8)
#define AIROHA_EPON_DISCOVERY_ACK_FLAG       BIT(12)
#define AIROHA_EPON_DISCOVERY_CMD_DONE       BIT(16)
#define AIROHA_EPON_DISCOVERY_COMMAND        GENMASK(31, 30)
#define AIROHA_EPON_COMMAND_REGISTER_REQUEST 1
#define AIROHA_EPON_COMMAND_NORMAL_REQUEST   2
#define AIROHA_EPON_COMMAND_REGISTER_ACK     3

#define AIROHA_EPON_LLID_VALUE          GENMASK(15, 0)
#define AIROHA_EPON_LLID_VALID          BIT(16)
#define AIROHA_EPON_REGISTER_FLAG       GENMASK(25, 24)
#define AIROHA_EPON_DISCOVERY_STATE     GENMASK(31, 30)
#define AIROHA_EPON_REGISTER_REREGISTER 0
#define AIROHA_EPON_REGISTER_DEREGISTER 1
#define AIROHA_EPON_REGISTER_ACK        2
#define AIROHA_EPON_REGISTER_NACK       3
#define AIROHA_EPON_STATE_UNREGISTERED  0
#define AIROHA_EPON_STATE_REGISTERING   1
#define AIROHA_EPON_STATE_REGISTERED    2

#define AIROHA_EPON_DISCOVERY_SYNC_UPDATE BIT(24)
#define AIROHA_EPON_DISCOVERY_RETRY_COUNT 3
#define AIROHA_EPON_NACK_SILENT_MS        60000
/* The standard MAC path starts at 0x20; OLT values are capped at 0x5f. */
#define AIROHA_EPON_SYNC_TIME_DEFAULT    0x20
#define AIROHA_EPON_SYNC_TIME_MAX        0x5f
#define AIROHA_EPON_TX_STAMP_ADJUST_BASE 0x002ffff1

#define AIROHA_SCU_RSTCTRL1         0x834
#define AIROHA_SCU_PON_MAC_RESET    BIT(31)
#define AIROHA_SCU_SSR3             0x94
#define AIROHA_SCU_EPON_LOGIC_RESET BIT(10)

static u32 airoha_epon_read(struct airoha_xpon *xpon, u32 reg)
{
	return readl(xpon->epon_base + reg);
}

static void airoha_epon_write(struct airoha_xpon *xpon, u32 reg, u32 value)
{
	writel(value, xpon->epon_base + reg);
}

static void airoha_epon_update_bits(struct airoha_xpon *xpon, u32 reg, u32 mask,
                                    u32 value)
{
	u32 old_value = airoha_epon_read(xpon, reg);

	airoha_epon_write(xpon, reg, (old_value & ~mask) | (value & mask));
}

bool airoha_xpon_mode_is_epon(enum airoha_xpon_mode mode)
{
	return mode == AIROHA_XPON_MODE_EPON_1G ||
	       mode == AIROHA_XPON_MODE_EPON_10G_1G ||
	       mode == AIROHA_XPON_MODE_EPON_10G_10G;
}

static enum airoha_pcs_pon_mode
airoha_xpon_epon_pcs_mode(struct airoha_xpon *xpon)
{
	if (xpon->active_mode == AIROHA_XPON_MODE_EPON_1G)
		return AIROHA_PCS_PON_MODE_EPON_1G;
	if (xpon->active_mode == AIROHA_XPON_MODE_EPON_10G_10G)
		return AIROHA_PCS_PON_MODE_EPON_10G_10G;
	return AIROHA_PCS_PON_MODE_EPON_10G_1G;
}

static bool airoha_xpon_epon_is_1g(struct airoha_xpon *xpon)
{
	return xpon->active_mode == AIROHA_XPON_MODE_EPON_1G;
}

bool airoha_xpon_epon_pcs_synced(struct airoha_xpon *xpon)
{
	if (airoha_xpon_epon_is_1g(xpon))
		return FIELD_GET(AIROHA_GEPON_PHY_READY,
		                 airoha_pon_phy_read(xpon,
		                                     AIROHA_GEPON_PHYSTA1)) ==
		       AIROHA_GEPON_PHY_READY_VALUE;

	return airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_RX_SYNC_STATUS) &
	       AIROHA_XEPON_PCS_SYNC_OK;
}

void airoha_xpon_epon_debug_pcs(struct airoha_xpon *xpon, struct seq_file *m)
{
	if (airoha_xpon_epon_is_1g(xpon)) {
		seq_printf(
			m,
			"phy_status_raw: 0x%08x\n"
			"xpon_status_raw: 0x%08x\n"
			"interrupt_status: 0x%08x\n"
			"interrupt_enable: 0x%08x\n"
			"error_bytes: %u\n"
			"error_codewords: %u\n"
			"uncorrected_codewords: %u\n"
			"all_codewords: %u\n"
			"fec_error_seconds: %u\n"
			"sync_loss_count_hw: %u\n",
			airoha_pon_phy_read(xpon, AIROHA_GEPON_PHYSTA1),
			airoha_pon_phy_read(xpon, AIROHA_GEPON_XPON_STA),
			airoha_pon_phy_read(xpon, AIROHA_GEPON_INT_STATUS),
			airoha_pon_phy_read(xpon, AIROHA_GEPON_INT_ENABLE),
			airoha_pon_phy_read(xpon, AIROHA_GEPON_ERROR_BYTES),
			airoha_pon_phy_read(xpon, AIROHA_GEPON_ERROR_CODEWORDS),
			airoha_pon_phy_read(xpon,
		                            AIROHA_GEPON_UNCORRECTED_CODEWORDS),
			airoha_pon_phy_read(xpon, AIROHA_GEPON_ALL_CODEWORDS),
			airoha_pon_phy_read(xpon,
		                            AIROHA_GEPON_FEC_ERROR_SECONDS),
			airoha_pon_phy_read(xpon,
		                            AIROHA_GEPON_SYNC_LOSS_COUNT));
		return;
	}

	seq_printf(
		m,
		"rx_control_raw: 0x%08x\n"
		"sync_status_raw: 0x%08x\n"
		"interrupt_status: 0x%08x\n"
		"interrupt_enable: 0x%08x\n"
		"sfp_status_raw: 0x%08x\n"
		"all_codewords: %u\n"
		"no_error_codewords: %u\n"
		"corrected_codewords: %u\n"
		"uncorrected_codewords: %u\n"
		"error_bytes: %u\n"
		"error_bits: %u\n"
		"fec_error_seconds: %u\n"
		"sync_ok_count_hw: %u\n"
		"sync_loss_count_hw: %u\n",
		airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_RX_CTRL),
		airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_RX_SYNC_STATUS),
		airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_INT_STATUS),
		airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_INT_ENABLE),
		airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_SFP_STATUS),
		airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_ALL_CODEWORDS),
		airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_NO_ERROR_CODEWORDS),
		airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_CORRECTED_CODEWORDS),
		airoha_pon_phy_read(xpon,
	                            AIROHA_XEPON_PCS_UNCORRECTED_CODEWORDS),
		airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_ERROR_BYTES),
		airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_ERROR_BITS),
		airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_FEC_ERROR_SECONDS),
		airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_SYNC_OK_COUNT),
		airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_SYNC_LOSS_COUNT));
}

void airoha_xpon_epon_debug_mac(struct airoha_xpon *xpon, struct seq_file *m)
{
	/* Active-line registers require EPON MAC initialization first. */
	seq_printf(m,
	           "interrupt_status: 0x%08x\n"
	           "interrupt_enable: 0x%08x\n"
	           "error_status: 0x%08x\n"
	           "error_enable: 0x%08x\n"
	           "global_config: 0x%08x\n"
	           "global_config2: 0x%08x\n"
	           "discovery_control: 0x%08x\n"
	           "llid0_status: 0x%08x\n"
	           "sync_time: 0x%04x\n"
	           "rx_mpcp: %u\n"
	           "tx_mpcp: %u\n"
	           "rx_oam: %u\n"
	           "mac_errors: %u\n"
	           "last_mac_error: 0x%08x\n",
	           airoha_epon_read(xpon, AIROHA_EPON_INT_STATUS),
	           airoha_epon_read(xpon, AIROHA_EPON_INT_ENABLE),
	           airoha_epon_read(xpon, AIROHA_EPON_INT_STATUS2),
	           airoha_epon_read(xpon, AIROHA_EPON_INT_ENABLE2),
	           airoha_epon_read(xpon, AIROHA_EPON_GLB_CFG),
	           airoha_epon_read(xpon, AIROHA_EPON_GLB_CFG2),
	           airoha_epon_read(xpon, AIROHA_EPON_LLID_DISCOVERY_CTRL),
	           airoha_epon_read(xpon, AIROHA_EPON_LLID0_DISCOVERY_STATUS),
	           airoha_epon_read(xpon, AIROHA_EPON_SYNC_TIME) & 0xffff,
	           airoha_epon_read(xpon, AIROHA_EPON_RX_MPCP_COUNT),
	           airoha_epon_read(xpon, AIROHA_EPON_TX_MPCP_COUNT),
	           airoha_epon_read(xpon, AIROHA_EPON_RX_OAM_COUNT),
	           xpon->epon.mac_error_count, xpon->epon.last_mac_error);
}

static void airoha_xpon_epon_set_discovery_state(struct airoha_xpon *xpon,
                                                 u32 state)
{
	airoha_epon_update_bits(xpon, AIROHA_EPON_LLID0_DISCOVERY_STATUS,
	                        AIROHA_EPON_DISCOVERY_STATE,
	                        FIELD_PREP(AIROHA_EPON_DISCOVERY_STATE, state));
}

static int airoha_xpon_epon_wait_mac_table(struct airoha_xpon *xpon)
{
	u32 value;

	return readl_poll_timeout_atomic(
		xpon->epon_base + AIROHA_EPON_MAC_ADDR_CFG, value,
		!(value & AIROHA_EPON_MAC_ADDR_BUSY), 1, 100);
}

static int airoha_xpon_epon_program_mac(struct airoha_xpon *xpon)
{
	const u8 *mac = xpon->pon_netdev->dev_addr;
	u32 high = (u32)mac[0] << 8 | mac[1];
	u32 low = (u32)mac[2] << 24 | (u32)mac[3] << 16 | (u32)mac[4] << 8 |
	          mac[5];

	int ret;

	ret = airoha_xpon_epon_wait_mac_table(xpon);
	if (ret)
		return ret;

	/* Address-table dw_idx 0/1 select the low 32 and high 16 MAC bits. */
	airoha_epon_write(xpon, AIROHA_EPON_MAC_ADDR_VALUE, low);
	airoha_epon_write(xpon, AIROHA_EPON_MAC_ADDR_CFG,
	                  AIROHA_EPON_MAC_ADDR_WRITE);
	ret = airoha_xpon_epon_wait_mac_table(xpon);
	if (ret)
		return ret;

	airoha_epon_write(xpon, AIROHA_EPON_MAC_ADDR_VALUE, high);
	airoha_epon_write(xpon, AIROHA_EPON_MAC_ADDR_CFG,
	                  AIROHA_EPON_MAC_ADDR_WRITE |
	                          AIROHA_EPON_MAC_ADDR_DWORD);

	return airoha_xpon_epon_wait_mac_table(xpon);
}

static void airoha_xpon_epon_reset_registration(struct airoha_xpon *xpon)
{
	if (xpon->epon.registered_since) {
		xpon->epon.last_registration_lifetime =
			jiffies - xpon->epon.registered_since;
		xpon->epon.registered_since = 0;
	}
	airoha_xpon_oam_set_link(xpon->oam, false, 0, 0);
	airoha_xpon_set_data_path_link(xpon, false);
	airoha_eth_pon_configure_epon(xpon->pon_netdev, 0, false);
	xpon->service_ready = false;
	xpon->data_path.count = 0;
	xpon->data_path.configured = false;
	xpon->epon.llid = 0;
	xpon->epon.llid_valid = false;
	xpon->epon.discovery_retry = 0;
	xpon->epon.denied_until = 0;
	xpon->epon.mpcp_state = AIROHA_EPON_MPCP_REGISTERING;
	xpon->lifecycle = AIROHA_XPON_PROTOCOL_ACTIVATING;
	airoha_xpon_epon_set_discovery_state(xpon,
	                                     AIROHA_EPON_STATE_UNREGISTERED);
	airoha_xpon_epon_set_discovery_state(xpon,
	                                     AIROHA_EPON_STATE_REGISTERING);
	airoha_xpon_leds_update(xpon);
}

static void airoha_xpon_epon_enter_denied(struct airoha_xpon *xpon)
{
	airoha_xpon_epon_reset_registration(xpon);
	xpon->epon.mpcp_state = AIROHA_EPON_MPCP_DENIED;
	xpon->epon.denied_until =
		jiffies + msecs_to_jiffies(AIROHA_EPON_NACK_SILENT_MS);
}

static void airoha_xpon_epon_send_deregister(struct airoha_xpon *xpon)
{
	u32 command;

	if (xpon->epon.mpcp_state != AIROHA_EPON_MPCP_REGISTERED)
		return;

	command = FIELD_PREP(AIROHA_EPON_DISCOVERY_LLID, xpon->epon.channel) |
	          AIROHA_EPON_DISCOVERY_REQ_FLAG |
	          FIELD_PREP(AIROHA_EPON_DISCOVERY_COMMAND,
	                     AIROHA_EPON_COMMAND_NORMAL_REQUEST);
	airoha_epon_write(xpon, AIROHA_EPON_LLID_DISCOVERY_CTRL, command);
}

static int airoha_xpon_epon_start_mac(struct airoha_xpon *xpon)
{
	u32 tx_cal;
	int ret;

	/* epon_mac_reset() pulses RSTCTRL1[31] before holding SSR3[10]. */
	regmap_set_bits(xpon->scu, AIROHA_SCU_RSTCTRL1,
	                AIROHA_SCU_PON_MAC_RESET);
	udelay(1);
	regmap_clear_bits(xpon->scu, AIROHA_SCU_RSTCTRL1,
	                  AIROHA_SCU_PON_MAC_RESET);
	regmap_set_bits(xpon->scu, AIROHA_SCU_SSR3,
	                AIROHA_SCU_EPON_LOGIC_RESET);
	udelay(1);

	airoha_epon_write(xpon, AIROHA_EPON_TRX_ADJUST_TIME1, 0x0000fff6);
	airoha_epon_write(xpon, AIROHA_EPON_TRX_ADJUST_TIME2, 0x00000006);
	airoha_epon_write(xpon, AIROHA_EPON_TX_FETCH_CFG, 0x002a03e8);
	xpon->epon.sync_time = AIROHA_EPON_SYNC_TIME_DEFAULT;

	/* PHY readiness gates EPON MAC logic reset release. */
	regmap_clear_bits(xpon->scu, AIROHA_SCU_SSR3,
	                  AIROHA_SCU_EPON_LOGIC_RESET);
	udelay(1);

	airoha_epon_write(xpon, AIROHA_EPON_INT_ENABLE, 0);
	airoha_epon_write(xpon, AIROHA_EPON_INT_ENABLE2, 0);
	airoha_epon_write(xpon, AIROHA_EPON_INT_ENABLE3, 0);
	airoha_epon_write(xpon, AIROHA_EPON_INT_STATUS, U32_MAX);
	airoha_epon_write(xpon, AIROHA_EPON_INT_STATUS2, 0x1fff);
	airoha_epon_write(xpon, AIROHA_EPON_INT_STATUS3, U32_MAX);
	atomic_set(&xpon->epon.pending_events, 0);
	atomic_set(&xpon->epon.pending_error_events, 0);
	xpon->epon.discovery_seen = false;

	airoha_epon_update_bits(xpon, AIROHA_EPON_GLB_CFG,
	                        AIROHA_EPON_GLB_DISCOVERY_BURST |
	                                AIROHA_EPON_GLB_STOP_MASK,
	                        AIROHA_EPON_GLB_DISCOVERY_BURST);
	airoha_epon_update_bits(
		xpon, AIROHA_EPON_GLB_CFG2, AIROHA_EPON_GLB2_U10G_TX_MODE,
		xpon->active_mode == AIROHA_XPON_MODE_EPON_10G_10G ?
			AIROHA_EPON_GLB2_U10G_TX_MODE :
			0);
	/*
	 * Clearing PHY_CSR_DUMMY_REG_RX[21] selects grant-controlled burst mode.
	 * The frontend holds BEN and board TX_DISABLE until line activation, so
	 * the PHY mode can be selected before the first Discovery Gate.
	 */
	airoha_pon_phy_update_bits(xpon, AIROHA_XEPON_PHY_CSR_DUMMY_RX,
	                           AIROHA_XEPON_PHY_CONTINUOUS_TX, 0);

	/* 10G EPON reports across 16 queue bits with report configuration 1. */
	airoha_epon_write(xpon, AIROHA_EPON_REPORT_BITMAP, 0xffff);
	airoha_epon_write(xpon, AIROHA_EPON_REPORT_CFG, 1);
	/*
	 * Bit 24 lets the first Discovery Gate capture the OLT sync_time. Clear
	 * it after REGISTER_REQ_DONE to retain the learned value.
	 */
	airoha_epon_write(xpon, AIROHA_EPON_MPCP_TIMEOUT_INTERVAL,
	                  AIROHA_EPON_DISCOVERY_SYNC_UPDATE | 0x1f4);
	tx_cal = airoha_epon_read(xpon, AIROHA_EPON_TX_CAL_CONST);
	airoha_epon_write(xpon, AIROHA_EPON_TX_CAL_CONST,
	                  (tx_cal & ~GENMASK(5, 0)) | 8);
	if (airoha_xpon_epon_is_1g(xpon)) {
		airoha_epon_update_bits(xpon, AIROHA_EPON_TRX_ADJUST_TIME2,
		                        GENMASK(15, 0), 0x18);
	} else {
		airoha_epon_update_bits(xpon, AIROHA_EPON_TRX_ADJUST_TIME4,
		                        GENMASK(31, 16), 0xff90 << 16);
	}
	if (xpon->active_mode == AIROHA_XPON_MODE_EPON_10G_10G)
		airoha_epon_update_bits(xpon, AIROHA_EPON_TRX_ADJUST_TIME3,
		                        GENMASK(15, 0), 0x8);

	ret = airoha_xpon_epon_program_mac(xpon);
	if (ret)
		return ret;
	xpon->epon.channel = 0;
	airoha_xpon_epon_reset_registration(xpon);
	/* MPCP grants control BEN while the MAC is in burst mode. */
	ret = airoha_pon_frontend_set_tx_enable(xpon->frontend, true);
	if (ret)
		return ret;
	xpon->tx_armed = true;

	airoha_epon_write(xpon, AIROHA_EPON_INT_ENABLE2, 0x1fff);
	airoha_epon_write(xpon, AIROHA_EPON_INT_ENABLE,
	                  AIROHA_EPON_INT_LINE_MASK);
	xpon->mac_initialized = true;

	dev_info(xpon->dev,
	         "%s MAC started; LLID0 is waiting for a Discovery Gate\n",
	         airoha_xpon_mode_name(xpon->active_mode));
	return 0;
}

static int airoha_xpon_epon_start_1g_rx(struct airoha_xpon *xpon, bool retrain)
{
	int ret;

	airoha_pon_phy_write(xpon, AIROHA_GEPON_INT_ENABLE, 0);
	airoha_pon_phy_update_bits(xpon, AIROHA_GEPON_PHYSET10,
	                           AIROHA_GEPON_GPON_MODE, 0);
	airoha_pon_phy_write(xpon, AIROHA_GEPON_XPON_SETTING,
	                     xpon->frontend_link.gepon_xpon_setting);

	if (retrain) {
		ret = airoha_pcs_pon_retrain_rx(xpon->pcs);
	} else {
		ret = airoha_pcs_pon_config_rx(
			xpon->pcs, AIROHA_PCS_PON_MODE_EPON_1G,
			xpon->frontend_link.pma_xpon_setting0,
			xpon->frontend_link.pma_xpon_setting1);
		if (!ret) {
			xpon->pcs_profile = AIROHA_PCS_PON_MODE_EPON_1G;
			xpon->pcs_profile_valid = true;
		}
	}
	if (ret)
		return ret;

	airoha_pon_phy_write(xpon, AIROHA_GEPON_INT_CLEAR, U32_MAX);
	airoha_pon_phy_write(xpon, AIROHA_GEPON_INT_ENABLE,
	                     AIROHA_GEPON_INT_RX_MASK);

	xpon->epon.pcs_synced = false;
	xpon->epon.sync_loss_since = 0;
	xpon->rx_active = true;
	xpon->stats.rx_start_count++;
	xpon->lifecycle = AIROHA_XPON_WAIT_LINE_SYNC;
	return 0;
}

int airoha_xpon_epon_start_rx(struct airoha_xpon *xpon)
{
	enum airoha_pcs_pon_mode pcs_mode = airoha_xpon_epon_pcs_mode(xpon);
	bool retrain = xpon->pcs_profile_valid && xpon->pcs_profile == pcs_mode;
	int ret;

	if (airoha_xpon_epon_is_1g(xpon))
		return airoha_xpon_epon_start_1g_rx(xpon, retrain);

	airoha_pon_phy_write(xpon, AIROHA_XEPON_PCS_INT_ENABLE, 0);
	airoha_pon_phy_write(xpon, AIROHA_XEPON_PCS_RX_CTRL,
	                     AIROHA_XEPON_PCS_RX_DISABLE);
	airoha_pon_phy_write(xpon, AIROHA_XEPON_PCS_LOGIC_RESET, 0);
	usleep_range(1000, 1200);

	if (retrain) {
		ret = airoha_pcs_pon_retrain_rx(xpon->pcs);
	} else {
		ret = airoha_pcs_pon_config_rx(
			xpon->pcs, pcs_mode,
			xpon->frontend_link.pma_xpon_setting0,
			xpon->frontend_link.pma_xpon_setting1);
		if (!ret) {
			xpon->pcs_profile = pcs_mode;
			xpon->pcs_profile_valid = true;
		}
	}
	if (ret)
		return ret;

	/*
	 * config_rx()/retrain_rx() reset XFI, PCS, and SFP_STATUS polarity.
	 * Restore the frontend's board-specific BEN polarity before releasing PCS;
	 * MPCP grants and the frontend TX gate then jointly constrain bursts.
	 */
	airoha_pon_phy_write(xpon, AIROHA_XEPON_PCS_SFP_STATUS,
	                     xpon->frontend_link.xepon_sfp_status);
	airoha_pon_phy_write(xpon, AIROHA_XEPON_PCS_LOGIC_RESET, 1);
	usleep_range(1000, 1200);
	airoha_pon_phy_write(xpon, AIROHA_XEPON_PCS_INT_STATUS, U32_MAX);
	airoha_pon_phy_write(xpon, AIROHA_XEPON_PCS_INT_ENABLE,
	                     AIROHA_XEPON_PCS_INT_RX_MASK);
	airoha_pon_phy_write(xpon, AIROHA_XEPON_PCS_RX_CTRL,
	                     AIROHA_XEPON_PCS_RX_ENABLE);

	xpon->epon.pcs_synced = false;
	xpon->epon.sync_loss_since = 0;
	xpon->rx_active = true;
	xpon->stats.rx_start_count++;
	xpon->lifecycle = AIROHA_XPON_WAIT_LINE_SYNC;
	return 0;
}

void airoha_xpon_epon_stop_rx(struct airoha_xpon *xpon)
{
	airoha_pon_frontend_set_tx_enable(xpon->frontend, false);
	xpon->tx_armed = false;
	airoha_epon_write(xpon, AIROHA_EPON_INT_ENABLE, 0);
	airoha_epon_write(xpon, AIROHA_EPON_INT_ENABLE2, 0);
	airoha_epon_write(xpon, AIROHA_EPON_INT_ENABLE3, 0);
	if (airoha_xpon_epon_is_1g(xpon)) {
		airoha_pon_phy_write(xpon, AIROHA_GEPON_INT_ENABLE, 0);
	} else {
		airoha_pon_phy_write(xpon, AIROHA_XEPON_PCS_INT_ENABLE, 0);
		airoha_pon_phy_write(xpon, AIROHA_XEPON_PCS_RX_CTRL,
		                     AIROHA_XEPON_PCS_RX_DISABLE);
	}
	regmap_set_bits(xpon->scu, AIROHA_SCU_SSR3,
	                AIROHA_SCU_EPON_LOGIC_RESET);
	airoha_pcs_pon_stop_rx(xpon->pcs);
	airoha_xpon_epon_reset_registration(xpon);
	xpon->epon.pcs_synced = false;
	xpon->epon.sync_loss_since = 0;
	xpon->mac_initialized = false;
	xpon->rx_active = false;
}

void airoha_xpon_epon_mask_irqs(struct airoha_xpon *xpon)
{
	airoha_epon_write(xpon, AIROHA_EPON_INT_ENABLE, 0);
	airoha_epon_write(xpon, AIROHA_EPON_INT_ENABLE2, 0);
	airoha_epon_write(xpon, AIROHA_EPON_INT_ENABLE3, 0);
	if (airoha_xpon_epon_is_1g(xpon))
		airoha_pon_phy_write(xpon, AIROHA_GEPON_INT_ENABLE, 0);
	else
		airoha_pon_phy_write(xpon, AIROHA_XEPON_PCS_INT_ENABLE, 0);
	atomic_set(&xpon->epon.pending_events, 0);
	atomic_set(&xpon->epon.pending_error_events, 0);
}

bool airoha_xpon_epon_los(struct airoha_xpon *xpon)
{
	if (airoha_xpon_epon_is_1g(xpon))
		return airoha_pon_phy_read(xpon, AIROHA_GEPON_XPON_STA) &
		       AIROHA_GEPON_STA_LOS;

	return !!(airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_SFP_STATUS) &
	          AIROHA_XEPON_PCS_SFP_RX_LOS);
}

static void airoha_xpon_epon_apply_sync_time(struct airoha_xpon *xpon)
{
	u16 observed, effective;

	/*
	 * A Discovery Gate updates the MAC sync_time. A zero OLT value keeps the
	 * current epoch's value; a valid OLT value seeds later Gates.
	 */
	observed = airoha_epon_read(xpon, AIROHA_EPON_SYNC_TIME) & 0xffff;
	if (observed == 0)
		effective = xpon->epon.sync_time;
	else
		effective = min_t(u16, observed, AIROHA_EPON_SYNC_TIME_MAX);

	if (effective != observed)
		airoha_epon_write(xpon, AIROHA_EPON_SYNC_TIME, effective);
	xpon->epon.sync_time = effective;
	airoha_epon_write(xpon, AIROHA_EPON_TRX_ADJUST_TIME1,
	                  AIROHA_EPON_TX_STAMP_ADJUST_BASE +
	                          ((u32)effective << 16));
}

static void
airoha_xpon_epon_send_register_request_fast(struct airoha_xpon *xpon)
{
	u32 command;

	/* Three responses followed by one skipped Gate stagger upstream attempts. */
	if (xpon->epon.discovery_retry) {
		xpon->epon.discovery_retry--;
		if (!xpon->epon.discovery_retry)
			return;
	} else {
		xpon->epon.discovery_retry = AIROHA_EPON_DISCOVERY_RETRY_COUNT;
	}

	airoha_xpon_epon_apply_sync_time(xpon);

	airoha_xpon_epon_set_discovery_state(xpon,
	                                     AIROHA_EPON_STATE_REGISTERING);
	command = FIELD_PREP(AIROHA_EPON_DISCOVERY_LLID, 0) |
	          FIELD_PREP(AIROHA_EPON_DISCOVERY_COMMAND,
	                     AIROHA_EPON_COMMAND_REGISTER_REQUEST);
	/*
	 * REGISTER_REQUEST uses command 1 and a separate bit-8 request flag.
	 * A clear request flag yields LLID_DISCOVERY_CTRL value 0x40000000.
	 */
	/* writel() orders timestamp and state updates before the MPCP command. */
	airoha_epon_write(xpon, AIROHA_EPON_LLID_DISCOVERY_CTRL, command);
	atomic_inc(&xpon->epon.register_request_count);
}

static void airoha_xpon_epon_handle_register(struct airoha_xpon *xpon)
{
	u32 status = airoha_epon_read(xpon, AIROHA_EPON_LLID0_DISCOVERY_STATUS);
	u32 flag = FIELD_GET(AIROHA_EPON_REGISTER_FLAG, status);
	u32 command;

	xpon->epon.last_register_status = status;
	xpon->epon.register_message_count++;
	if (flag == AIROHA_EPON_REGISTER_DEREGISTER) {
		xpon->epon.deregister_count++;
		dev_info(xpon->dev, "EPON deregister received: status=0x%08x\n",
		         status);
		airoha_xpon_epon_reset_registration(xpon);
		return;
	}
	if (flag == AIROHA_EPON_REGISTER_NACK) {
		xpon->epon.nack_count++;
		dev_warn_ratelimited(
			xpon->dev,
			"EPON registration denied: status=0x%08x, retry delayed %u ms\n",
			status, AIROHA_EPON_NACK_SILENT_MS);
		airoha_xpon_epon_enter_denied(xpon);
		return;
	}
	if (flag != AIROHA_EPON_REGISTER_ACK &&
	    flag != AIROHA_EPON_REGISTER_REREGISTER)
		return;
	if (!(status & AIROHA_EPON_LLID_VALID))
		return;
	if (flag == AIROHA_EPON_REGISTER_REREGISTER)
		xpon->epon.reregister_count++;

	xpon->epon.llid = FIELD_GET(AIROHA_EPON_LLID_VALUE, status);
	xpon->epon.llid_valid = true;
	xpon->epon.mpcp_state = AIROHA_EPON_MPCP_REGISTER_PENDING;
	command = FIELD_PREP(AIROHA_EPON_DISCOVERY_LLID, 0) |
	          AIROHA_EPON_DISCOVERY_ACK_FLAG |
	          AIROHA_EPON_DISCOVERY_CMD_DONE |
	          FIELD_PREP(AIROHA_EPON_DISCOVERY_COMMAND,
	                     AIROHA_EPON_COMMAND_REGISTER_ACK);
	airoha_epon_write(xpon, AIROHA_EPON_LLID_DISCOVERY_CTRL, command);
}

static void airoha_xpon_epon_complete_registration(struct airoha_xpon *xpon)
{
	int ret;

	if (xpon->epon.mpcp_state != AIROHA_EPON_MPCP_REGISTER_PENDING ||
	    !xpon->epon.llid_valid)
		return;

	airoha_xpon_epon_set_discovery_state(xpon,
	                                     AIROHA_EPON_STATE_REGISTERED);
	ret = airoha_eth_pon_configure_epon(xpon->pon_netdev,
	                                    xpon->epon.channel, true);
	if (ret) {
		xpon->last_start_error = ret;
		airoha_xpon_epon_reset_registration(xpon);
		return;
	}

	xpon->epon.mpcp_state = AIROHA_EPON_MPCP_REGISTERED;
	xpon->epon.register_ack_count++;
	xpon->epon.registered_since = jiffies;
	xpon->data_path.configured = true;
	xpon->lifecycle = AIROHA_XPON_OPERATIONAL;
	airoha_xpon_oam_set_link(xpon->oam, true, xpon->epon.llid,
	                         xpon->epon.channel);
	airoha_xpon_set_data_path_link(xpon, true);
	airoha_xpon_leds_update(xpon);
	dev_info(xpon->dev, "EPON LLID0 registered: LLID=0x%04x\n",
	         xpon->epon.llid);
}

irqreturn_t airoha_xpon_epon_irq_fast(struct airoha_xpon *xpon)
{
	u32 enabled = airoha_epon_read(xpon, AIROHA_EPON_INT_ENABLE);
	u32 events = airoha_epon_read(xpon, AIROHA_EPON_INT_STATUS) & enabled;
	u32 error_enabled = airoha_epon_read(xpon, AIROHA_EPON_INT_ENABLE2);
	u32 error_events = airoha_epon_read(xpon, AIROHA_EPON_INT_STATUS2) &
	                   error_enabled;

	if (!events && !error_events)
		return IRQ_NONE;
	if (events)
		airoha_epon_write(xpon, AIROHA_EPON_INT_STATUS, events);
	if (error_events)
		airoha_epon_write(xpon, AIROHA_EPON_INT_STATUS2, error_events);
	if (READ_ONCE(xpon->stopping))
		return IRQ_HANDLED;

	/*
	 * The hard IRQ clears W1C status and publishes pending bits. Discovery
	 * Gates use the bounded fast path; sleepable transitions stay in the thread.
	 */
	if (error_events)
		atomic_or(error_events, &xpon->epon.pending_error_events);
	if (events & ~AIROHA_EPON_INT_DISCOVERY_GATE)
		atomic_or(events & ~AIROHA_EPON_INT_DISCOVERY_GATE,
		          &xpon->epon.pending_events);

	if (events & AIROHA_EPON_INT_DISCOVERY_GATE) {
		atomic_inc(&xpon->epon.discovery_gate_count);
		/* The thread emits the first Gate while hard IRQ latency stays bounded. */
		if (!READ_ONCE(xpon->epon.discovery_seen)) {
			WRITE_ONCE(xpon->epon.discovery_seen, true);
			atomic_or(AIROHA_EPON_INT_DISCOVERY_GATE,
			          &xpon->epon.pending_events);
		}
		if (READ_ONCE(xpon->tx_armed) &&
		    !(events & AIROHA_EPON_INT_REGISTER_REQ_DONE) &&
		    READ_ONCE(xpon->epon.mpcp_state) !=
		            AIROHA_EPON_MPCP_REGISTERED &&
		    READ_ONCE(xpon->epon.mpcp_state) !=
		            AIROHA_EPON_MPCP_DENIED) {
			airoha_xpon_epon_send_register_request_fast(xpon);
		}
	}

	if (atomic_read(&xpon->epon.pending_events) ||
	    atomic_read(&xpon->epon.pending_error_events))
		return IRQ_WAKE_THREAD;

	return IRQ_HANDLED;
}

irqreturn_t airoha_xpon_epon_irq(struct airoha_xpon *xpon)
{
	u32 events = atomic_xchg(&xpon->epon.pending_events, 0);
	u32 error_events = atomic_xchg(&xpon->epon.pending_error_events, 0);

	if (!events && !error_events)
		return IRQ_NONE;
	if (READ_ONCE(xpon->stopping))
		return IRQ_HANDLED;
	mutex_lock(&xpon->state_lock);
	if (xpon->stopping || !xpon->active_mode_valid ||
	    !airoha_xpon_mode_is_epon(xpon->active_mode))
		goto out_unlock;
	if (error_events) {
		xpon->epon.last_mac_error = error_events;
		xpon->epon.mac_error_count += hweight32(error_events);
		dev_warn_ratelimited(xpon->dev,
		                     "EPON MAC error interrupt: 0x%04x\n",
		                     error_events);
	}
	if (events & AIROHA_EPON_INT_DISCOVERY_GATE)
		dev_info(
			xpon->dev,
			"EPON first Discovery Gate received: sync_time=0x%04x, tx_armed=%u\n",
			xpon->epon.sync_time, xpon->tx_armed);

	/* Consume Register Request completion before a coalesced Discovery Gate. */
	if (events & AIROHA_EPON_INT_REGISTER_REQ_DONE)
		airoha_epon_update_bits(xpon, AIROHA_EPON_MPCP_TIMEOUT_INTERVAL,
		                        AIROHA_EPON_DISCOVERY_SYNC_UPDATE, 0);
	if (events & AIROHA_EPON_INT_REGISTER_REQ_DONE) {
		xpon->epon.mpcp_state = AIROHA_EPON_MPCP_REGISTER_REQUEST;
		dev_info_ratelimited(
			xpon->dev,
			"EPON Register Request completed: requests=%d\n",
			atomic_read(&xpon->epon.register_request_count));
	}
	if (events & AIROHA_EPON_INT_LLID0_REGISTER)
		airoha_xpon_epon_handle_register(xpon);
	if (events & AIROHA_EPON_INT_REGISTER_ACK_DONE)
		airoha_xpon_epon_complete_registration(xpon);
	if (events & AIROHA_EPON_INT_MPCP_TIMEOUT) {
		xpon->epon.mpcp_timeout_count++;
		dev_warn_ratelimited(
			xpon->dev,
			"EPON MPCP timeout: state=%u llid_valid=%u\n",
			xpon->epon.mpcp_state, xpon->epon.llid_valid);
		airoha_xpon_epon_send_deregister(xpon);
		airoha_xpon_epon_reset_registration(xpon);
	}
out_unlock:
	mutex_unlock(&xpon->state_lock);
	return IRQ_HANDLED;
}

irqreturn_t airoha_xpon_epon_phy_irq(struct airoha_xpon *xpon)
{
	u32 raw_events;
	u32 enabled =
		airoha_pon_phy_read(xpon, airoha_xpon_epon_is_1g(xpon) ?
	                                          AIROHA_GEPON_INT_ENABLE :
	                                          AIROHA_XEPON_PCS_INT_ENABLE);
	u32 events;

	if (airoha_xpon_epon_is_1g(xpon)) {
		raw_events =
			airoha_pon_phy_read(xpon, AIROHA_GEPON_INT_STATUS) &
			enabled;
		if (!raw_events)
			return IRQ_NONE;
		airoha_pon_phy_write(xpon, AIROHA_GEPON_INT_CLEAR, raw_events);
		events = 0;
		if (raw_events & AIROHA_GEPON_INT_PHY_READY)
			events |= AIROHA_XEPON_PCS_INT_SYNC_OK;
		if (raw_events & AIROHA_GEPON_INT_LOF)
			events |= AIROHA_XEPON_PCS_INT_SYNC_LOSS;
		if (raw_events & AIROHA_GEPON_INT_LOS)
			events |= AIROHA_XEPON_PCS_INT_LASER_LOSS;
		if (raw_events & AIROHA_GEPON_INT_NO_LOS)
			events |= AIROHA_XEPON_PCS_INT_NO_LASER_LOSS;
	} else {
		events =
			airoha_pon_phy_read(xpon, AIROHA_XEPON_PCS_INT_STATUS) &
			enabled;
		if (!events)
			return IRQ_NONE;
		airoha_pon_phy_write(xpon, AIROHA_XEPON_PCS_INT_STATUS, events);
	}

	if (READ_ONCE(xpon->stopping))
		return IRQ_HANDLED;

	/* PCS may latch SYNC_OK and SYNC_LOSS together; use current sync state. */
	if (!airoha_xpon_epon_is_1g(xpon) &&
	    (events &
	     (AIROHA_XEPON_PCS_INT_SYNC_OK | AIROHA_XEPON_PCS_INT_SYNC_LOSS)) ==
	            (AIROHA_XEPON_PCS_INT_SYNC_OK |
	             AIROHA_XEPON_PCS_INT_SYNC_LOSS)) {
		u32 sync = airoha_pon_phy_read(xpon,
		                               AIROHA_XEPON_PCS_RX_SYNC_STATUS);

		if (sync & AIROHA_XEPON_PCS_SYNC_OK)
			events &= ~AIROHA_XEPON_PCS_INT_SYNC_LOSS;
		else
			events &= ~AIROHA_XEPON_PCS_INT_SYNC_OK;
	}

	atomic_or(events, &xpon->phy_events);
	xpon->stats.phy_irq_count++;
	if (events & AIROHA_XEPON_PCS_INT_SYNC_OK)
		xpon->stats.phy_sync_count++;
	if (events & AIROHA_XEPON_PCS_INT_SYNC_LOSS)
		xpon->stats.phy_lof_count++;
	if (events & AIROHA_XEPON_PCS_INT_LASER_LOSS)
		xpon->stats.phy_los_count++;
	if (events & (AIROHA_XEPON_PCS_INT_LASER_LOSS |
	              AIROHA_XEPON_PCS_INT_NO_LASER_LOSS))
		mod_delayed_work(system_wq, &xpon->link_work, 0);
	else
		queue_delayed_work(system_wq, &xpon->link_work, 0);

	return IRQ_HANDLED;
}

int airoha_xpon_epon_advance(struct airoha_xpon *xpon, u32 phy_events,
                             unsigned long *delay)
{
	bool synced = airoha_xpon_epon_pcs_synced(xpon);
	bool full_reinit;
	unsigned long deadline;
	int ret;

	if (xpon->epon.mpcp_state == AIROHA_EPON_MPCP_DENIED) {
		if (time_before(jiffies, xpon->epon.denied_until))
			return 0;
		dev_info(xpon->dev,
		         "EPON registration deny interval expired\n");
		airoha_xpon_epon_reset_registration(xpon);
	}

	if (synced) {
		xpon->epon.sync_loss_since = 0;
		if (xpon->epon.pcs_synced)
			return 0;
		xpon->epon.pcs_synced = true;
		dev_info(xpon->dev,
		         "EPON PCS synchronized: PHY events=0x%08x\n",
		         phy_events);
		xpon->lifecycle = AIROHA_XPON_PROTOCOL_ACTIVATING;
		ret = airoha_xpon_epon_start_mac(xpon);
		if (ret)
			xpon->epon.pcs_synced = false;
		return ret;
	}
	if (xpon->epon.pcs_synced) {
		xpon->epon.pcs_synced = false;
		xpon->epon.sync_loss_count++;
		dev_warn_ratelimited(
			xpon->dev,
			"EPON PCS synchronization lost: PHY events=0x%08x\n",
			phy_events);
	}
	xpon->lifecycle = AIROHA_XPON_WAIT_LINE_SYNC;
	if (!xpon->epon.sync_loss_since)
		xpon->epon.sync_loss_since = jiffies;
	deadline = xpon->epon.sync_loss_since +
	           msecs_to_jiffies(AIROHA_XPON_NO_SYNC_RECOVERY_MS);
	if (time_before(jiffies, deadline)) {
		if (time_before(deadline, jiffies + *delay))
			*delay = deadline - jiffies;
		return 0;
	}

	airoha_xpon_epon_stop_rx(xpon);
	xpon->epon.recovery_count++;
	full_reinit = xpon->epon.recovery_count % 5 == 0;
	if (full_reinit) {
		xpon->pcs_profile_valid = false;
		xpon->epon.full_reinit_count++;
		dev_warn_ratelimited(
			xpon->dev,
			"EPON PCS remains unsynchronized; running full PMA initialization (recovery %u)\n",
			xpon->epon.recovery_count);
	} else {
		dev_warn_ratelimited(
			xpon->dev,
			"EPON PCS remains unsynchronized; running short PMA retrain (recovery %u)\n",
			xpon->epon.recovery_count);
	}

	ret = airoha_xpon_epon_start_rx(xpon);
	if (!ret)
		xpon->last_start_error = 0;
	return ret;
}
