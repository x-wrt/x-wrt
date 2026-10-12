// SPDX-License-Identifier: GPL-2.0-only

#include <airoha-pon-frontend.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/workqueue.h>

#include "airoha-xpon-private.h"
#include "airoha-xpon-omci.h"
#include "airoha-xpon-xgpon.h"

static void airoha_xpon_xgpon_release_mac_interfaces(struct airoha_xpon *xpon)
{
	/* Clear MBI/MPI stop bits after SW_RST is released. */
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_MBI_MPI_STOP,
	                         AIROHA_XGPON_MBI_MPI_STOP_MASK, 0);
}

static void airoha_xpon_xgpon_stop_mac_interfaces(struct airoha_xpon *xpon,
                                                  const char *reason)
{
	u32 value;
	int ret;

	/* All four done bits mark a quiescent MAC before reset. */
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_MBI_MPI_STOP,
	                         AIROHA_XGPON_MBI_MPI_STOP_MASK,
	                         AIROHA_XGPON_MBI_MPI_STOP_MASK);
	ret = readl_poll_timeout_atomic(
		xpon->xgpon_base + AIROHA_XGPON_MBI_MPI_STOP, value,
		(value & AIROHA_XGPON_MBI_MPI_STOP_DONE_MASK) ==
			AIROHA_XGPON_MBI_MPI_STOP_DONE_MASK,
		1, 3000);
	if (ret)
		dev_warn_ratelimited(
			xpon->dev,
			"timed out stopping XG-PON MBI/MPI (%s), raw=0x%08x\n",
			reason, value);
}

void airoha_xpon_clear_mac_errors(struct airoha_xpon *xpon,
                                  bool clear_software_latch)
{
	/* Clear detailed W1C errors before their summary bits. */
	airoha_xgpon_write(xpon, AIROHA_XGPON_FIFO_ERR_STS, U32_MAX);
	airoha_xgpon_write(xpon, AIROHA_XGPON_TX_ERR_STS, U32_MAX);
	airoha_xgpon_write(xpon, AIROHA_XGPON_RX_ERR_STS, U32_MAX);
	airoha_xgpon_write(xpon, AIROHA_XGPON_DBG_BWM_CHK_STS, U32_MAX);
	airoha_xgpon_write(xpon, AIROHA_XGPON_INT_STATUS,
	                   AIROHA_XGPON_INT_ERROR_MASK);

	if (clear_software_latch) {
		WRITE_ONCE(xpon->stats.latched_fifo_error, 0);
		WRITE_ONCE(xpon->stats.latched_tx_error, 0);
		WRITE_ONCE(xpon->stats.latched_rx_error, 0);
		WRITE_ONCE(xpon->stats.latched_bwm_check, 0);
	}
}

static int airoha_xpon_xgpon_set_tx_interface_stop(struct airoha_xpon *xpon,
                                                   u32 stop_bit, u32 done_bit,
                                                   bool stop)
{
	u32 value;

	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_MBI_MPI_STOP, stop_bit,
	                         stop ? stop_bit : 0);
	if (!stop)
		return 0;

	/* stop_done signals that the corresponding interface is quiescent. */
	return readl_poll_timeout_atomic(xpon->xgpon_base +
	                                         AIROHA_XGPON_MBI_MPI_STOP,
	                                 value, value & done_bit, 1, 3000);
}

static int airoha_xpon_xgpon_resync_tx_locked(struct airoha_xpon *xpon)
{
	u32 profile_valid, value;
	bool mbi_stopped = false;
	bool mpi_stopped = false;
	int ret;

	lockdep_assert_held(&xpon->state_lock);
	mutex_lock(&xpon->ploam_lock);

	profile_valid = airoha_xgpon_read(xpon, AIROHA_XGPON_US_PROF_VLD);
	airoha_xgpon_write(xpon, AIROHA_XGPON_US_PROF_VLD,
	                   profile_valid & ~AIROHA_XGPON_US_PROFILE_VALID_MASK);

	mbi_stopped = true;
	ret = airoha_xpon_xgpon_set_tx_interface_stop(
		xpon, AIROHA_XGPON_MBI_TX_STOP, AIROHA_XGPON_MBI_TX_STOP_DONE,
		true);
	if (ret)
		goto out_restore;

	mpi_stopped = true;
	ret = airoha_xpon_xgpon_set_tx_interface_stop(
		xpon, AIROHA_XGPON_MPI_TX_STOP, AIROHA_XGPON_MPI_TX_STOP_DONE,
		true);
	if (ret)
		goto out_restore;

	/* Software resync uses the current OLT EqD when DBG_RESYNC is submitted. */
	airoha_xgpon_write(xpon, AIROHA_XGPON_EQD, xpon->eqd);
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_DBG_RESYNC,
	                         AIROHA_XGPON_SW_RESYNC_MASK,
	                         AIROHA_XGPON_SW_RESYNC_MASK);
	readl_poll_timeout_atomic(xpon->xgpon_base + AIROHA_XGPON_DBG_RESYNC,
	                          value, value & AIROHA_XGPON_TX_SYNC_READY, 1,
	                          3000);

	/* Restore MPI first; TX sync ready gates MBI and profile restoration. */
	airoha_xpon_xgpon_set_tx_interface_stop(xpon, AIROHA_XGPON_MPI_TX_STOP,
	                                        AIROHA_XGPON_MPI_TX_STOP_DONE,
	                                        false);
	mpi_stopped = false;
	ret = readl_poll_timeout_atomic(
		xpon->xgpon_base + AIROHA_XGPON_DBG_RESYNC, value,
		value & AIROHA_XGPON_TX_SYNC_READY, 1, 200);

