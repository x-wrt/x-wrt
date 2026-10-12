// SPDX-License-Identifier: GPL-2.0-only
#include <linux/airoha-eth.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/netdevice.h>
#include <linux/random.h>
#include <linux/unaligned.h>

#include "airoha-xpon-omci.h"
#include "airoha-xpon-private.h"

void airoha_xpon_disarm_upstream(struct airoha_xpon *xpon, const char *reason)
{
	int ret;

	/* The external burst gate covers software PLOAMu and MAC O2/O4 replies. */
	WRITE_ONCE(xpon->tx_armed, false);
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_INT_ENABLE,
	                         AIROHA_XGPON_INT_TX_ERROR, 0);
	airoha_xpon_update_omci_link(xpon);
	ret = airoha_pon_frontend_set_tx_enable(xpon->frontend, false);
	if (ret)
		dev_warn_ratelimited(
			xpon->dev,
			"failed to disable optical frontend burst gate (%s): %d\n",
			reason, ret);
}

int airoha_xpon_try_arm_upstream(struct airoha_xpon *xpon)
{
	u32 phy_status, sync;
	u32 sync_state;
	int ret;

	if (READ_ONCE(xpon->tx_armed))
		return 0;
	if (READ_ONCE(xpon->stopping) || !xpon->optical_signal ||
	    !xpon->mac_initialized ||
	    !xpon->crypto.default_ploam_ik_programmed ||
	    !xpon->crypto.activation_keys_programmed ||
	    !xpon->identity.active_identity.serial_configured ||
	    !READ_ONCE(xpon->burst_profile_programmed))
		return 0;
	if (xpon->onu_state != AIROHA_XGPON_O2_3 &&
	    xpon->onu_state != AIROHA_XGPON_O4 &&
	    xpon->onu_state != AIROHA_XGPON_O5)
		return 0;

	phy_status = airoha_pon_phy_read(xpon, AIROHA_XGPON_PHY_XG_PHY_STA);
	sync = airoha_pon_phy_read(xpon, AIROHA_XGPON_PHY_DBG_RX_SYNC_ST);
	sync_state = FIELD_GET(AIROHA_XGPON_PHY_RX_SYNC_MASK, sync);
	if (!(phy_status & AIROHA_XGPON_PHY_PHYA_READY) ||
	    sync_state != AIROHA_XGPON_PHY_RX_SYNC_IN)
		return 0;
	/* DBG_RESYNC[31] reports TX synchronization before opening the burst gate. */
	if (!(airoha_xgpon_read(xpon, AIROHA_XGPON_DBG_RESYNC) &
	      AIROHA_XGPON_TX_SYNC_READY))
		return 0;

	/* TX-late monitoring starts with a valid burst profile. */
	airoha_xgpon_write(xpon, AIROHA_XGPON_TX_ERR_STS, U32_MAX);
	airoha_xgpon_write(xpon, AIROHA_XGPON_INT_STATUS,
	                   AIROHA_XGPON_INT_TX_ERROR);
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_INT_ENABLE,
	                         AIROHA_XGPON_INT_TX_ERROR,
	                         AIROHA_XGPON_INT_TX_ERROR);
	/* MAC grants and PMA BEN jointly control optical bursts. */
	ret = airoha_pon_frontend_set_tx_enable(xpon->frontend, true);
	if (ret) {
		airoha_xgpon_update_bits(xpon, AIROHA_XGPON_INT_ENABLE,
		                         AIROHA_XGPON_INT_TX_ERROR, 0);
		return ret;
	}
	WRITE_ONCE(xpon->tx_armed, true);
	airoha_xpon_update_omci_link(xpon);
	dev_info(
		xpon->dev,
		"upstream TX enabled: SN=%s, PHY/XGTC/profile ready, frontend enable accepted\n",
		xpon->identity.active_identity.serial_source);

	return 0;
}

void airoha_xpon_clear_assignment(struct airoha_xpon *xpon)
{
	/* Withdraw OMCI, burst gate, data path, and ONU-ID in that order. */
	airoha_xpon_omci_set_link(xpon->omci, false, AIROHA_XGPON_BROADCAST_ID,
	                          0);
	airoha_xpon_disarm_upstream(xpon, "clear-assignment");
	if (xpon->pon_netdev) {
		airoha_xpon_set_data_path_link(xpon, false);
		airoha_eth_pon_configure(xpon->pon_netdev, 0, 0, false);
	}

	airoha_xgpon_update_bits(
		xpon, AIROHA_XGPON_ONU_ID,
		AIROHA_XGPON_ONU_ID_VALID | AIROHA_XGPON_ONU_ID_MASK, 0x3ff);
	airoha_xpon_reset_data_key_exchange(xpon);
	xpon->onu_id = 0x3ff;
	xpon->eqd = 0;
	xpon->service_ready = false;
	xpon->data_path.count = 0;
	WRITE_ONCE(xpon->data_path.pending_count, 0);
	atomic_set(&xpon->data_path_retry_pending, 0);
	xpon->data_path.configured = false;
}

/*
 * FIFO used is eight bits. Twenty complete messages exceed that count, so
 * one drain covers the representable depth. Each RDATA read pops a word.
 */
