/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_PRIVATE_H_
#define _AIROHA_XPON_PRIVATE_H_

#include <airoha-pon-frontend.h>
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pcs/pcs-airoha.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "airoha-xpon-netlink.h"
#include "airoha-xpon-regs.h"

struct attribute_group;
struct crypto_sync_skcipher;
struct device;
struct net_device;
struct phylink_pcs;
struct regmap;
struct airoha_xpon_omci;
struct airoha_xpon_oam;

enum airoha_xpon_control_protocol {
	AIROHA_XPON_CONTROL_OMCI,
	AIROHA_XPON_CONTROL_OAM,
};

struct airoha_xpon_mode_info {
	enum airoha_xpon_mode mode;
	const char *name;
	enum airoha_pon_frontend_mode frontend_mode;
	enum airoha_xpon_control_protocol control_protocol;
	bool mac_supported;
};

enum airoha_xpon_lifecycle {
	AIROHA_XPON_STOPPED,
	AIROHA_XPON_WAIT_OPTICAL_SIGNAL,
	AIROHA_XPON_PMA_CONFIGURED,
	AIROHA_XPON_WAIT_LINE_SYNC,
	AIROHA_XPON_PROTOCOL_ACTIVATING,
	AIROHA_XPON_OPERATIONAL,
	AIROHA_XPON_ERROR,
};

enum airoha_epon_mpcp_state {
	AIROHA_EPON_MPCP_WAIT,
	AIROHA_EPON_MPCP_REGISTERING,
	AIROHA_EPON_MPCP_REGISTER_REQUEST,
	AIROHA_EPON_MPCP_REGISTER_PENDING,
	AIROHA_EPON_MPCP_REGISTERED,
	AIROHA_EPON_MPCP_DENIED,
};

/* The single LLID uses channel 0, mapped to GDM2/QDMA channel 0. */
struct airoha_xpon_epon {
	enum airoha_epon_mpcp_state mpcp_state;
	/*
	 * The MAC hard IRQ submits Register Request within the Discovery grant.
	 * Atomic pending bits preserve W1C events for the threaded state machine.
	 */
	atomic_t pending_events;
	atomic_t pending_error_events;
	u16 llid;
	/* MPCP sync_time for the active MAC epoch. */
	u16 sync_time;
	u8 channel;
	u8 discovery_retry;
	bool llid_valid;
	bool pcs_synced;
	bool discovery_seen;
	unsigned long denied_until;
	/* Hard IRQ updates these values; readers use atomic_read(). */
	atomic_t discovery_gate_count;
	atomic_t register_request_count;
	unsigned int register_message_count;
	unsigned int register_ack_count;
	unsigned int nack_count;
	unsigned int reregister_count;
	unsigned int deregister_count;
	unsigned int mpcp_timeout_count;
	unsigned long sync_loss_since;
	unsigned int sync_loss_count;
	unsigned int recovery_count;
	unsigned int full_reinit_count;
	unsigned long registered_since;
	unsigned long last_registration_lifetime;
	unsigned int mac_error_count;
	u32 last_mac_error;
	u32 last_register_status;
};

/* ONU states match hardware ACTIVATION_ST[3:0] values. */
enum airoha_xgpon_onu_state {
	AIROHA_XGPON_O1 = 1,
	AIROHA_XGPON_O2_3 = 2,
	AIROHA_XGPON_O4 = 4,
	AIROHA_XGPON_O5 = 5,
	AIROHA_XGPON_O7 = 7,
};

#define AIROHA_XGPON_BROADCAST_ID 0x03ff
/* 0x3fe is the global destination accepted by the Deactivate handler. */
#define AIROHA_XGPON_ALL_ONU_ID 0x03fe

enum airoha_xgpon_key_exchange_state {
	AIROHA_XGPON_KEY_IDLE,
	AIROHA_XGPON_KEY_WAIT_CONFIRM,
	AIROHA_XGPON_KEY_ACTIVE,
};

/* PLOAM identity carries an eight-byte SN and 36-byte Registration-ID. */
struct airoha_xpon_identity {
	u8 serial_number[8];
	u8 registration_id[36];
	const char *serial_source;
	bool serial_configured;
	/* false selects the G.987.3 default of 36 zero bytes. */
	bool registration_id_configured;
};