out_restore:
	if (mpi_stopped)
		airoha_xpon_xgpon_set_tx_interface_stop(
			xpon, AIROHA_XGPON_MPI_TX_STOP,
			AIROHA_XGPON_MPI_TX_STOP_DONE, false);
	if (mbi_stopped)
		airoha_xpon_xgpon_set_tx_interface_stop(
			xpon, AIROHA_XGPON_MBI_TX_STOP,
			AIROHA_XGPON_MBI_TX_STOP_DONE, false);
	airoha_xgpon_write(xpon, AIROHA_XGPON_US_PROF_VLD, profile_valid);
	mutex_unlock(&xpon->ploam_lock);

	return ret;
}

void airoha_xpon_set_onu_state(struct airoha_xpon *xpon,
                               enum airoha_xgpon_onu_state state)
{
	/* ACTIVATION_ST commits the ONU-ID, EqD, and T-CONT settings together. */
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_ACTIVATION_ST,
	                         AIROHA_XGPON_ACTIVATION_STATE_MASK,
	                         FIELD_PREP(AIROHA_XGPON_ACTIVATION_STATE_MASK,
	                                    state));
	xpon->onu_state = state;
	airoha_xpon_leds_update(xpon);
}

void airoha_xpon_update_omci_link(struct airoha_xpon *xpon)
{
	const struct airoha_xpon_mode_info *mode;
	u32 key_index;
	bool ready;

	mode = airoha_xpon_mode_info(xpon->active_mode);
	if (!xpon->active_mode_valid || !mode ||
	    mode->control_protocol != AIROHA_XPON_CONTROL_OMCI || !xpon->omci)
		return;

	ready = xpon->onu_state == AIROHA_XGPON_O5 &&
	        xpon->onu_id != AIROHA_XGPON_BROADCAST_ID &&
	        xpon->crypto.default_ploam_ik_programmed &&
	        xpon->crypto.activation_keys_programmed &&
	        READ_ONCE(xpon->tx_armed);
	key_index = airoha_xgpon_read(xpon, AIROHA_XGPON_CUR_KIDX);
	airoha_xpon_omci_set_link(xpon->omci, ready,
	                          ready ? xpon->onu_id :
	                                  AIROHA_XGPON_BROADCAST_ID,
	                          !!(key_index & AIROHA_XGPON_CUR_OIK_INDEX));
}

static void airoha_xpon_xgpon_program_identity(struct airoha_xpon *xpon)
{
	const struct airoha_xpon_identity *identity =
		&xpon->identity.active_identity;
	unsigned int i;

	if (identity->serial_configured) {
		airoha_xgpon_write(
			xpon, AIROHA_XGPON_VENDOR_ID,
			airoha_xpon_bytes_to_u32(identity->serial_number));
		airoha_xgpon_write(
			xpon, AIROHA_XGPON_VS_SN,
			airoha_xpon_bytes_to_u32(identity->serial_number + 4));
	}

	/* Registration-ID words map to RGS_ID registers in reverse order. */
	for (i = 0; i < 9; i++)
		airoha_xgpon_write(
			xpon, AIROHA_XGPON_RGS_ID3_0 + i * 4,
			airoha_xpon_bytes_to_u32(identity->registration_id +
		                                 (8 - i) * 4));
}

