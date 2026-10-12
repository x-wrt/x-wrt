// SPDX-License-Identifier: GPL-2.0-only

#include <airoha-pon-frontend.h>
#include <linux/airoha-eth.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/netdevice.h>
#include <linux/seq_file.h>
#include <net/netlink.h>

#include "airoha-xpon-private.h"
#include "airoha-xpon-epon.h"

static const char *airoha_xpon_lifecycle_name(enum airoha_xpon_lifecycle state)
{
	switch (state) {
	case AIROHA_XPON_STOPPED:
		return "stopped";
	case AIROHA_XPON_WAIT_OPTICAL_SIGNAL:
		return "wait-optical-signal";
	case AIROHA_XPON_PMA_CONFIGURED:
		return "pma-configured";
	case AIROHA_XPON_WAIT_LINE_SYNC:
		return "wait-line-sync";
	case AIROHA_XPON_PROTOCOL_ACTIVATING:
		return "protocol-activating";
	case AIROHA_XPON_OPERATIONAL:
		return "operational";
	case AIROHA_XPON_ERROR:
		return "error";
	default:
		return "invalid";
	}
}

static const char *
airoha_xgpon_onu_state_name(enum airoha_xgpon_onu_state state)
{
	switch (state) {
	case AIROHA_XGPON_O1:
		return "O1";
	case AIROHA_XGPON_O2_3:
		return "O2_3";
	case AIROHA_XGPON_O4:
		return "O4";
	case AIROHA_XGPON_O5:
		return "O5";
	case AIROHA_XGPON_O7:
		return "O7";
	default:
		return "unknown";
	}
}

static const char *airoha_xgpon_sync_name(u32 state)
{
	switch (state) {
	case AIROHA_XGPON_PHY_RX_SYNC_HUNT:
		return "hunt";
	case AIROHA_XGPON_PHY_RX_SYNC_PRE:
		return "pre-sync";
	case AIROHA_XGPON_PHY_RX_SYNC_IN:
		return "in-sync";
	case AIROHA_XGPON_PHY_RX_SYNC_RE:
		return "re-sync";
	default:
		return "invalid";
	}
}

static const char *airoha_xpon_pcs_profile_name(enum airoha_pcs_pon_mode mode)
{
	static const char *const names[] = {
		"gpon",    "xgpon",       "xgspon",
		"epon-1g", "epon-10g-1g", "epon-10g-10g",
	};

	return mode < ARRAY_SIZE(names) ? names[mode] : "invalid";
}

static const char *
calibration_name(enum airoha_pon_frontend_calibration_state state)
{
	switch (state) {
	case AIROHA_PON_FRONTEND_CALIBRATION_READY:
		return "ready";
	case AIROHA_PON_FRONTEND_CALIBRATION_MISSING:
		return "missing";
	case AIROHA_PON_FRONTEND_CALIBRATION_INVALID:
		return "invalid";
	case AIROHA_PON_FRONTEND_CALIBRATION_NOT_REQUIRED:
		return "not-required";
	default:
		return "unknown";
	}
}