enum airoha_xpon_mib_index {
	AIROHA_XPON_MIB_XGTC_RX,
	AIROHA_XPON_MIB_BURSTS_TX,
	AIROHA_XPON_MIB_PLOAMD_RX,
	AIROHA_XPON_MIB_PLOAMU_TX,
	AIROHA_XPON_MIB_XGEM_RX,
	AIROHA_XPON_MIB_XGEM_TX,
	AIROHA_XPON_MIB_COUNT,
};

struct airoha_xpon_stats {
	/* state_lock protects the accumulated count and previous hardware sample. */
	u64 mib_total[AIROHA_XPON_MIB_COUNT];
	u32 mib_last[AIROHA_XPON_MIB_COUNT];
	unsigned int rx_start_count;
	unsigned int full_reinit_count;
	unsigned int no_ready_recovery_count;
	unsigned int phy_irq_count;
	unsigned int phy_rx_ready_count;
	unsigned int phy_sync_count;
	unsigned int phy_los_count;
	unsigned int phy_lof_count;
	unsigned int sn_request_count;
	unsigned int sn_sent_count;
	unsigned int ranging_request_count;
	unsigned int registration_sent_count;
	unsigned int mac_error_irq_count;
	unsigned int rx_mbi_header_overrun_irq_count;
	unsigned int rx_mbi_payload_overrun_irq_count;
	unsigned int tx_late_error_count;
	unsigned int tx_resync_count;
	unsigned int tx_resync_failure_count;
	u32 latched_fifo_error;
	u32 latched_tx_error;
	u32 latched_rx_error;
	u32 latched_bwm_check;
	unsigned int ploamd_accepted;
	unsigned int ploamd_rejected;
	unsigned int ploamu_blocked;
};

struct airoha_xpon_identity_set {
	/* ndo_open latches pending_identity into the active line epoch. */
	struct airoha_xpon_identity default_identity;
	struct airoha_xpon_identity pending_identity;
	struct airoha_xpon_identity active_identity;
};

/* Crypto and PLOAM share PIK/OIK/KEK and data-key epoch state. */
struct airoha_xpon_crypto_state {
	struct crypto_sync_skcipher *aes;
	/* Each controller has a direct-mapped AES bounce block. */
	u8 aes_block[16];
	unsigned int generation_runs;
	u32 generation_failed_type;
	u32 activation_pon_tag0;
	u32 activation_pon_tag1;
	unsigned int key_control_rx_count;
	unsigned int key_report_tx_count;
	unsigned int key_exchange_completed_count;
	unsigned int key_exchange_error_count;
	enum airoha_xgpon_key_exchange_state exchange_state;
	u8 data_keys[2][16];
	u8 encrypted_data_key[16];
	u8 key_confirm_digest[16];
	u8 pending_data_key_index;
	u8 active_data_key_index;
	bool default_ploam_ik_programmed;
	bool activation_keys_programmed;
	bool o4_key_switch_observed;
};

#define AIROHA_XPON_MAX_DATA_PATHS 32

struct airoha_xpon_data_path_entry {
	u16 alloc_id;
	u16 gem_id;
	u16 vlan_id;
	u8 tcont;
	u8 pbit_mask;
	/* Class 281 describes downstream multicast GEMs; upstream unicast GEMs
	 * allocate T-CONTs and QDMA flows.
	 */
	bool multicast;
};

struct airoha_xpon_data_path_state {
	struct airoha_xpon_data_path_entry entries[AIROHA_XPON_MAX_DATA_PATHS];
	/* OMCI paths wait here until PLOAM assigns every unicast Alloc-ID. */
	struct airoha_xpon_data_path_entry pending[AIROHA_XPON_MAX_DATA_PATHS];
	u8 count;
	u8 pending_count;
	/* The first mapping serves status reporting and EPON's single LLID. */
	u16 alloc_id;
	u16 gem_id;
	u8 tcont;
	bool configured;
};