irqreturn_t airoha_xpon_xgpon_phy_irq(struct airoha_xpon *xpon)
{
	u32 events, status, sync;

	status = airoha_pon_phy_read(xpon, AIROHA_XGPON_PHY_INT_STATUS);
	events = status & AIROHA_XGPON_PHY_INT_RX_MASK;
	if (!events)
		return IRQ_NONE;

	/* SPI 43 status is W1C; link_work serializes PMA calibration. */
	airoha_pon_phy_write(xpon, AIROHA_XGPON_PHY_INT_STATUS, events);
	if (READ_ONCE(xpon->stopping))
		return IRQ_HANDLED;

	/* SYNC_OK and LOF may coexist; the current XGTC state resolves the signal. */
	if ((events &
	     (AIROHA_XGPON_PHY_INT_RX_SYNC_OK | AIROHA_XGPON_PHY_INT_RX_LOF)) ==
	    (AIROHA_XGPON_PHY_INT_RX_SYNC_OK | AIROHA_XGPON_PHY_INT_RX_LOF)) {
		sync = airoha_pon_phy_read(xpon,
		                           AIROHA_XGPON_PHY_DBG_RX_SYNC_ST);
		if (FIELD_GET(AIROHA_XGPON_PHY_RX_SYNC_MASK, sync) ==
		    AIROHA_XGPON_PHY_RX_SYNC_IN)
			events &= ~AIROHA_XGPON_PHY_INT_RX_LOF;
		else
			events &= ~AIROHA_XGPON_PHY_INT_RX_SYNC_OK;
	}

	atomic_or(events, &xpon->phy_events);
	xpon->stats.phy_irq_count++;
	if (events & AIROHA_XGPON_PHY_INT_RX_RDY)
		xpon->stats.phy_rx_ready_count++;
	if (events & AIROHA_XGPON_PHY_INT_RX_SYNC_OK)
		xpon->stats.phy_sync_count++;
	if (events & AIROHA_XGPON_PHY_INT_RX_LOS)
		xpon->stats.phy_los_count++;
	if (events & AIROHA_XGPON_PHY_INT_RX_LOF)
		xpon->stats.phy_lof_count++;

	if (events &
	    (AIROHA_XGPON_PHY_INT_RX_LOS | AIROHA_XGPON_PHY_INT_RX_RDY))
		mod_delayed_work(system_wq, &xpon->link_work, 0);
	else
		queue_delayed_work(system_wq, &xpon->link_work, 0);
	return IRQ_HANDLED;
}

irqreturn_t airoha_xpon_xgpon_irq(struct airoha_xpon *xpon)
{
	u32 enabled, events, status;
	u32 fifo_error = 0, tx_error = 0, rx_error = 0, bwm_check = 0;

	status = airoha_xgpon_read(xpon, AIROHA_XGPON_INT_STATUS);
	enabled = airoha_xgpon_read(xpon, AIROHA_XGPON_INT_ENABLE);
	events = status & enabled;
	if (!events)
		return IRQ_NONE;

	airoha_xgpon_write(xpon, AIROHA_XGPON_INT_STATUS, events);
	if (READ_ONCE(xpon->stopping))
		return IRQ_HANDLED;

	if (events & AIROHA_XGPON_INT_SN_REQUEST)
		xpon->stats.sn_request_count++;
	if (events & AIROHA_XGPON_INT_SN_SENT)
		xpon->stats.sn_sent_count++;
	if (events & AIROHA_XGPON_INT_RANGING_REQUEST) {
		u32 key_index;

		xpon->stats.ranging_request_count++;
		key_index = airoha_xgpon_read(xpon, AIROHA_XGPON_CUR_KIDX);
		if (!(key_index & AIROHA_XGPON_CUR_PIK_INDEX) &&
		    !(key_index & AIROHA_XGPON_CUR_OIK_INDEX))
			WRITE_ONCE(xpon->crypto.o4_key_switch_observed, true);
	}
	if (events & AIROHA_XGPON_INT_REGISTRATION_SENT)
		xpon->stats.registration_sent_count++;

	if (events & AIROHA_XGPON_INT_ERROR_MASK) {
		fifo_error = airoha_xgpon_read(xpon, AIROHA_XGPON_FIFO_ERR_STS);
		tx_error = airoha_xgpon_read(xpon, AIROHA_XGPON_TX_ERR_STS);
		rx_error = airoha_xgpon_read(xpon, AIROHA_XGPON_RX_ERR_STS);
		bwm_check =
			airoha_xgpon_read(xpon, AIROHA_XGPON_DBG_BWM_CHK_STS);
		WRITE_ONCE(xpon->stats.latched_fifo_error,
		           READ_ONCE(xpon->stats.latched_fifo_error) |
		                   fifo_error);
		WRITE_ONCE(xpon->stats.latched_tx_error,
		           READ_ONCE(xpon->stats.latched_tx_error) | tx_error);
		WRITE_ONCE(xpon->stats.latched_rx_error,
		           READ_ONCE(xpon->stats.latched_rx_error) | rx_error);
		WRITE_ONCE(xpon->stats.latched_bwm_check,
		           READ_ONCE(xpon->stats.latched_bwm_check) |
		                   bwm_check);
		xpon->stats.mac_error_irq_count++;
		if (fifo_error & AIROHA_XGPON_FIFO_ERR_RX_MBI_HEADER_OVERRUN)
			xpon->stats.rx_mbi_header_overrun_irq_count++;
		if (fifo_error & AIROHA_XGPON_FIFO_ERR_RX_MBI_PAYLOAD_OVERRUN)
			xpon->stats.rx_mbi_payload_overrun_irq_count++;

		airoha_xgpon_write(xpon, AIROHA_XGPON_FIFO_ERR_STS, U32_MAX);
		airoha_xgpon_write(xpon, AIROHA_XGPON_TX_ERR_STS, U32_MAX);
		airoha_xgpon_write(xpon, AIROHA_XGPON_RX_ERR_STS, U32_MAX);
		airoha_xgpon_write(xpon, AIROHA_XGPON_DBG_BWM_CHK_STS, U32_MAX);
		dev_warn_ratelimited(
			xpon->dev,
			"XG-PON MAC error: int=0x%08x fifo=0x%08x tx=0x%08x rx=0x%08x bwm=0x%08x\n",
			(u32)(events & AIROHA_XGPON_INT_ERROR_MASK), fifo_error,
			tx_error, rx_error, bwm_check);

		if ((events & AIROHA_XGPON_INT_TX_ERROR) &&
		    (tx_error & AIROHA_XGPON_TX_ERR_LATE_START)) {
			xpon->stats.tx_late_error_count++;
			atomic_set(&xpon->tx_resync_pending, 1);
			mod_delayed_work(system_wq, &xpon->link_work, 0);
		}
	}

	if ((events & AIROHA_XGPON_INT_PLOAMD_RECV) &&
	    airoha_xpon_drain_ploamd(xpon) == AIROHA_XGPON_PLOAMD_MAX_DRAIN)
		dev_warn_ratelimited(
			xpon->dev,
			"PLOAMd FIFO drain reached the per-IRQ safety limit\n");
	if (atomic_read(&xpon->mac_restart_pending) ||
	    atomic_read(&xpon->data_path_retry_pending))
		mod_delayed_work(system_wq, &xpon->link_work, 0);

	return IRQ_HANDLED;
}