#define AIROHA_XGPON_PLOAMD_PROFILE              0x01
#define AIROHA_XGPON_PLOAMD_ASSIGN_ONU_ID        0x03
#define AIROHA_XGPON_PLOAMD_RANGING_TIME         0x04
#define AIROHA_XGPON_PLOAMD_DEACTIVATE           0x05
#define AIROHA_XGPON_PLOAMD_REQUEST_REGISTRATION 0x09
#define AIROHA_XGPON_PLOAMD_ASSIGN_ALLOC_ID      0x0a
#define AIROHA_XGPON_PLOAMD_KEY_CONTROL          0x0d
#define AIROHA_XGPON_PLOAMU_REGISTRATION         0x02
#define AIROHA_XGPON_PLOAMU_KEY_REPORT           0x05
#define AIROHA_XGPON_PLOAMU_ACKNOWLEDGE          0x09
#define AIROHA_XGPON_PLOAM_ACK_OK                0x00
#define AIROHA_XGPON_PLOAM_ACK_PROCESS_ERROR     0x01
#define AIROHA_XGPON_KEY_CONTROL_GENERATE        0
#define AIROHA_XGPON_KEY_CONTROL_CONFIRM         1
#define AIROHA_XGPON_DATA_KEY_BYTES              16

static int airoha_xpon_wait_command(struct airoha_xpon *xpon, u32 reg,
                                    u32 done_mask)
{
	u32 value;

	/* T-CONT/GEM table commands use the hardware path's 3000 us timeout. */
	return readl_poll_timeout_atomic(xpon->xgpon_base + reg, value,
	                                 value & done_mask, 1, 3000);
}

static int airoha_xpon_set_tcont(struct airoha_xpon *xpon, u8 index,
                                 u16 alloc_id, bool valid)
{
	u32 command;

	if (index >= 32 || alloc_id > 0x3fff)
		return -EINVAL;

	command = AIROHA_XGPON_TCONT_CMD_WRITE |
	          FIELD_PREP(AIROHA_XGPON_TCONT_INDEX_MASK, index) |
	          FIELD_PREP(AIROHA_XGPON_ALLOC_ID_MASK, alloc_id);
	if (valid)
		command |= AIROHA_XGPON_TCONT_VALID;

	/* Command bit 31 starts the shared table port; ploam_lock serializes writes. */
	airoha_xgpon_write(xpon, AIROHA_XGPON_TCONT_ID_CFG, command);
	return airoha_xpon_wait_command(xpon, AIROHA_XGPON_TCONT_ID_STS,
	                                AIROHA_XGPON_TCONT_CMD_DONE);
}

static int airoha_xpon_get_tcont(struct airoha_xpon *xpon, u8 index,
                                 u16 *alloc_id, bool *valid)
{
	u32 status;
	int ret;

	if (index >= 32)
		return -EINVAL;

	airoha_xgpon_write(xpon, AIROHA_XGPON_TCONT_ID_CFG,
	                   FIELD_PREP(AIROHA_XGPON_TCONT_INDEX_MASK, index));
	ret = airoha_xpon_wait_command(xpon, AIROHA_XGPON_TCONT_ID_STS,
	                               AIROHA_XGPON_TCONT_CMD_DONE);
	if (ret)
		return ret;

	status = airoha_xgpon_read(xpon, AIROHA_XGPON_TCONT_ID_STS);
	*alloc_id = FIELD_GET(AIROHA_XGPON_ALLOC_ID_MASK, status);
	*valid = !!(status & AIROHA_XGPON_TCONT_VALID);
	return 0;
}

/*
 * T-CONT 0 shadows ONU-ID; ordinary Alloc-IDs start at index 1. first_free,
 * when given, receives the first unused index or 0xff.
 */
static int airoha_xpon_find_tcont(struct airoha_xpon *xpon, u16 alloc_id,
                                  u8 *index, u8 *first_free)
{
	unsigned int i;

	if (first_free)
		*first_free = 0xff;
	for (i = 1; i < 32; i++) {
		u16 current_id;
		bool valid;
		int ret;

		ret = airoha_xpon_get_tcont(xpon, i, &current_id, &valid);
		if (ret)
			return ret;
		if (valid && current_id == alloc_id) {
			*index = i;
			return 0;
		}
		if (!valid && first_free && *first_free == 0xff)
			*first_free = i;
	}

	return -ENOENT;
}

static int airoha_xpon_find_or_create_tcont(struct airoha_xpon *xpon,
                                            u16 alloc_id, u8 *index)
{
	u8 first_free;
	int ret;

	ret = airoha_xpon_find_tcont(xpon, alloc_id, index, &first_free);
	if (ret != -ENOENT)
		return ret;
	if (first_free == 0xff)
		return -ENOSPC;
	if (airoha_xpon_set_tcont(xpon, first_free, alloc_id, true))
		return -ETIMEDOUT;

	*index = first_free;
	return 0;
}

void airoha_xpon_clear_tconts(struct airoha_xpon *xpon)
{
	unsigned int i;
	int ret;

	/*
	 * Deactivate releases every non-default Alloc-ID; MAC reset is not known
	 * to clear this table.
	 */
	mutex_lock(&xpon->ploam_lock);
	for (i = 1; i < 32; i++) {
		ret = airoha_xpon_set_tcont(xpon, i, 0x3ff, false);
		if (ret) {
			dev_warn(xpon->dev, "T-CONT %u clear failed: %d\n", i,
			         ret);
			break;
		}
	}
	mutex_unlock(&xpon->ploam_lock);
}

static int airoha_xpon_remove_tcont(struct airoha_xpon *xpon, u16 alloc_id)
{
	u8 index;
	int ret;

	ret = airoha_xpon_find_tcont(xpon, alloc_id, &index, NULL);
	if (ret)
		return ret;
	return airoha_xpon_set_tcont(xpon, index, 0x3ff, false);
}

static int airoha_xpon_set_gem(struct airoha_xpon *xpon, u16 gem_id, bool valid,
                               bool unicast)
{
	u32 command;

	command = AIROHA_XGPON_GEM_CMD_WRITE |
	          FIELD_PREP(AIROHA_XGPON_GEM_ID_MASK, gem_id);
	if (valid)
		command |= AIROHA_XGPON_GEM_VALID;
	if (unicast)
		command |= AIROHA_XGPON_GEM_UNICAST;

	/* Key_Control installs the downstream key; GEM settings control US_ENCRYPT. */
	airoha_xgpon_write(xpon, AIROHA_XGPON_GEM_PORT_CFG, command);
	return airoha_xpon_wait_command(xpon, AIROHA_XGPON_GEM_PORT_STS,
	                                AIROHA_XGPON_GEM_CMD_DONE);
}