struct airoha_xpon {
	struct device *dev;
	struct dentry *debug_dir;
	void __iomem *xgpon_base;
	void __iomem *epon_base;
	void __iomem *pon_phy_base;
	struct regmap *scu;
	struct airoha_pon_frontend *frontend;
	struct airoha_pon_frontend_link_config frontend_link;
	struct phylink_pcs *pcs;
	struct net_device *pon_netdev;
	struct airoha_xpon_omci *omci;
	struct airoha_xpon_oam *oam;
	struct airoha_xpon_leds *leds;
	struct airoha_xpon_crypto_state crypto;
	struct airoha_xpon_identity_set identity;
	struct airoha_xpon_data_path_state data_path;
	struct airoha_xpon_epon epon;
	struct airoha_xpon_stats stats;
	struct mutex state_lock;
	struct mutex ploam_lock;
	struct work_struct start_work;
	struct delayed_work link_work;
	struct list_head netlink_node;
	enum airoha_xpon_mode configured_mode;
	enum airoha_xpon_mode active_mode;
	enum airoha_xpon_lifecycle lifecycle;
	int last_start_error;
	unsigned long no_sync_since;
	int mac_irq;
	int phy_irq;
	atomic_t phy_events;
	/* IRQ queues Deactivate; link_work performs the plain reset. */
	atomic_t mac_restart_pending;
	/* link_work handles TX-late resync under state_lock. */
	atomic_t tx_resync_pending;
	/* Assign_Alloc-ID queues deferred data paths for link_work. */
	atomic_t data_path_retry_pending;
	enum airoha_xgpon_onu_state onu_state;
	u16 onu_id;
	u32 eqd;
	bool optical_signal;
	/* The PCS profile identifies retained PMA calibration and line rate. */
	enum airoha_pcs_pon_mode pcs_profile;
	bool pcs_profile_valid;
	bool rx_active;
	bool xgpon_irq_enabled;
	bool mac_initialized;
	bool burst_profile_programmed;
	bool tx_armed;
	/* The userspace agent publishes service readiness after operator authentication. */
	bool service_ready;
	bool stopping;
	bool active_mode_valid;
};

/*
 * state_lock serializes lifecycle, identity snapshots, ONU/GEM epochs, and
 * BEN/MAC/PMA state. ploam_lock serializes the PLOAM FIFO and T-CONT/GEM
 * command port. Nested acquisition follows state_lock -> ploam_lock.
 * IRQ publishes events atomically for link_work to consume under state_lock.
 * stopping closes asynchronous entry before IRQ and work synchronization.
 */

#define AIROHA_XGPON_PLOAMD_MAX_DRAIN    20
#define AIROHA_XPON_LINK_POLL_MS         250
#define AIROHA_XPON_RETRAIN_RETRY_MS     2000
#define AIROHA_XPON_SYNC_LOSS_CONFIRM_MS AIROHA_XPON_LINK_POLL_MS
/* Allow 3500 ms for readiness or LOS recovery before another PMA reset. */
#define AIROHA_XPON_NO_SYNC_RECOVERY_MS 3500
/*
 * Every fifth recovery from attempt 2 also resets SCU; others reset PMA.
 */
#define AIROHA_XPON_FULL_REINIT_PERIOD 5

static inline u32 airoha_xgpon_read(struct airoha_xpon *xpon, u32 reg)
{
	return readl(xpon->xgpon_base + reg);
}

static inline void airoha_xgpon_write(struct airoha_xpon *xpon, u32 reg,
                                      u32 value)
{
	writel(value, xpon->xgpon_base + reg);
}

static inline u32 airoha_pon_phy_read(struct airoha_xpon *xpon, u32 reg)
{
	return readl(xpon->pon_phy_base + reg);
}

static inline void airoha_pon_phy_write(struct airoha_xpon *xpon, u32 reg,
                                        u32 value)
{
	writel(value, xpon->pon_phy_base + reg);
}

static inline void airoha_xgpon_update_bits(struct airoha_xpon *xpon, u32 reg,
                                            u32 mask, u32 value)
{
	u32 old_value = airoha_xgpon_read(xpon, reg);

	airoha_xgpon_write(xpon, reg, (old_value & ~mask) | (value & mask));
}

static inline void airoha_pon_phy_update_bits(struct airoha_xpon *xpon, u32 reg,
                                              u32 mask, u32 value)
{
	u32 old_value = airoha_pon_phy_read(xpon, reg);

	airoha_pon_phy_write(xpon, reg, (old_value & ~mask) | (value & mask));
}

static inline u32 airoha_xpon_bytes_to_u32(const u8 *bytes)
{
	return (u32)bytes[0] << 24 | (u32)bytes[1] << 16 | (u32)bytes[2] << 8 |
	       bytes[3];
}