void airoha_xpon_xgpon_mask_irqs(struct airoha_xpon *xpon)
{
	airoha_xgpon_write(xpon, AIROHA_XGPON_INT_ENABLE, 0);
	airoha_pon_phy_write(xpon, AIROHA_XGPON_PHY_INT_ENABLE, 0);
}

bool airoha_xpon_xgpon_los(struct airoha_xpon *xpon)
{
	return !!(airoha_pon_phy_read(xpon, AIROHA_XGPON_PHY_SFP_STA) &
	          AIROHA_XGPON_PHY_SFP_RX_LOS);
}

static void airoha_xpon_xgpon_hold_digital_rx_reset(struct airoha_xpon *xpon)
{
	u32 rx_ctrl;

	rx_ctrl = airoha_pon_phy_read(xpon, AIROHA_XGPON_PHY_RESET_CTRL);
	airoha_pon_phy_write(xpon, AIROHA_XGPON_PHY_RESET_CTRL,
	                     rx_ctrl & ~AIROHA_XGPON_PHY_RESET_MASK);
}

static int airoha_xpon_xgpon_release_digital_rx_reset(struct airoha_xpon *xpon)
{
	u32 rx_ctrl;

	rx_ctrl = airoha_pon_phy_read(xpon, AIROHA_XGPON_PHY_RESET_CTRL);
	airoha_pon_phy_write(xpon, AIROHA_XGPON_PHY_RESET_CTRL,
	                     rx_ctrl | AIROHA_XGPON_PHY_RESET_RELEASED);
	usleep_range(1000, 1200);
	rx_ctrl = airoha_pon_phy_read(xpon, AIROHA_XGPON_PHY_RESET_CTRL);
	if ((rx_ctrl & AIROHA_XGPON_PHY_RESET_MASK) !=
	    AIROHA_XGPON_PHY_RESET_RELEASED)
		return -EIO;

	return 0;
}

static int airoha_xpon_xgpon_enable_digital_rx(struct airoha_xpon *xpon)
{
	u32 rx_ctrl;

	airoha_pon_phy_write(xpon, AIROHA_XGPON_PHY_SFP_VLD_LEVEL,
	                     xpon->frontend_link.xgpon_sfp_valid_level);
	airoha_pon_phy_write(xpon, AIROHA_XGPON_PHY_INT_ENABLE, 0);
	airoha_pon_phy_write(xpon, AIROHA_XGPON_PHY_INT_STATUS,
	                     AIROHA_XGPON_PHY_INT_RX_MASK);
	airoha_pon_phy_write(xpon, AIROHA_XGPON_PHY_INT_ENABLE,
	                     AIROHA_XGPON_PHY_INT_RX_MASK);

	rx_ctrl = airoha_pon_phy_read(xpon, AIROHA_XGPON_PHY_RX_SYNC_CTRL);
	airoha_pon_phy_write(xpon, AIROHA_XGPON_PHY_RX_SYNC_CTRL,
	                     (rx_ctrl & ~AIROHA_XGPON_PHY_RX_MODE_MASK) |
	                             AIROHA_XGPON_PHY_RX_MODE_XGPON |
	                             AIROHA_XGPON_PHY_RX_ENABLE);
	rx_ctrl = airoha_pon_phy_read(xpon, AIROHA_XGPON_PHY_RX_SYNC_CTRL);

	return (rx_ctrl &
	        (AIROHA_XGPON_PHY_RX_MODE_MASK | AIROHA_XGPON_PHY_RX_ENABLE)) ==
	                       (AIROHA_XGPON_PHY_RX_MODE_XGPON |
	                        AIROHA_XGPON_PHY_RX_ENABLE) ?
	               0 :
	               -EIO;
}