static bool
airoha_xpon_data_path_has_gem(const struct airoha_xpon_data_path_entry *entries,
                              unsigned int count, u16 gem_id)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (entries[i].gem_id == gem_id)
			return true;

	return false;
}

static int airoha_xpon_apply_data_paths_locked(
	struct airoha_xpon *xpon,
	const struct airoha_xpon_data_path_entry *requested, unsigned int count)
{
	struct airoha_xpon_data_path_entry next[AIROHA_XPON_MAX_DATA_PATHS];
	struct airoha_pon_flow_config flows[AIROHA_XPON_MAX_DATA_PATHS];
	unsigned int flow_count = 0;
	unsigned int i, j;
	int ret = 0;

	lockdep_assert_held(&xpon->state_lock);
	if (!xpon->mac_initialized || xpon->onu_state != AIROHA_XGPON_O5)
		return -ENOLINK;

	mutex_lock(&xpon->ploam_lock);
	/*
	 * Only PLOAM assigns non-default Alloc-IDs. The OMCI MIB survives
	 * Deactivate, so its T-CONTs wait for this epoch's Assign_Alloc-ID
	 * instead of answering grants the OLT has not issued to this ONU.
	 */
	for (i = 0; i < count; i++) {
		next[i] = requested[i];
		if (next[i].multicast) {
			next[i].tcont = 0xff;
			continue;
		}
		if (next[i].alloc_id == xpon->onu_id)
			ret = airoha_xpon_find_or_create_tcont(
				xpon, next[i].alloc_id, &next[i].tcont);
		else
			ret = airoha_xpon_find_tcont(xpon, next[i].alloc_id,
			                             &next[i].tcont, NULL);
		if (ret == -ENOENT) {
			if (!xpon->data_path.pending_count)
				dev_info(xpon->dev,
				         "data paths deferred: count=%u, Alloc-ID %u not assigned by PLOAM\n",
				         count, next[i].alloc_id);
			memcpy(xpon->data_path.pending, requested,
			       sizeof(requested[0]) * count);
			WRITE_ONCE(xpon->data_path.pending_count, count);
			ret = -EAGAIN;
			goto out_ploam;
		}
		if (ret)
			goto out_ploam;
	}
	WRITE_ONCE(xpon->data_path.pending_count, 0);

	/* Keep Linux TX queues stopped until the complete flow table is active. */
	airoha_xpon_set_data_path_link(xpon, false);
	for (i = 0; i < count; i++) {
		ret = airoha_xpon_set_gem(xpon, next[i].gem_id, true,
		                          !next[i].multicast);
		if (ret)
			goto out_ploam;
		if (next[i].multicast)
			continue;
		flows[flow_count].tcont = next[i].tcont;
		flows[flow_count].gem_id = next[i].gem_id;
		flows[flow_count].vlan_id = next[i].vlan_id;
		flows[flow_count].pbit_mask = next[i].pbit_mask;
		flow_count++;
	}

	ret = airoha_eth_pon_replace_flows(xpon->pon_netdev, flows, flow_count);
	if (ret)
		goto out_ploam;

	/*
	 * QDMA flow replacement publishes the new path atomically. Release the old
	 * GEM afterward; the GEM table write already updated matching GEM IDs.
	 */
	for (i = 0; i < xpon->data_path.count; i++) {
		const struct airoha_xpon_data_path_entry *old =
			&xpon->data_path.entries[i];

		if (!airoha_xpon_data_path_has_gem(next, count, old->gem_id))
			airoha_xpon_set_gem(xpon, old->gem_id, false,
			                    !old->multicast);
	}
	memcpy(xpon->data_path.entries, next, sizeof(next[0]) * count);
	xpon->data_path.count = count;
	for (j = 0; j < count && next[j].multicast; j++)
		;
	if (j < count) {
		xpon->data_path.alloc_id = next[j].alloc_id;
		xpon->data_path.gem_id = next[j].gem_id;
		xpon->data_path.tcont = next[j].tcont;
	} else {
		xpon->data_path.alloc_id = 0;
		xpon->data_path.gem_id = next[0].gem_id;
		xpon->data_path.tcont = 0;
	}
	xpon->data_path.configured = true;
	airoha_xpon_set_data_path_link(xpon, true);
	dev_info(xpon->dev, "data paths mapped: count=%u\n", count);

out_ploam:
	mutex_unlock(&xpon->ploam_lock);
	return ret;
}

int airoha_xpon_replace_data_paths(
	struct airoha_xpon *xpon,
	const struct airoha_xpon_data_path_entry *requested, unsigned int count)
{
	unsigned int i, j;
	int ret;

	if (!count || count > AIROHA_XPON_MAX_DATA_PATHS)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if ((!requested[i].multicast &&
		     requested[i].alloc_id > 0x3fff) ||
		    (requested[i].multicast &&
		     requested[i].alloc_id != 0xffff) ||
		    (requested[i].vlan_id > 4094 &&
		     requested[i].vlan_id != AIROHA_PON_VID_ANY) ||
		    !requested[i].pbit_mask)
			return -EINVAL;
		/*
		 * VID_ANY covers untagged frames and tagged frames after VID lookup.
		 * Equal VID/P-bit selectors conflict.
		 */
		for (j = 0; !requested[i].multicast && j < i; j++)
			if (!requested[j].multicast &&
			    requested[i].vlan_id == requested[j].vlan_id &&
			    requested[i].pbit_mask & requested[j].pbit_mask)
				return -EINVAL;
	}

	mutex_lock(&xpon->state_lock);
	ret = airoha_xpon_apply_data_paths_locked(xpon, requested, count);
	mutex_unlock(&xpon->state_lock);
	return ret;
}