void airoha_xpon_set_onu_state(struct airoha_xpon *xpon,
                               enum airoha_xgpon_onu_state state);
void airoha_xpon_set_data_path_link(struct airoha_xpon *xpon, bool ready);
void airoha_xpon_update_omci_link(struct airoha_xpon *xpon);
const struct airoha_xpon_mode_info *
airoha_xpon_mode_info(enum airoha_xpon_mode mode);
const char *airoha_xpon_mode_name(enum airoha_xpon_mode mode);
bool airoha_xpon_pcs_profile_matches(enum airoha_xpon_mode mode,
                                     enum airoha_pcs_pon_mode profile);
int airoha_xpon_mode_parse(const char *name, enum airoha_xpon_mode *mode);
int airoha_xpon_mode_get_default(struct device *dev,
                                 enum airoha_xpon_mode *mode);
int airoha_xpon_identity_init(struct device *dev, struct airoha_xpon *xpon);
int airoha_xpon_create_control_rtnl(struct airoha_xpon *xpon);
int airoha_xpon_set_mode_locked(struct airoha_xpon *xpon,
                                enum airoha_xpon_mode mode);
int airoha_xpon_netlink_init(void);
void airoha_xpon_netlink_exit(void);
void airoha_xpon_netlink_register(struct airoha_xpon *xpon);
void airoha_xpon_netlink_unregister(struct airoha_xpon *xpon);
struct airoha_xpon_leds *airoha_xpon_leds_create(struct device *dev,
                                                 const char *pon_name);
void airoha_xpon_leds_destroy(void *data);
void airoha_xpon_leds_update(struct airoha_xpon *xpon);
void airoha_xpon_clear_mac_errors(struct airoha_xpon *xpon,
                                  bool clear_software_latch);

void airoha_xpon_disarm_upstream(struct airoha_xpon *xpon, const char *reason);
int airoha_xpon_try_arm_upstream(struct airoha_xpon *xpon);
void airoha_xpon_clear_assignment(struct airoha_xpon *xpon);
int airoha_xpon_configure_data_path(struct airoha_xpon *xpon, u16 alloc_id,
                                    u16 gem_id);
int airoha_xpon_replace_data_paths(
	struct airoha_xpon *xpon,
	const struct airoha_xpon_data_path_entry *requested,
	unsigned int count);
int airoha_xpon_clear_data_path(struct airoha_xpon *xpon);
void airoha_xpon_retry_data_paths(struct airoha_xpon *xpon);
void airoha_xpon_clear_tconts(struct airoha_xpon *xpon);
unsigned int airoha_xpon_drain_ploamd(struct airoha_xpon *xpon);

int airoha_xpon_crypto_init(struct airoha_xpon *xpon);
void airoha_xpon_crypto_cleanup(void *data);
int airoha_xpon_program_activation_keys(struct airoha_xpon *xpon);
int airoha_xpon_install_omci_msk(struct airoha_xpon *xpon, const u8 key[16]);
int airoha_xpon_aes_encrypt_block(struct airoha_xpon *xpon, const u8 key[16],
                                  const u8 input[16], u8 output[16]);
int airoha_xpon_aes_cmac_32(struct airoha_xpon *xpon, const u8 key[16],
                            const u8 message[32], u8 digest[16]);
int airoha_xpon_get_active_kek(struct airoha_xpon *xpon, u8 kek[16]);
int airoha_xpon_program_data_key(struct airoha_xpon *xpon, u8 index,
                                 const u8 key[16]);
int airoha_xpon_set_data_key_rx_valid(struct airoha_xpon *xpon, u8 index);
int airoha_xpon_activate_data_key(struct airoha_xpon *xpon, u8 index);
void airoha_xpon_reset_data_key_exchange(struct airoha_xpon *xpon);

struct sk_buff;
int airoha_xpon_put_status(struct sk_buff *reply, struct airoha_xpon *xpon);
void airoha_xpon_xgpon_counters_start(struct airoha_xpon *xpon);
void airoha_xpon_xgpon_counters_update(struct airoha_xpon *xpon);
void airoha_xpon_debugfs_init(struct airoha_xpon *xpon);
void airoha_xpon_debugfs_remove(struct airoha_xpon *xpon);
extern const struct attribute_group *airoha_xpon_groups[];

#endif