static int
airoha_xpon_xgpon_reset_and_enable_digital_rx(struct airoha_xpon *xpon)
{
	int ret;

	/* FIRST_PLUG_IN requires a 1 ms hold/release digital reset handoff. */
	airoha_xpon_xgpon_hold_digital_rx_reset(xpon);
	usleep_range(1000, 1200);
	ret = airoha_xpon_xgpon_release_digital_rx_reset(xpon);
	if (ret)
		return ret;

	return airoha_xpon_xgpon_enable_digital_rx(xpon);
}

static int airoha_xpon_xgpon_start_control(struct airoha_xpon *xpon)
{
	if (READ_ONCE(xpon->stopping))
		return -ESHUTDOWN;

	/* Release MAC reset after XGTC synchronization, before O2_3. */
	airoha_xgpon_write(xpon, AIROHA_XGPON_INT_ENABLE, 0);
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_SW_RST,
	                         AIROHA_XGPON_MAC_RESET_RELEASED,
	                         AIROHA_XGPON_MAC_RESET_RELEASED);
	airoha_xpon_xgpon_counters_start(xpon);
	airoha_xpon_xgpon_release_mac_interfaces(xpon);
	airoha_xgpon_write(xpon, AIROHA_XGPON_US_PROF_VLD, 0);
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_RSP_TIME,
	                         AIROHA_XGPON_RESPONSE_TIME_MASK, 0x551);
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_O23_O4_PLOAMU_CTRL,
	                         AIROHA_XGPON_O23_O4_SOFTWARE_REPLY, 0);
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_DBG_CAP_SETTING,
	                         AIROHA_XGPON_DBG_O52_IDLE_ONLY |
	                                 AIROHA_XGPON_DBG_HW_US_OMCI_MIC |
	                                 AIROHA_XGPON_DBG_HW_DS_OMCI_MIC,
	                         AIROHA_XGPON_DBG_O52_IDLE_ONLY |
	                                 AIROHA_XGPON_DBG_HW_US_OMCI_MIC |
	                                 AIROHA_XGPON_DBG_HW_DS_OMCI_MIC);
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_DBG_RESYNC,
	                         AIROHA_XGPON_TX_LATE_AUTO_RESYNC, 0);
	airoha_pon_phy_update_bits(xpon, AIROHA_XGPON_PHY_XG_CONTINUE_CTRL,
	                           AIROHA_XGPON_PHY_CONTINUE_MASK, 0);
	airoha_xpon_xgpon_program_identity(xpon);

	xpon->crypto.default_ploam_ik_programmed = false;
	xpon->crypto.activation_keys_programmed = false;
	xpon->crypto.o4_key_switch_observed = false;
	xpon->crypto.generation_runs = 0;
	xpon->crypto.generation_failed_type = 0;
	xpon->crypto.key_control_rx_count = 0;
	xpon->crypto.key_report_tx_count = 0;
	xpon->crypto.key_exchange_completed_count = 0;
	xpon->crypto.key_exchange_error_count = 0;
	airoha_xpon_clear_assignment(xpon);
	airoha_xpon_clear_tconts(xpon);
	airoha_xpon_set_onu_state(xpon, AIROHA_XGPON_O2_3);

	/* Clear bootloader state before enabling activation interrupts. */
	airoha_xpon_drain_ploamd(xpon);
	airoha_xpon_clear_mac_errors(xpon, true);
	airoha_xgpon_write(xpon, AIROHA_XGPON_INT_STATUS,
	                   AIROHA_XGPON_INT_ACTIVATION_MASK |
	                           AIROHA_XGPON_INT_ERROR_MASK);
	airoha_xgpon_write(xpon, AIROHA_XGPON_INT_ENABLE,
	                   AIROHA_XGPON_INT_ACTIVATION_MASK |
	                           AIROHA_XGPON_INT_FIFO_ERROR);
	xpon->xgpon_irq_enabled = true;
	xpon->mac_initialized = true;
	xpon->lifecycle = AIROHA_XPON_PROTOCOL_ACTIVATING;

	dev_info(
		xpon->dev,
		"XGTC in sync; MAC entered O2_3, SN=%s, BEN/upstream TX remains disabled\n",
		xpon->identity.active_identity.serial_configured ?
			"configured" :
			"missing");

	return 0;
}