void airoha_xpon_retry_data_paths(struct airoha_xpon *xpon)
{
	struct airoha_xpon_data_path_entry pending[AIROHA_XPON_MAX_DATA_PATHS];
	unsigned int count = xpon->data_path.pending_count;
	int ret;

	lockdep_assert_held(&xpon->state_lock);
	if (!count)
		return;

	memcpy(pending, xpon->data_path.pending, sizeof(pending[0]) * count);
	ret = airoha_xpon_apply_data_paths_locked(xpon, pending, count);
	if (ret) {
		if (ret != -EAGAIN)
			dev_warn(xpon->dev, "deferred data paths failed: %d\n",
			         ret);
		return;
	}

	/*
	 * Only unicast lookups defer, so the applied set carries a service GEM.
	 * pond cleared service_ready on -EAGAIN and learns of this apply late.
	 */
	xpon->service_ready = true;
	airoha_xpon_leds_update(xpon);
}

int airoha_xpon_configure_data_path(struct airoha_xpon *xpon, u16 alloc_id,
                                    u16 gem_id)
{
	struct airoha_xpon_data_path_entry path = {
		.alloc_id = alloc_id,
		.gem_id = gem_id,
		.vlan_id = AIROHA_PON_VID_ANY,
		.pbit_mask = 0xff,
	};

	return airoha_xpon_replace_data_paths(xpon, &path, 1);
}

int airoha_xpon_clear_data_path(struct airoha_xpon *xpon)
{
	int ret = 0;

	/* OMCI Delete and MIB reset close TX, QDMA mapping, then GEM entries.
	 * PLOAM Assign_Alloc-ID retains ownership of Alloc-IDs.
	 */
	mutex_lock(&xpon->state_lock);
	WRITE_ONCE(xpon->data_path.pending_count, 0);
	if (!xpon->data_path.configured)
		goto out_state;

	airoha_xpon_set_data_path_link(xpon, false);
	mutex_lock(&xpon->ploam_lock);
	ret = airoha_eth_pon_replace_flows(xpon->pon_netdev, NULL, 0);
	if (!ret) {
		unsigned int i;

		for (i = 0; i < xpon->data_path.count; i++) {
			ret = airoha_xpon_set_gem(
				xpon, xpon->data_path.entries[i].gem_id, false,
				!xpon->data_path.entries[i].multicast);
			if (ret)
				break;
		}
	}
	if (!ret) {
		xpon->data_path.count = 0;
		xpon->data_path.alloc_id = 0;
		xpon->data_path.gem_id = 0;
		xpon->data_path.tcont = 0;
		xpon->data_path.configured = false;
		xpon->service_ready = false;
		airoha_xpon_leds_update(xpon);
		dev_info(xpon->dev, "data path cleared\n");
	}
	mutex_unlock(&xpon->ploam_lock);
out_state:
	mutex_unlock(&xpon->state_lock);
	return ret;
}

static int airoha_xpon_program_profile(struct airoha_xpon *xpon,
                                       const u8 *message)
{
	u8 index = message[4] & 0x03;
	u8 line_rate = (message[4] >> 2) & 0x01;
	u8 version = message[4] >> 4;
	u8 delimiter_len = message[6] & 0x0f;
	u8 preamble_len = message[15] & 0x0f;
	u8 repeat = message[16] & 0x1f;
	u32 length, psbu, valid_reg, length_reg, pon_tag0, pon_tag1;
	u32 preamble_reg, delimiter_reg;
	int ret;

	if (line_rate || delimiter_len > 8 || preamble_len > 8)
		return -EINVAL;

	length = preamble_len * repeat + delimiter_len;

	/* SN grants can arrive during profile setup; publish profile valid last. */
	valid_reg = airoha_xgpon_read(xpon, AIROHA_XGPON_US_PROF_VLD);
	valid_reg &= ~AIROHA_XGPON_PROFILE_VERSION_MASK(index);
	valid_reg |= AIROHA_XGPON_PROFILE_VALID(index) |
	             (u32)version << (index * 8 + 4);

	length_reg = index < 2 ? AIROHA_XGPON_US_PROF_LEN_01 :
	                         AIROHA_XGPON_US_PROF_LEN_23;
	if (index & 1)
		airoha_xgpon_update_bits(xpon, length_reg, GENMASK(31, 16),
		                         length << 16);
	else
		airoha_xgpon_update_bits(xpon, length_reg, GENMASK(15, 0),
		                         length);

	/*
	 * The digital PHY stores complete preamble/delimiter patterns. Wire byte 0
	 * maps to the register's high byte.
	 */
	preamble_reg = AIROHA_XGPON_PHY_PREAMBLE_BASE +
	               index * AIROHA_XGPON_PHY_PROFILE_STRIDE;
	delimiter_reg = AIROHA_XGPON_PHY_DELIMITER_BASE +
	                index * AIROHA_XGPON_PHY_PROFILE_STRIDE;
	airoha_pon_phy_write(xpon, preamble_reg,
	                     airoha_xpon_bytes_to_u32(message + 17));
	airoha_pon_phy_write(xpon, preamble_reg + 4,
	                     airoha_xpon_bytes_to_u32(message + 21));
	airoha_pon_phy_write(xpon, delimiter_reg,
	                     airoha_xpon_bytes_to_u32(message + 7));
	airoha_pon_phy_write(xpon, delimiter_reg + 4,
	                     airoha_xpon_bytes_to_u32(message + 11));
	airoha_pon_phy_update_bits(
		xpon, AIROHA_XGPON_PHY_TX_FEC_CTRL,
		AIROHA_XGPON_PHY_FEC_ENABLE(index),
		message[5] & BIT(0) ? AIROHA_XGPON_PHY_FEC_ENABLE(index) : 0);
	psbu = FIELD_PREP(AIROHA_XGPON_PHY_PSBU_DELIMITER_MASK, delimiter_len) |
	       FIELD_PREP(AIROHA_XGPON_PHY_PSBU_PREAMBLE_MASK, preamble_len) |
	       FIELD_PREP(AIROHA_XGPON_PHY_PSBU_REPEAT_MASK, repeat);
	airoha_pon_phy_write(xpon,
	                     AIROHA_XGPON_PHY_PSBU_INFO_BASE +
	                             index * AIROHA_XGPON_PHY_PSBU_STRIDE,
	                     psbu);

	/* TAG_0 holds the final four PON Tag bytes. */
	pon_tag0 = airoha_xpon_bytes_to_u32(message + 29);
	pon_tag1 = airoha_xpon_bytes_to_u32(message + 25);
	airoha_xgpon_write(xpon, AIROHA_XGPON_PON_TAG_0, pon_tag0);
	airoha_xgpon_write(xpon, AIROHA_XGPON_PON_TAG_1, pon_tag1);
	if (xpon->onu_state == AIROHA_XGPON_O2_3 &&
	    (!xpon->crypto.activation_keys_programmed ||
	     xpon->crypto.activation_pon_tag0 != pon_tag0 ||
	     xpon->crypto.activation_pon_tag1 != pon_tag1)) {
		/* Derive activation keys from the PON Tag of this profile epoch. */
		ret = airoha_xpon_program_activation_keys(xpon);
		if (ret)
			return ret;
	}

	xpon->burst_profile_programmed = true;

	/* Profile validity follows integrity keys, TX sync, and BEN preparation. */
	ret = airoha_xpon_try_arm_upstream(xpon);
	if (ret || !READ_ONCE(xpon->tx_armed)) {
		xpon->burst_profile_programmed = false;
		return ret ?: -EAGAIN;
	}

	/* Clear stale W1C errors before publishing profile validity. */
	airoha_xpon_clear_mac_errors(xpon, true);
	airoha_xgpon_write(xpon, AIROHA_XGPON_US_PROF_VLD, valid_reg);

	return 0;
}