int airoha_xpon_put_status(struct sk_buff *reply, struct airoha_xpon *xpon)
{
	static const char *const mpcp_names[] = { "wait",
		                                  "registering",
		                                  "register-request",
		                                  "register-pending",
		                                  "registered",
		                                  "denied",
		                                  "unknown" };
	struct airoha_pon_frontend_diagnostics optics = {};
	struct nlattr *group;
	bool epon, active_epon;
	int optics_error, ret = -EMSGSIZE;

	/* Read I2C telemetry outside the line-state lock used by protocol IRQs. */
	optics_error =
		airoha_pon_frontend_get_diagnostics(xpon->frontend, &optics);
	mutex_lock(&xpon->state_lock);
	active_epon = xpon->active_mode_valid &&
	              airoha_xpon_mode_is_epon(xpon->active_mode);
	epon = airoha_xpon_mode_is_epon(xpon->active_mode_valid ?
	                                        xpon->active_mode :
	                                        xpon->configured_mode);

	group = nla_nest_start(reply, AIROHA_XPON_ATTR_LINE);
	if (!group)
		goto out;
	if (nla_put_string(reply, XPON_LINE_CONFIGURED_MODE,
	                   airoha_xpon_mode_name(xpon->configured_mode)))
		goto out;
	if (nla_put_string(reply, XPON_LINE_ACTIVE_MODE,
	                   xpon->active_mode_valid ?
	                           airoha_xpon_mode_name(xpon->active_mode) :
	                           "none"))
		goto out;
	if (nla_put_u8(reply, XPON_LINE_MODE_PENDING,
	               xpon->active_mode_valid &&
	                       xpon->configured_mode != xpon->active_mode))
		goto out;
	if (nla_put_string(reply, XPON_LINE_LIFECYCLE,
	                   airoha_xpon_lifecycle_name(xpon->lifecycle)))
		goto out;
	if (nla_put_s32(reply, XPON_LINE_LAST_START_ERROR,
	                xpon->last_start_error))
		goto out;
	if (nla_put_u8(reply, XPON_LINE_OPTICAL_SIGNAL, xpon->optical_signal))
		goto out;
	if (nla_put_u8(reply, XPON_LINE_RX_ACTIVE, xpon->rx_active))
		goto out;
	if (!epon &&
	    nla_put_u8(reply, XPON_LINE_PHY_READY,
	               xpon->rx_active &&
	                       !!(airoha_pon_phy_read(
					  xpon, AIROHA_XGPON_PHY_XG_PHY_STA) &
	                          AIROHA_XGPON_PHY_PHYA_READY)))
		goto out;
	if (nla_put_string(
		    reply, XPON_LINE_XGTC_SYNC,
		    epon ? "not-applicable" :
		    !xpon->rx_active ?
			   "hunt" :
			   airoha_xgpon_sync_name(FIELD_GET(
				   AIROHA_XGPON_PHY_RX_SYNC_MASK,
				   airoha_pon_phy_read(
					   xpon,
					   AIROHA_XGPON_PHY_DBG_RX_SYNC_ST)))))
		goto out;
	if (epon && nla_put_u8(reply, XPON_LINE_PCS_SYNC,
	                       active_epon && xpon->rx_active &&
	                               airoha_xpon_epon_pcs_synced(xpon)))
		goto out;
	if (nla_put_u8(reply, XPON_LINE_PCS_PROFILE_VALID,
	               xpon->pcs_profile_valid))
		goto out;
	if (xpon->pcs_profile_valid &&
	    nla_put_string(reply, XPON_LINE_PCS_PROFILE,
	                   airoha_xpon_pcs_profile_name(xpon->pcs_profile)))
		goto out;
	nla_nest_end(reply, group);

	group = nla_nest_start(reply, AIROHA_XPON_ATTR_FRONTEND);
	if (!group)
		goto out;
	if (nla_put_s32(reply, XPON_FRONTEND_ERROR, optics_error))
		goto out;
	if (nla_put_string(reply, XPON_FRONTEND_CALIBRATION,
	                   calibration_name(optics.calibration)))
		goto out;
	if ((!optics_error) && nla_put_u8(reply, XPON_FRONTEND_TX_GATE_ENABLED,
	                                  optics.tx_gate_enabled))
		goto out;
	if ((!optics_error &&
	     (optics.valid & AIROHA_PON_FRONTEND_DIAG_TEMPERATURE)) &&
	    nla_put_s32(reply, XPON_FRONTEND_TEMPERATURE_8472,
	                optics.temperature))
		goto out;
	if ((!optics_error &&
	     (optics.valid & AIROHA_PON_FRONTEND_DIAG_VOLTAGE)) &&
	    nla_put_u32(reply, XPON_FRONTEND_VOLTAGE_8472, optics.voltage))
		goto out;
	if ((!optics_error &&
	     (optics.valid & AIROHA_PON_FRONTEND_DIAG_TX_BIAS)) &&
	    nla_put_u32(reply, XPON_FRONTEND_TX_BIAS_8472, optics.tx_bias))
		goto out;
	if ((!optics_error &&
	     (optics.valid & AIROHA_PON_FRONTEND_DIAG_TX_POWER)) &&
	    nla_put_u32(reply, XPON_FRONTEND_TX_POWER_8472, optics.tx_power))
		goto out;
	if ((!optics_error &&
	     (optics.valid & AIROHA_PON_FRONTEND_DIAG_RX_POWER)) &&
	    nla_put_u32(reply, XPON_FRONTEND_RX_POWER_8472, optics.rx_power))
		goto out;
	nla_nest_end(reply, group);

	group = nla_nest_start(reply, AIROHA_XPON_ATTR_REGISTRATION);
	if (!group)
		goto out;
	if (nla_put_string(reply, XPON_REGISTRATION_ONU_STATE,
	                   epon ? "not-applicable" :
	                          airoha_xgpon_onu_state_name(xpon->onu_state)))
		goto out;
	if (nla_put_u8(reply, XPON_REGISTRATION_ONU_ID_VALID,
	               !epon && (xpon->onu_state == AIROHA_XGPON_O4 ||
	                         xpon->onu_state == AIROHA_XGPON_O5)))
		goto out;
	if ((!epon && (xpon->onu_state == AIROHA_XGPON_O4 ||
	               xpon->onu_state == AIROHA_XGPON_O5)) &&
	    nla_put_u32(reply, XPON_REGISTRATION_ONU_ID, xpon->onu_id))
		goto out;
	if (nla_put_string(
		    reply, XPON_REGISTRATION_MPCP_STATE,
		    epon ? mpcp_names[min_t(unsigned int, xpon->epon.mpcp_state,
	                                    ARRAY_SIZE(mpcp_names) - 1)] :
			   "not-applicable"))
		goto out;
	if ((epon) && nla_put_u8(reply, XPON_REGISTRATION_LLID_VALID,
	                         xpon->epon.llid_valid))
		goto out;
	if ((epon && xpon->epon.llid_valid) &&
	    nla_put_u32(reply, XPON_REGISTRATION_LLID, xpon->epon.llid))
		goto out;
	if (nla_put_u8(reply, XPON_REGISTRATION_UPSTREAM_TX_ARMED,
	               xpon->tx_armed))
		goto out;
	if (nla_put_u8(reply, XPON_REGISTRATION_SERIAL_CONFIGURED,
	               xpon->identity.active_identity.serial_configured))
		goto out;
	if (nla_put_u8(
		    reply, XPON_REGISTRATION_REGISTRATION_ID_CONFIGURED,
		    xpon->identity.active_identity.registration_id_configured))
		goto out;
	nla_nest_end(reply, group);

	group = nla_nest_start(reply, AIROHA_XPON_ATTR_DATAPATH);
	if (!group)
		goto out;
	if (nla_put_u8(reply, XPON_DATAPATH_DATA_PATH_CONFIGURED,
	               xpon->data_path.configured))
		goto out;
	if (nla_put_u8(reply, XPON_DATAPATH_SERVICE_READY, xpon->service_ready))
		goto out;
	nla_nest_end(reply, group);

	group = nla_nest_start(reply, AIROHA_XPON_ATTR_COUNTERS);
	if (!group)
		goto out;
	if (nla_put_u32(reply, XPON_COUNTERS_RX_START_COUNT,
	                xpon->stats.rx_start_count))
		goto out;
	if (!epon &&
	    nla_put_u64_64bit(reply, XPON_COUNTERS_XGTC_RX,
	                      xpon->stats.mib_total[AIROHA_XPON_MIB_XGTC_RX],
	                      XPON_COUNTERS_UNSPEC))
		goto out;
	if (!epon &&
	    nla_put_u64_64bit(reply, XPON_COUNTERS_UPSTREAM_BURSTS_TX,
	                      xpon->stats.mib_total[AIROHA_XPON_MIB_BURSTS_TX],
	                      XPON_COUNTERS_UNSPEC))
		goto out;
	if (!epon &&
	    nla_put_u64_64bit(reply, XPON_COUNTERS_PLOAMD_RX,
	                      xpon->stats.mib_total[AIROHA_XPON_MIB_PLOAMD_RX],
	                      XPON_COUNTERS_UNSPEC))
		goto out;
	if (!epon &&
	    nla_put_u64_64bit(reply, XPON_COUNTERS_PLOAMU_TX,
	                      xpon->stats.mib_total[AIROHA_XPON_MIB_PLOAMU_TX],
	                      XPON_COUNTERS_UNSPEC))
		goto out;
	if (!epon &&
	    nla_put_u64_64bit(reply, XPON_COUNTERS_XGEM_RX,
	                      xpon->stats.mib_total[AIROHA_XPON_MIB_XGEM_RX],
	                      XPON_COUNTERS_UNSPEC))
		goto out;
	if (!epon &&
	    nla_put_u64_64bit(reply, XPON_COUNTERS_XGEM_TX,
	                      xpon->stats.mib_total[AIROHA_XPON_MIB_XGEM_TX],
	                      XPON_COUNTERS_UNSPEC))
		goto out;
	if ((epon) &&
	    nla_put_u32(reply, XPON_COUNTERS_DISCOVERY_GATES,
	                atomic_read(&xpon->epon.discovery_gate_count)))
		goto out;
	if ((epon) &&
	    nla_put_u32(reply, XPON_COUNTERS_REGISTER_REQUESTS,
	                atomic_read(&xpon->epon.register_request_count)))
		goto out;
	if ((epon) && nla_put_u32(reply, XPON_COUNTERS_REGISTER_MESSAGES,
	                          xpon->epon.register_message_count))
		goto out;
	if ((epon) && nla_put_u32(reply, XPON_COUNTERS_REGISTER_ACKS,
	                          xpon->epon.register_ack_count))
		goto out;
	if ((epon) && nla_put_u32(reply, XPON_COUNTERS_REGISTER_NACKS,
	                          xpon->epon.nack_count))
		goto out;
	if ((epon) && nla_put_u32(reply, XPON_COUNTERS_MPCP_TIMEOUTS,
	                          xpon->epon.mpcp_timeout_count))
		goto out;
	if ((epon) && nla_put_u32(reply, XPON_COUNTERS_MAC_ERRORS,
	                          xpon->epon.mac_error_count))
		goto out;
	if (nla_put_u32(reply, XPON_COUNTERS_SYNC_LOSSES,
	                epon ? xpon->epon.sync_loss_count :
	                       xpon->stats.phy_lof_count))
		goto out;
	if (nla_put_u32(reply, XPON_COUNTERS_RECOVERIES,
	                epon ? xpon->epon.recovery_count :
	                       xpon->stats.no_ready_recovery_count))
		goto out;
	if (nla_put_u32(reply, XPON_COUNTERS_FULL_REINITIALIZATIONS,
	                epon ? xpon->epon.full_reinit_count :
	                       xpon->stats.full_reinit_count))
		goto out;
	nla_nest_end(reply, group);
	ret = 0;
out:
	mutex_unlock(&xpon->state_lock);
	return ret;
}