int airoha_xpon_xgpon_prepare(struct airoha_xpon *xpon)
{
	/* Hold the XG-PON MAC in reset during PMA initialization. */
	airoha_xgpon_write(xpon, AIROHA_XGPON_INT_ENABLE, 0);
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_MBI_MPI_STOP,
	                         AIROHA_XGPON_MBI_MPI_STOP_MASK,
	                         AIROHA_XGPON_MBI_MPI_STOP_MASK);
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_SW_RST,
	                         AIROHA_XGPON_MAC_RESET_RELEASED, 0);
	airoha_pon_phy_write(xpon, AIROHA_XGPON_PHY_SFP_VLD_LEVEL,
	                     xpon->frontend_link.xgpon_sfp_valid_level);
	airoha_pon_phy_write(xpon, AIROHA_XGPON_PHY_INT_ENABLE, 0);
	airoha_pon_phy_write(xpon, AIROHA_XGPON_PHY_INT_STATUS,
	                     AIROHA_XGPON_PHY_INT_RX_MASK);
	xpon->lifecycle = AIROHA_XPON_WAIT_OPTICAL_SIGNAL;

	return 0;
}

int airoha_xpon_xgpon_start_rx(struct airoha_xpon *xpon)
{
	enum airoha_pcs_pon_mode pcs_mode;
	bool retrain;
	u32 onu_id;
	int ret;

	pcs_mode = xpon->active_mode == AIROHA_XPON_MODE_XGSPON ?
	                   AIROHA_PCS_PON_MODE_XGSPON :
	                   AIROHA_PCS_PON_MODE_XGPON;
	retrain = xpon->pcs_profile_valid && xpon->pcs_profile == pcs_mode;
	if (!retrain) {
		ret = airoha_pcs_pon_config_rx(
			xpon->pcs, pcs_mode,
			xpon->frontend_link.pma_xpon_setting0,
			xpon->frontend_link.pma_xpon_setting1);
		if (!ret) {
			xpon->pcs_profile = pcs_mode;
			xpon->pcs_profile_valid = true;
		}
	} else {
		usleep_range(2000, 2500);
		airoha_xpon_xgpon_hold_digital_rx_reset(xpon);
		usleep_range(1000, 1200);
		ret = airoha_pcs_pon_retrain_rx(xpon->pcs);
		if (ret) {
			airoha_xpon_xgpon_release_digital_rx_reset(xpon);
			return ret;
		}
		usleep_range(1000, 1200);
	}
	if (ret)
		return ret;
	xpon->lifecycle = AIROHA_XPON_PMA_CONFIGURED;

	if (retrain) {
		ret = airoha_xpon_xgpon_release_digital_rx_reset(xpon);
		if (!ret)
			ret = airoha_xpon_xgpon_enable_digital_rx(xpon);
		if (!ret)
			usleep_range(8000, 9000);
	} else {
		ret = airoha_xpon_xgpon_reset_and_enable_digital_rx(xpon);
	}
	if (ret)
		return ret;

	xpon->lifecycle = AIROHA_XPON_WAIT_LINE_SYNC;
	xpon->rx_active = true;
	xpon->stats.rx_start_count++;
	xpon->no_sync_since = jiffies;

	onu_id = airoha_xgpon_read(xpon, AIROHA_XGPON_ONU_ID);
	dev_info(
		xpon->dev,
		"XG-PON RX started, irq=%d, ONU-ID=%lu (%s), burst TX remains disabled\n",
		xpon->mac_irq, FIELD_GET(AIROHA_XGPON_ONU_ID_MASK, onu_id),
		onu_id & AIROHA_XGPON_ONU_ID_VALID ? "valid" : "invalid");

	return 0;
}

static const u32 airoha_xpon_xgpon_counter_registers[AIROHA_XPON_MIB_COUNT] = {
	[AIROHA_XPON_MIB_XGTC_RX] = AIROHA_XGPON_RX_XGTC_CNT,
	[AIROHA_XPON_MIB_BURSTS_TX] = AIROHA_XGPON_TX_BURST_CNT,
	[AIROHA_XPON_MIB_PLOAMD_RX] = AIROHA_XGPON_RX_PLOAMD_CNT,
	[AIROHA_XPON_MIB_PLOAMU_TX] = AIROHA_XGPON_TX_PLOAMU_CNT,
	[AIROHA_XPON_MIB_XGEM_RX] = AIROHA_XGPON_RX_XGEM_CNT,
	[AIROHA_XPON_MIB_XGEM_TX] = AIROHA_XGPON_TX_XGEM_CNT,
};