static int airoha_xpon_send_ploamu(struct airoha_xpon *xpon, const u8 *message)
{
	u32 status;
	unsigned int i;

	if (!xpon->tx_armed) {
		xpon->stats.ploamu_blocked++;
		return -EACCES;
	}

	status = airoha_xgpon_read(xpon, AIROHA_XGPON_PLOAMU_FIFO_STS);
	if (status & AIROHA_XGPON_PLOAMU_FIFO_OVERRUN)
		return -EOVERFLOW;
	if (FIELD_GET(AIROHA_XGPON_PLOAMU_FIFO_AVAIL_MASK, status) <
	    AIROHA_XGPON_PLOAMU_WORDS)
		return -ENOSPC;

	for (i = 0; i < AIROHA_XGPON_PLOAMU_WORDS; i++)
		airoha_xgpon_write(xpon, AIROHA_XGPON_PLOAMU_WDATA,
		                   airoha_xpon_bytes_to_u32(message + i * 4));

	return 0;
}

static int airoha_xpon_send_ack(struct airoha_xpon *xpon, u8 seq,
                                u8 completion_code)
{
	u8 message[AIROHA_XGPON_PLOAMU_BYTES] = {};

	/* Upstream PLOAM dest/msg/seq occupy bytes 4–7; downstream uses 0–3. */
	message[4] = xpon->onu_id >> 8;
	message[5] = xpon->onu_id;
	message[6] = AIROHA_XGPON_PLOAMU_ACKNOWLEDGE;
	message[7] = seq;
	message[8] = completion_code;

	return airoha_xpon_send_ploamu(xpon, message);
}

static int airoha_xpon_send_registration(struct airoha_xpon *xpon, u8 seq)
{
	u8 message[AIROHA_XGPON_PLOAMU_BYTES] = {};

	message[4] = xpon->onu_id >> 8;
	message[5] = xpon->onu_id;
	message[6] = AIROHA_XGPON_PLOAMU_REGISTRATION;
	message[7] = seq;
	/* G.987.3 represents an unset Registration-ID with 36 zero bytes. */
	memcpy(message + 8, xpon->identity.active_identity.registration_id,
	       sizeof(xpon->identity.active_identity.registration_id));

	return airoha_xpon_send_ploamu(xpon, message);
}

static int airoha_xpon_send_key_report(struct airoha_xpon *xpon, u8 seq,
                                       u8 report_type, u8 data_key_index,
                                       const u8 fragment[16])
{
	u8 message[AIROHA_XGPON_PLOAMU_BYTES] = {};
	u32 key_index;
	int ret;

	/*
	 * A 44-byte upstream Key_Report places the MIC PIK index at byte 3,
	 * common header at 4–7, report metadata at 8–11, and up to 32 fragment
	 * bytes afterward. A 128-bit key or confirm digest occupies 16 bytes.
	 */
	key_index = airoha_xgpon_read(xpon, AIROHA_XGPON_CUR_KIDX);
	message[3] = !!(key_index & AIROHA_XGPON_CUR_PIK_INDEX);
	message[4] = xpon->onu_id >> 8;
	message[5] = xpon->onu_id;
	message[6] = AIROHA_XGPON_PLOAMU_KEY_REPORT;
	message[7] = seq;
	message[8] = report_type & BIT(0);
	message[9] = data_key_index & GENMASK(1, 0);
	message[10] = 0;
	memcpy(message + 12, fragment, AIROHA_XGPON_DATA_KEY_BYTES);

	ret = airoha_xpon_send_ploamu(xpon, message);
	if (!ret) {
		xpon->crypto.key_report_tx_count++;
		dev_info(xpon->dev,
		         "Key_Report sent: seq=%u type=%s data-key-index=%u\n",
		         seq,
		         report_type == AIROHA_XGPON_KEY_CONTROL_GENERATE ?
		                 "generate" :
		                 "confirm",
		         data_key_index);
	}
	return ret;
}