static int pon_line_debug_show(struct seq_file *m, void *unused)
{
	struct airoha_xpon *xpon = m->private;

	mutex_lock(&xpon->state_lock);
	seq_printf(m,
	           "configured_mode: %s\n"
	           "active_mode: %s\n"
	           "lifecycle: %s\n"
	           "last_start_error: %d\n"
	           "optical_signal: %d\n"
	           "rx_active: %d\n"
	           "upstream_tx_armed: %d\n"
	           "data_path_configured: %d\n"
	           "service_ready: %d\n"
	           "rx_start_count: %u\n"
	           "pcs_profile: %s\n",
	           airoha_xpon_mode_name(xpon->configured_mode),
	           xpon->active_mode_valid ?
	                   airoha_xpon_mode_name(xpon->active_mode) :
	                   "none",
	           airoha_xpon_lifecycle_name(xpon->lifecycle),
	           xpon->last_start_error, xpon->optical_signal,
	           xpon->rx_active, xpon->tx_armed, xpon->data_path.configured,
	           xpon->service_ready, xpon->stats.rx_start_count,
	           xpon->pcs_profile_valid ?
	                   airoha_xpon_pcs_profile_name(xpon->pcs_profile) :
	                   "none");
	mutex_unlock(&xpon->state_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pon_line_debug);

static int pon_frontend_debug_show(struct seq_file *m, void *unused)
{
	struct airoha_xpon *xpon = m->private;
	struct airoha_pon_frontend_diagnostics diagnostics = {};
	int ret;

	ret = airoha_pon_frontend_get_diagnostics(xpon->frontend, &diagnostics);
	seq_printf(m, "error: %d\ncalibration: %s\n", ret,
	           calibration_name(diagnostics.calibration));
	if (!ret) {
		seq_printf(m, "tx_gate_enabled: %d\n",
		           diagnostics.tx_gate_enabled);
		if (diagnostics.valid & AIROHA_PON_FRONTEND_DIAG_TEMPERATURE)
			seq_printf(m, "temperature_8472: 0x%04x\n",
			           (u16)diagnostics.temperature);
		if (diagnostics.valid & AIROHA_PON_FRONTEND_DIAG_VOLTAGE)
			seq_printf(m, "voltage_8472: 0x%04x\n",
			           diagnostics.voltage);
		if (diagnostics.valid & AIROHA_PON_FRONTEND_DIAG_TX_BIAS)
			seq_printf(m, "tx_bias_8472: 0x%04x\n",
			           diagnostics.tx_bias);
		if (diagnostics.valid & AIROHA_PON_FRONTEND_DIAG_TX_POWER)
			seq_printf(m, "tx_power_8472: 0x%04x\n",
			           diagnostics.tx_power);
		if (diagnostics.valid & AIROHA_PON_FRONTEND_DIAG_RX_POWER)
			seq_printf(m, "rx_power_8472: 0x%04x\n",
			           diagnostics.rx_power);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pon_frontend_debug);

static int pon_pcs_debug_show(struct seq_file *m, void *unused)
{
	struct airoha_xpon *xpon = m->private;
	struct airoha_pcs_pon_rx_status status = {};
	bool available, epon;

	mutex_lock(&xpon->state_lock);
	/* PMA and PCS registers describe the line only when the profile matches. */
	available = xpon->active_mode_valid && xpon->pcs_profile_valid &&
	            airoha_xpon_pcs_profile_matches(xpon->active_mode,
	                                            xpon->pcs_profile);
	seq_printf(m, "available: %d\nprofile: %s\nrx_active: %d\n", available,
	           xpon->pcs_profile_valid ?
	                   airoha_xpon_pcs_profile_name(xpon->pcs_profile) :
	                   "none",
	           xpon->rx_active);
	if (!available)
		goto out;
	epon = airoha_xpon_mode_is_epon(xpon->active_mode);
	if (airoha_pcs_pon_get_rx_status(xpon->pcs, &status))
		goto out;
	seq_printf(m,
	           "signal_detected: %d\n"
	           "rx_mode_raw: 0x%08x\n"
	           "freqdet_locked: %d\n"
	           "frequency_level: 0x%04x\n"
	           "frequency_target: 0x%04x\n"
	           "rx_ready_gate: %d\n"
	           "rx_fifo_reset_released: %d\n"
	           "calibration_stage: %u\n"
	           "calibration_error: %d\n"
	           "calibration_runs: %u\n"
	           "eo_gain: %u\n"
	           "eo_peaking: %u\n"
	           "eo_fom: %u\n",
	           status.signal_detected, status.rx_mode_raw,
	           status.freqdet_locked, status.frequency_level,
	           status.frequency_target, status.rx_ready_gate,
	           status.rx_fifo_reset_released, status.calibration_stage,
	           status.calibration_error, status.calibration_runs,
	           status.eo_gain, status.eo_peaking, status.eo_fom);
	if (epon) {
		seq_printf(m, "pcs_sync: %d\n",
		           airoha_xpon_epon_pcs_synced(xpon));
		airoha_xpon_epon_debug_pcs(xpon, m);
	} else {
		u32 sfp = airoha_pon_phy_read(xpon, AIROHA_XGPON_PHY_SFP_STA);
		u32 sync = FIELD_GET(
			AIROHA_XGPON_PHY_RX_SYNC_MASK,
			airoha_pon_phy_read(xpon,
		                            AIROHA_XGPON_PHY_DBG_RX_SYNC_ST));

		seq_printf(m,
		           "sfp_status: 0x%08x\n"
		           "phy_ready: %d\n"
		           "xgtc_sync: %s\n"
		           "xg_continue_ctrl: 0x%08x\n",
		           sfp,
		           !!(airoha_pon_phy_read(xpon,
		                                  AIROHA_XGPON_PHY_XG_PHY_STA) &
		              AIROHA_XGPON_PHY_PHYA_READY),
		           airoha_xgpon_sync_name(sync),
		           airoha_pon_phy_read(
				   xpon, AIROHA_XGPON_PHY_XG_CONTINUE_CTRL));
	}
out:
	mutex_unlock(&xpon->state_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pon_pcs_debug);

static int pon_mac_debug_show(struct seq_file *m, void *unused)
{
	struct airoha_xpon *xpon = m->private;
	bool available, epon;

	mutex_lock(&xpon->state_lock);
	/* Only the active MAC has a clock under the selected line mode. */
	available = xpon->active_mode_valid && xpon->mac_initialized;
	seq_printf(m, "available: %d\n", available);
	if (!available)
		goto out;
	epon = airoha_xpon_mode_is_epon(xpon->active_mode);
	if (epon) {
		airoha_xpon_epon_debug_mac(xpon, m);
	} else {
		seq_printf(m,
		           "interrupt_status: 0x%08x\n"
		           "interrupt_enable: 0x%08x\n"
		           "fifo_error: 0x%08x\n"
		           "tx_error: 0x%08x\n"
		           "rx_error: 0x%08x\n"
		           "mbi_mpi_stop: 0x%08x\n",
		           airoha_xgpon_read(xpon, AIROHA_XGPON_INT_STATUS),
		           airoha_xgpon_read(xpon, AIROHA_XGPON_INT_ENABLE),
		           airoha_xgpon_read(xpon, AIROHA_XGPON_FIFO_ERR_STS),
		           airoha_xgpon_read(xpon, AIROHA_XGPON_TX_ERR_STS),
		           airoha_xgpon_read(xpon, AIROHA_XGPON_RX_ERR_STS),
		           airoha_xgpon_read(xpon, AIROHA_XGPON_MBI_MPI_STOP));
	}
out:
	mutex_unlock(&xpon->state_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pon_mac_debug);

static int pon_crypto_debug_show(struct seq_file *m, void *unused)
{
	struct airoha_xpon *xpon = m->private;
	bool active;

	mutex_lock(&xpon->state_lock);
	active = xpon->rx_active &&
	         !airoha_xpon_mode_is_epon(xpon->active_mode);
	seq_printf(m, "active: %d\n", active);
	if (active)
		seq_printf(m,
		           "default_ploam_ik_programmed: %d\n"
		           "activation_keys_programmed: %d\n"
		           "key_exchange_state: %u\n"
		           "key_exchange_errors: %u\n"
		           "current_key_index: 0x%08x\n",
		           xpon->crypto.default_ploam_ik_programmed,
		           xpon->crypto.activation_keys_programmed,
		           xpon->crypto.exchange_state,
		           xpon->crypto.key_exchange_error_count,
		           airoha_xgpon_read(xpon, AIROHA_XGPON_CUR_KIDX));
	mutex_unlock(&xpon->state_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pon_crypto_debug);

static int pon_datapath_debug_show(struct seq_file *m, void *unused)
{
	struct airoha_xpon *xpon = m->private;
	struct airoha_pon_datapath_stats stats = {};
	unsigned int channel;
	int ret;

	ret = airoha_eth_pon_get_datapath_stats(xpon->pon_netdev, &stats);
	if (ret)
		return ret;
	seq_printf(m,
	           "tx_packets_submitted: %llu\n"
	           "tx_packets_completed: %llu\n"
	           "tx_packets_dropped: %llu\n"
	           "tx_no_mapping: %llu\n"
	           "rx_packets_delivered: %llu\n"
	           "rx_descriptors_dropped: %llu\n"
	           "rx_crc_errors: %llu\n",
	           (unsigned long long)stats.tx_packets_submitted,
	           (unsigned long long)stats.tx_packets_completed,
	           (unsigned long long)(stats.tx_dropped +
	                                stats.tx_packets_hw_dropped),
	           (unsigned long long)stats.tx_no_mapping,
	           (unsigned long long)stats.rx_packets_delivered,
	           (unsigned long long)stats.rx_descriptors_dropped,
	           (unsigned long long)stats.rx_crc_errors);
	for (channel = 0; channel < AIROHA_PON_MAX_QDMA_CHANNELS; channel++) {
		if (!stats.tx_channel_packets[channel] &&
		    !stats.rx_channel_packets[channel])
			continue;
		seq_printf(
			m,
			"channel_%u_tx_packets: %llu\n"
			"channel_%u_rx_packets: %llu\n",
			channel,
			(unsigned long long)stats.tx_channel_packets[channel],
			channel,
			(unsigned long long)stats.rx_channel_packets[channel]);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pon_datapath_debug);

static int pon_registration_debug_show(struct seq_file *m, void *unused)
{
	struct airoha_xpon *xpon = m->private;
	bool epon;

	mutex_lock(&xpon->state_lock);
	epon = airoha_xpon_mode_is_epon(xpon->active_mode_valid ?
	                                        xpon->active_mode :
	                                        xpon->configured_mode);
	if (epon) {
		seq_printf(
			m,
			"mpcp_state: %u\n"
			"llid_valid: %d\n"
			"llid: %u\n"
			"register_requests: %u\n"
			"register_messages: %u\n"
			"register_acks: %u\n"
			"register_nacks: %u\n"
			"reregisters: %u\n"
			"deregisters: %u\n"
			"mpcp_timeouts: %u\n"
			"sync_losses: %u\n"
			"recoveries: %u\n"
			"full_reinitializations: %u\n"
			"registered_lifetime_ms: %u\n"
			"last_registration_lifetime_ms: %u\n"
			"last_register_status: 0x%08x\n",
			xpon->epon.mpcp_state, xpon->epon.llid_valid,
			xpon->epon.llid,
			atomic_read(&xpon->epon.register_request_count),
			xpon->epon.register_message_count,
			xpon->epon.register_ack_count, xpon->epon.nack_count,
			xpon->epon.reregister_count,
			xpon->epon.deregister_count,
			xpon->epon.mpcp_timeout_count,
			xpon->epon.sync_loss_count, xpon->epon.recovery_count,
			xpon->epon.full_reinit_count,
			xpon->epon.registered_since ?
				jiffies_to_msecs(jiffies -
		                                 xpon->epon.registered_since) :
				0,
			jiffies_to_msecs(xpon->epon.last_registration_lifetime),
			xpon->epon.last_register_status);
	} else {
		seq_printf(m,
		           "onu_state: %s\n"
		           "onu_id: %u\n"
		           "serial_configured: %d\n"
		           "registration_id_configured: %d\n"
		           "ploamd_accepted: %u\n"
		           "ploamd_rejected: %u\n",
		           airoha_xgpon_onu_state_name(xpon->onu_state),
		           xpon->onu_id,
		           xpon->identity.active_identity.serial_configured,
		           xpon->identity.active_identity
		                   .registration_id_configured,
		           xpon->stats.ploamd_accepted,
		           xpon->stats.ploamd_rejected);
	}
	mutex_unlock(&xpon->state_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pon_registration_debug);

void airoha_xpon_debugfs_init(struct airoha_xpon *xpon)
{
	char name[64];

	/* Name the debug directory after the PON netdev to identify each line. */
	snprintf(name, sizeof(name), "airoha-xpon-%s", xpon->pon_netdev->name);
	xpon->debug_dir = debugfs_create_dir(name, NULL);
	debugfs_create_file("line", 0400, xpon->debug_dir, xpon,
	                    &pon_line_debug_fops);
	debugfs_create_file("frontend", 0400, xpon->debug_dir, xpon,
	                    &pon_frontend_debug_fops);
	debugfs_create_file("pcs", 0400, xpon->debug_dir, xpon,
	                    &pon_pcs_debug_fops);
	debugfs_create_file("mac", 0400, xpon->debug_dir, xpon,
	                    &pon_mac_debug_fops);
	debugfs_create_file("crypto", 0400, xpon->debug_dir, xpon,
	                    &pon_crypto_debug_fops);
	debugfs_create_file("registration", 0400, xpon->debug_dir, xpon,
	                    &pon_registration_debug_fops);
	debugfs_create_file("datapath", 0400, xpon->debug_dir, xpon,
	                    &pon_datapath_debug_fops);
}

void airoha_xpon_debugfs_remove(struct airoha_xpon *xpon)
{
	debugfs_remove_recursive(xpon->debug_dir);
}