void airoha_xpon_xgpon_counters_start(struct airoha_xpon *xpon)
{
	unsigned int i;

	lockdep_assert_held(&xpon->state_lock);
	for (i = 0; i < ARRAY_SIZE(airoha_xpon_xgpon_counter_registers); i++)
		xpon->stats.mib_last[i] = airoha_xgpon_read(
			xpon, airoha_xpon_xgpon_counter_registers[i]);
}

void airoha_xpon_xgpon_counters_update(struct airoha_xpon *xpon)
{
	unsigned int i;

	lockdep_assert_held(&xpon->state_lock);
	if (!xpon->rx_active || !xpon->mac_initialized)
		return;

	/* u32 subtraction accounts for hardware counter wrap across MAC activations. */
	for (i = 0; i < ARRAY_SIZE(airoha_xpon_xgpon_counter_registers); i++) {
		u32 value = airoha_xgpon_read(
			xpon, airoha_xpon_xgpon_counter_registers[i]);

		xpon->stats.mib_total[i] +=
			(u32)(value - xpon->stats.mib_last[i]);
		xpon->stats.mib_last[i] = value;
	}
}

void airoha_xpon_xgpon_stop_rx(struct airoha_xpon *xpon)
{
	u32 rx_ctrl;
	int ret;

	atomic_set(&xpon->tx_resync_pending, 0);
	if (xpon->xgpon_irq_enabled) {
		airoha_xgpon_write(xpon, AIROHA_XGPON_INT_ENABLE, 0);
		xpon->xgpon_irq_enabled = false;
		synchronize_irq(xpon->mac_irq);
	}

	airoha_xpon_xgpon_counters_update(xpon);
	ret = airoha_pcs_pon_stop_rx(xpon->pcs);
	if (ret)
		dev_warn_ratelimited(xpon->dev, "PON PMA PLUG_OUT failed: %d\n",
		                     ret);
	rx_ctrl = airoha_pon_phy_read(xpon, AIROHA_XGPON_PHY_RX_SYNC_CTRL);
	airoha_pon_phy_write(xpon, AIROHA_XGPON_PHY_RX_SYNC_CTRL,
	                     rx_ctrl & ~AIROHA_XGPON_PHY_RX_ENABLE);
	if (xpon->mac_initialized)
		airoha_xpon_xgpon_stop_mac_interfaces(xpon, "rx-deactivate");
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_SW_RST,
	                         AIROHA_XGPON_MAC_RESET_RELEASED, 0);
	airoha_xpon_clear_assignment(xpon);
	xpon->onu_state = AIROHA_XGPON_O1;
	xpon->mac_initialized = false;
	xpon->crypto.default_ploam_ik_programmed = false;
	xpon->crypto.activation_keys_programmed = false;
	xpon->crypto.o4_key_switch_observed = false;
	xpon->burst_profile_programmed = false;
	xpon->rx_active = false;
	xpon->lifecycle = AIROHA_XPON_WAIT_OPTICAL_SIGNAL;
}

void airoha_xpon_xgpon_restart_mac(struct airoha_xpon *xpon, const char *reason)
{
	airoha_xpon_xgpon_counters_update(xpon);
	atomic_set(&xpon->tx_resync_pending, 0);
	airoha_xgpon_write(xpon, AIROHA_XGPON_INT_ENABLE, 0);
	xpon->xgpon_irq_enabled = false;
	synchronize_irq(xpon->mac_irq);
	if (xpon->mac_initialized)
		airoha_xpon_xgpon_stop_mac_interfaces(xpon, reason);
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_SW_RST,
	                         AIROHA_XGPON_MAC_RESET_RELEASED, 0);
	airoha_xpon_clear_assignment(xpon);
	airoha_xpon_set_onu_state(xpon, AIROHA_XGPON_O1);
	xpon->mac_initialized = false;
	xpon->crypto.default_ploam_ik_programmed = false;
	xpon->crypto.activation_keys_programmed = false;
	xpon->crypto.o4_key_switch_observed = false;
	xpon->burst_profile_programmed = false;
	xpon->lifecycle = AIROHA_XPON_WAIT_LINE_SYNC;

	dev_info(xpon->dev, "XG-PON MAC epoch reset (%s); PHY RX retained\n",
	         reason);
}