static void airoha_xpon_retire_other_data_key(struct airoha_xpon *xpon,
                                              u8 active_index)
{
	u8 old_index = active_index == 1 ? 2 : 1;
	u32 base = old_index == 1 ? AIROHA_XGPON_AES_UC_IDX0_KEY0 :
	                            AIROHA_XGPON_AES_UC_IDX1_KEY0;
	u32 valid = old_index == 1 ? AIROHA_XGPON_DS_AES_UC_IDX0_VALID :
	                             AIROHA_XGPON_DS_AES_UC_IDX1_VALID;
	unsigned int i;

	/*
	 * OLT confirmation retires the previous downstream slot and key.
	 * US_AES_KEY_CTRL switches the upstream slot atomically.
	 */
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_DS_AES_KEY_VLD, valid, 0);
	for (i = 0; i < 4; i++)
		airoha_xgpon_write(xpon, base + i * 4, 0);
	memzero_explicit(xpon->crypto.data_keys[old_index - 1],
	                 sizeof(xpon->crypto.data_keys[old_index - 1]));
}

static int airoha_xpon_handle_key_control(struct airoha_xpon *xpon,
                                          const u8 *message)
{
	static const u8 key_name[16] = "3141592653589793";
	u8 cmac_input[32], control, data_key_index, key_length;
	u8 kek[16];
	u16 dest = get_unaligned_be16(message);
	bool repeated;
	int ret;

	if (xpon->onu_state != AIROHA_XGPON_O5 || dest != xpon->onu_id)
		return -EINVAL;

	control = message[5] & BIT(0);
	data_key_index = message[6] & GENMASK(1, 0);
	key_length = message[7];
	xpon->crypto.key_control_rx_count++;
	dev_info(
		xpon->dev,
		"Key_Control received: seq=%u control=%s data-key-index=%u key-length=%u\n",
		message[3],
		control == AIROHA_XGPON_KEY_CONTROL_GENERATE ? "generate" :
							       "confirm",
		data_key_index, key_length);

	if (data_key_index != 1 && data_key_index != 2) {
		ret = -EINVAL;
		goto error;
	}
	/* MAC data-key slots hold 128 bits; key_lens=0 requests a 256-bit key. */
	if (!key_length) {
		ret = -EOPNOTSUPP;
		goto error;
	}

	if (control == AIROHA_XGPON_KEY_CONTROL_GENERATE) {
		repeated = xpon->crypto.exchange_state ==
		                   AIROHA_XGPON_KEY_WAIT_CONFIRM &&
		           xpon->crypto.pending_data_key_index ==
		                   data_key_index;
		if (xpon->crypto.exchange_state ==
		            AIROHA_XGPON_KEY_WAIT_CONFIRM &&
		    !repeated) {
			ret = -EBUSY;
			goto error;
		}

		if (!repeated) {
			ret = airoha_xpon_get_active_kek(xpon, kek);
			if (ret)
				goto error;
			get_random_bytes(
				xpon->crypto.data_keys[data_key_index - 1],
				AIROHA_XGPON_DATA_KEY_BYTES);
			ret = airoha_xpon_aes_encrypt_block(
				xpon, kek,
				xpon->crypto.data_keys[data_key_index - 1],
				xpon->crypto.encrypted_data_key);
			memzero_explicit(kek, sizeof(kek));
			if (ret)
				goto error;
			ret = airoha_xpon_program_data_key(
				xpon, data_key_index,
				xpon->crypto.data_keys[data_key_index - 1]);
			if (ret)
				goto error;
			xpon->crypto.pending_data_key_index = data_key_index;
			xpon->crypto.exchange_state =
				AIROHA_XGPON_KEY_WAIT_CONFIRM;
		}

		/*
		 * The OLT retries Key_Control each second. Reuse the pending slot's
		 * random key across retries while advancing the report sequence.
		 */
		ret = airoha_xpon_send_key_report(
			xpon, message[3], AIROHA_XGPON_KEY_CONTROL_GENERATE,
			data_key_index, xpon->crypto.encrypted_data_key);
		if (ret)
			goto error;
		/*
		 * RX valid follows successful report FIFO submission, allowing the MAC
		 * to decrypt downstream traffic with the reported slot.
		 */
		ret = airoha_xpon_set_data_key_rx_valid(xpon, data_key_index);
		if (ret)
			goto error;
		return 0;
	}

	if (control != AIROHA_XGPON_KEY_CONTROL_CONFIRM) {
		ret = -EINVAL;
		goto error;
	}

	repeated = xpon->crypto.exchange_state == AIROHA_XGPON_KEY_ACTIVE &&
	           xpon->crypto.active_data_key_index == data_key_index;
	if (!repeated) {
		if (xpon->crypto.exchange_state !=
		            AIROHA_XGPON_KEY_WAIT_CONFIRM ||
		    xpon->crypto.pending_data_key_index != data_key_index) {
			ret = -EINVAL;
			goto error;
		}

		ret = airoha_xpon_get_active_kek(xpon, kek);
		if (ret)
			goto error;
		memcpy(cmac_input, xpon->crypto.data_keys[data_key_index - 1],
		       16);
		memcpy(cmac_input + 16, key_name, sizeof(key_name));
		ret = airoha_xpon_aes_cmac_32(xpon, kek, cmac_input,
		                              xpon->crypto.key_confirm_digest);
		memzero_explicit(kek, sizeof(kek));
		memzero_explicit(cmac_input, sizeof(cmac_input));
		if (ret)
			goto error;

		ret = airoha_xpon_activate_data_key(xpon, data_key_index);
		if (ret)
			goto error;
		airoha_xpon_retire_other_data_key(xpon, data_key_index);
		xpon->crypto.active_data_key_index = data_key_index;
		xpon->crypto.exchange_state = AIROHA_XGPON_KEY_ACTIVE;
		xpon->crypto.key_exchange_completed_count++;
	}

	/* Repeated requests reuse the digest and slot if Confirm Report was lost. */
	ret = airoha_xpon_send_key_report(xpon, message[3],
	                                  AIROHA_XGPON_KEY_CONTROL_CONFIRM,
	                                  data_key_index,
	                                  xpon->crypto.key_confirm_digest);
	if (ret)
		goto error;
	return 0;

error:
	memzero_explicit(kek, sizeof(kek));
	memzero_explicit(cmac_input, sizeof(cmac_input));
	xpon->crypto.key_exchange_error_count++;
	dev_warn_ratelimited(
		xpon->dev,
		"Key_Control failed: seq=%u control=%u data-key-index=%u key-length=%u error=%d\n",
		message[3], control, data_key_index, key_length, ret);
	return ret;
}

static int airoha_xpon_accept_assign_onu_id(struct airoha_xpon *xpon,
                                            const u8 *message)
{
	u16 dest = get_unaligned_be16(message);
	u16 onu_id = (message[4] & 0x03) << 8 | message[5];
	int ret;

	if (dest != AIROHA_XGPON_BROADCAST_ID) {
		dev_dbg(xpon->dev, "Assign ONU-ID rejected: destination=%u\n",
		        dest);
		return -EINVAL;
	}
	if (!xpon->identity.active_identity.serial_configured) {
		dev_dbg(xpon->dev,
		        "Assign ONU-ID rejected: serial unconfigured\n");
		return -EINVAL;
	}
	/* Broadcast assignments reach multiple ONUs; retain the SN match result. */
	if (memcmp(message + 6, xpon->identity.active_identity.serial_number,
	           sizeof(xpon->identity.active_identity.serial_number))) {
		dev_dbg(xpon->dev,
		        "Assign ONU-ID ignored: serial mismatch, offered ONU-ID=%u\n",
		        onu_id);
		return -EINVAL;
	}

	/*
	 * The OLT may repeat Assign ONU-ID before receiving Registration. Reuse
	 * the current assignment for matching SN and ONU-ID across O4/O5 retries.
	 */
	if (xpon->onu_state == AIROHA_XGPON_O4 ||
	    xpon->onu_state == AIROHA_XGPON_O5)
		return xpon->onu_id == onu_id ? 0 : -EINVAL;
	if (xpon->onu_state != AIROHA_XGPON_O2_3)
		return -EINVAL;

	/* The unicast GEM follows publication of ONU_ID.valid. */
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_ONU_ID,
	                         AIROHA_XGPON_ONU_ID_VALID |
	                                 AIROHA_XGPON_ONU_ID_MASK,
	                         AIROHA_XGPON_ONU_ID_VALID | onu_id);
	ret = airoha_xpon_set_gem(xpon, onu_id, true, true);
	if (ret) {
		/* Roll back ONU_ID.valid if GEM installation fails. */
		airoha_xgpon_update_bits(xpon, AIROHA_XGPON_ONU_ID,
		                         AIROHA_XGPON_ONU_ID_VALID |
		                                 AIROHA_XGPON_ONU_ID_MASK,
		                         AIROHA_XGPON_BROADCAST_ID);
		return ret;
	}
	xpon->onu_id = onu_id;
	airoha_xpon_set_onu_state(xpon, AIROHA_XGPON_O4);

	dev_info(
		xpon->dev,
		"PLOAM: ONU-ID=%u and matching OMCC GEM committed in stock order; entering O4\n",
		onu_id);
	return 0;
}

static int airoha_xpon_accept_ranging_time(struct airoha_xpon *xpon,
                                           const u8 *message)
{
	u16 dest = get_unaligned_be16(message);
	u32 eqd = get_unaligned_be32(message + 5);
	bool absolute = message[4] & BIT(0);
	bool positive = !(message[4] & BIT(1));

	if (xpon->onu_state == AIROHA_XGPON_O4) {
		if (dest != xpon->onu_id || !absolute)
			return -EINVAL;
		xpon->eqd = eqd;
		airoha_xgpon_write(xpon, AIROHA_XGPON_EQD, eqd);
		airoha_xpon_set_onu_state(xpon, AIROHA_XGPON_O5);
		/* O5 marks line activation; service_ready tracks usable service GEMs. */
		xpon->lifecycle = AIROHA_XPON_OPERATIONAL;
		airoha_xpon_update_omci_link(xpon);
		if (xpon->data_path.configured && xpon->pon_netdev)
			airoha_xpon_set_data_path_link(xpon, true);
		dev_info(
			xpon->dev,
			"PLOAM: absolute EqD=0x%08x; entered O5; service GEM/T-CONT mapping is still required\n",
			eqd);
		/* Record the O-state even when a closed TX gate blocks its ACK. */
		airoha_xpon_send_ack(xpon, message[3],
		                     AIROHA_XGPON_PLOAM_ACK_OK);
		return 0;
	}

	if (xpon->onu_state != AIROHA_XGPON_O5 ||
	    (dest != xpon->onu_id && dest != AIROHA_XGPON_BROADCAST_ID))
		return -EINVAL;

	if (absolute)
		xpon->eqd = eqd;
	else if (positive)
		xpon->eqd += eqd;
	else
		xpon->eqd -= min(eqd, xpon->eqd);
	airoha_xgpon_write(xpon, AIROHA_XGPON_EQD, xpon->eqd);

	return 0;
}