static void airoha_xpon_xgpon_recover_no_sync(struct airoha_xpon *xpon,
                                              unsigned long *delay)
{
	unsigned long deadline;
	bool full_reinit;
	int ret;

	if (!xpon->no_sync_since)
		xpon->no_sync_since = jiffies;
	deadline = xpon->no_sync_since +
	           msecs_to_jiffies(AIROHA_XPON_NO_SYNC_RECOVERY_MS);
	if (time_before(jiffies, deadline)) {
		if (time_before(deadline, jiffies + *delay))
			*delay = deadline - jiffies;
		return;
	}

	xpon->stats.no_ready_recovery_count++;
	full_reinit = (xpon->stats.no_ready_recovery_count + 3) %
	                      AIROHA_XPON_FULL_REINIT_PERIOD ==
	              0;
	airoha_xpon_xgpon_stop_rx(xpon);
	if (full_reinit) {
		airoha_xpon_xgpon_hold_digital_rx_reset(xpon);
		xpon->pcs_profile_valid = false;
		xpon->stats.full_reinit_count++;
		dev_warn_ratelimited(
			xpon->dev,
			"optical signal present without XGTC sync; running full PON PHY reinitialization (recovery %u)\n",
			xpon->stats.no_ready_recovery_count);
	} else {
		dev_warn_ratelimited(
			xpon->dev,
			"optical signal present without XGTC sync; running short PMA recovery (recovery %u)\n",
			xpon->stats.no_ready_recovery_count);
	}

	ret = airoha_xpon_xgpon_start_rx(xpon);
	if (ret) {
		xpon->last_start_error = ret;
		xpon->lifecycle = AIROHA_XPON_ERROR;
		*delay = msecs_to_jiffies(AIROHA_XPON_RETRAIN_RETRY_MS);
	} else {
		xpon->last_start_error = 0;
	}
}

static void airoha_xpon_xgpon_advance_xgtc(struct airoha_xpon *xpon,
                                           unsigned long *delay)
{
	u32 sync = airoha_pon_phy_read(xpon, AIROHA_XGPON_PHY_DBG_RX_SYNC_ST);
	u32 sync_state = FIELD_GET(AIROHA_XGPON_PHY_RX_SYNC_MASK, sync);
	int ret;

	if (sync_state == AIROHA_XGPON_PHY_RX_SYNC_IN) {
		xpon->no_sync_since = 0;
		xpon->stats.no_ready_recovery_count = 0;
		if (!xpon->mac_initialized) {
			ret = airoha_xpon_xgpon_start_control(xpon);
			if (ret) {
				xpon->last_start_error = ret;
				xpon->lifecycle = AIROHA_XPON_ERROR;
				*delay = msecs_to_jiffies(
					AIROHA_XPON_RETRAIN_RETRY_MS);
				return;
			}
		}
		ret = airoha_xpon_try_arm_upstream(xpon);
		if (ret) {
			xpon->last_start_error = ret;
			dev_warn_ratelimited(
				xpon->dev,
				"failed to arm upstream BEN: %d; remaining receive-only and retrying\n",
				ret);
		}
		return;
	}

	if (xpon->mac_initialized) {
		unsigned long deadline;

		if (!xpon->no_sync_since)
			xpon->no_sync_since = jiffies;
		deadline = xpon->no_sync_since +
		           msecs_to_jiffies(AIROHA_XPON_SYNC_LOSS_CONFIRM_MS);
		if (time_before(jiffies, deadline)) {
			if (time_before(deadline, jiffies + *delay))
				*delay = deadline - jiffies;
			return;
		}
		dev_warn_ratelimited(
			xpon->dev,
			"XGTC sync lost; resetting MAC epoch while retaining PHY RX\n");
		airoha_xpon_xgpon_restart_mac(xpon, "PHY LOF");
		*delay = msecs_to_jiffies(AIROHA_XPON_LINK_POLL_MS);
		return;
	}

	airoha_xpon_xgpon_recover_no_sync(xpon, delay);
}

void airoha_xpon_xgpon_advance(struct airoha_xpon *xpon, u32 phy_events,
                               unsigned long *delay)
{
	int ret;

	if (atomic_xchg(&xpon->tx_resync_pending, 0) && xpon->mac_initialized &&
	    xpon->tx_armed) {
		xpon->stats.tx_resync_count++;
		ret = airoha_xpon_xgpon_resync_tx_locked(xpon);
		if (ret) {
			xpon->stats.tx_resync_failure_count++;
			dev_warn_ratelimited(
				xpon->dev, "XG-PON TX late resync failed: %d\n",
				ret);
		} else {
			dev_info_ratelimited(
				xpon->dev, "XG-PON TX late resync completed\n");
		}
	}

	if ((phy_events & AIROHA_XGPON_PHY_INT_RX_RDY) && xpon->rx_active &&
	    !xpon->mac_initialized)
		airoha_xpon_xgpon_stop_rx(xpon);

	if (!xpon->rx_active) {
		ret = airoha_xpon_xgpon_start_rx(xpon);
		if (ret) {
			xpon->last_start_error = ret;
			xpon->lifecycle = AIROHA_XPON_ERROR;
			*delay = msecs_to_jiffies(AIROHA_XPON_RETRAIN_RETRY_MS);
			dev_err_ratelimited(
				xpon->dev,
				"failed to start PON RX with optical signal present: %d; BEN remains disabled, retrying\n",
				ret);
		} else {
			xpon->last_start_error = 0;
		}
		return;
	}

	airoha_xpon_xgpon_advance_xgtc(xpon, delay);
}