static int airoha_xpon_accept_alloc_id(struct airoha_xpon *xpon,
                                       const u8 *message)
{
	u16 dest = get_unaligned_be16(message);
	u16 alloc_id = (message[4] & 0x3f) << 8 | message[5];
	u8 type = message[6];
	u8 index;
	int ret;

	if (xpon->onu_state != AIROHA_XGPON_O5 || dest != xpon->onu_id ||
	    alloc_id == xpon->onu_id)
		return -EINVAL;

	if (type == 1)
		ret = airoha_xpon_find_or_create_tcont(xpon, alloc_id, &index);
	else if (type == 0xff)
		ret = airoha_xpon_remove_tcont(xpon, alloc_id);
	else
		ret = -EINVAL;

	if (ret)
		airoha_xpon_send_ack(xpon, message[3],
		                     AIROHA_XGPON_PLOAM_ACK_PROCESS_ERROR);
	else
		airoha_xpon_send_ack(xpon, message[3],
		                     AIROHA_XGPON_PLOAM_ACK_OK);
	/* drain holds only ploam_lock; link_work applies under state_lock. */
	if (!ret && type == 1 && READ_ONCE(xpon->data_path.pending_count))
		atomic_set(&xpon->data_path_retry_pending, 1);
	return ret;
}

static int airoha_xpon_handle_ploamd(struct airoha_xpon *xpon,
                                     const u8 *message)
{
	u16 dest = get_unaligned_be16(message);
	int ret;

	/* Byte 51 bit 0 carries the MAC's MIC verdict for state dispatch. */
	if (!(message[AIROHA_XGPON_PLOAMD_BYTES - 1] & BIT(0)))
		return -EBADMSG;

	switch (message[2]) {
	case AIROHA_XGPON_PLOAMD_PROFILE:
		if (dest != AIROHA_XGPON_BROADCAST_ID && dest != xpon->onu_id)
			return -EADDRNOTAVAIL;
		ret = airoha_xpon_program_profile(xpon, message);
		break;
	case AIROHA_XGPON_PLOAMD_ASSIGN_ONU_ID:
		ret = airoha_xpon_accept_assign_onu_id(xpon, message);
		break;
	case AIROHA_XGPON_PLOAMD_RANGING_TIME:
		ret = airoha_xpon_accept_ranging_time(xpon, message);
		break;
	case AIROHA_XGPON_PLOAMD_ASSIGN_ALLOC_ID:
		ret = airoha_xpon_accept_alloc_id(xpon, message);
		break;
	case AIROHA_XGPON_PLOAMD_REQUEST_REGISTRATION:
		if (xpon->onu_state != AIROHA_XGPON_O5 || dest != xpon->onu_id)
			ret = -EINVAL;
		else
			ret = airoha_xpon_send_registration(xpon, message[3]);
		if (ret == -EACCES)
			ret = 0;
		break;
	case AIROHA_XGPON_PLOAMD_KEY_CONTROL:
		ret = airoha_xpon_handle_key_control(xpon, message);
		break;
	case AIROHA_XGPON_PLOAMD_DEACTIVATE:
		if (dest != xpon->onu_id && dest != AIROHA_XGPON_BROADCAST_ID &&
		    dest != AIROHA_XGPON_ALL_ONU_ID)
			ret = -EINVAL;
		else {
			/* PLAIN_RESET preserves PHY sync; link_work resets the MAC after
			 * releasing ploam_lock.
			 */
			atomic_set(&xpon->mac_restart_pending, 1);
			ret = 0;
		}
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	if (!ret)
		xpon->stats.ploamd_accepted++;
	else
		xpon->stats.ploamd_rejected++;
	return ret;
}

unsigned int airoha_xpon_drain_ploamd(struct airoha_xpon *xpon)
{
	unsigned int count;

	/* T-CONT/GEM share a command port with manual data-path updates. */
	mutex_lock(&xpon->ploam_lock);
	for (count = 0; count < AIROHA_XGPON_PLOAMD_MAX_DRAIN; count++) {
		u8 message[AIROHA_XGPON_PLOAMD_BYTES];
		u32 fifo_status, word;
		int ret;
		unsigned int i, used;

		fifo_status =
			airoha_xgpon_read(xpon, AIROHA_XGPON_PLOAMD_FIFO_STS);
		used = FIELD_GET(AIROHA_XGPON_PLOAMD_FIFO_USED_MASK,
		                 fifo_status);
		if (used < AIROHA_XGPON_PLOAMD_WORDS)
			break;

		/* RDATA packs four wire bytes from bits 31:24 to 7:0. */
		for (i = 0; i < AIROHA_XGPON_PLOAMD_WORDS; i++) {
			word = airoha_xgpon_read(xpon,
			                         AIROHA_XGPON_PLOAMD_RDATA);
			message[i * 4] = word >> 24;
			message[i * 4 + 1] = word >> 16;
			message[i * 4 + 2] = word >> 8;
			message[i * 4 + 3] = word;
		}

		ret = airoha_xpon_handle_ploamd(xpon, message);
		dev_info_ratelimited(
			xpon->dev,
			"PLOAMd: dest=%u, msg-id=0x%02x, seq=%u, mic=%s, result=%d\n",
			(message[0] << 8) | message[1], message[2], message[3],
			message[AIROHA_XGPON_PLOAMD_BYTES - 1] & BIT(0) ?
				"valid" :
				"invalid",
			ret);

		if (fifo_status & AIROHA_XGPON_PLOAMD_FIFO_OVERRUN)
			dev_warn_ratelimited(xpon->dev,
			                     "PLOAMd FIFO overrun detected\n");
	}
	mutex_unlock(&xpon->ploam_lock);

	return count;
}
