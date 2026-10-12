/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AIROHA_PON_FRONTEND_H
#define __AIROHA_PON_FRONTEND_H

/* Shared interface between the PON MAC and optical frontend providers. */
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/types.h>

struct airoha_pon_frontend;

/**
 * enum airoha_pon_frontend_mode - Optical line mode
 *
 * The line mode selects analog settings, wavelength, and burst behavior.
 * Each chip driver interprets its own calibration format.
 */
enum airoha_pon_frontend_mode {
	AIROHA_PON_FRONTEND_MODE_GPON,
	AIROHA_PON_FRONTEND_MODE_XGPON,
	AIROHA_PON_FRONTEND_MODE_XGSPON,
	AIROHA_PON_FRONTEND_MODE_EPON_1G,
	AIROHA_PON_FRONTEND_MODE_EPON_10G_1G,
	AIROHA_PON_FRONTEND_MODE_EPON_10G_10G,
};

#define AIROHA_PON_FRONTEND_MODE_BIT(_mode) BIT(_mode)

/**
 * enum airoha_pon_frontend_calibration_state - Device calibration state
 * @AIROHA_PON_FRONTEND_CALIBRATION_UNKNOWN: State unavailable from the provider
 * @AIROHA_PON_FRONTEND_CALIBRATION_READY: Data loaded and format checked
 * @AIROHA_PON_FRONTEND_CALIBRATION_MISSING: Board calibration source is empty
 * @AIROHA_PON_FRONTEND_CALIBRATION_INVALID: Source failed format validation
 * @AIROHA_PON_FRONTEND_CALIBRATION_NOT_REQUIRED: Device stores its own settings
 *
 * Diagnostics expose this state independently of the provider's TX gate.
 */
enum airoha_pon_frontend_calibration_state {
	AIROHA_PON_FRONTEND_CALIBRATION_UNKNOWN,
	AIROHA_PON_FRONTEND_CALIBRATION_READY,
	AIROHA_PON_FRONTEND_CALIBRATION_MISSING,
	AIROHA_PON_FRONTEND_CALIBRATION_INVALID,
	AIROHA_PON_FRONTEND_CALIBRATION_NOT_REQUIRED,
};

/**
 * struct airoha_pon_frontend_signal_status - Receive signal snapshot
 * @los_raw: Complete LOS or alarm register returned by the provider
 * @signal_present: Optical signal detected
 * @signal_valid: Device-level evidence supports signal_present
 *
 * Link polling reads the signal registers. A separate, lower-rate operation
 * samples DDMI so extended-page switching stays outside the polling path.
 */
struct airoha_pon_frontend_signal_status {
	u32 los_raw;
	bool signal_present;
	bool signal_valid;
};

enum airoha_pon_frontend_diagnostics_valid {
	AIROHA_PON_FRONTEND_DIAG_TEMPERATURE = BIT(0),
	AIROHA_PON_FRONTEND_DIAG_VOLTAGE = BIT(1),
	AIROHA_PON_FRONTEND_DIAG_TX_BIAS = BIT(2),
	AIROHA_PON_FRONTEND_DIAG_TX_POWER = BIT(3),
	AIROHA_PON_FRONTEND_DIAG_RX_POWER = BIT(4),
};

/**
 * struct airoha_pon_frontend_diagnostics - SFF-8472-style telemetry
 * @valid: AIROHA_PON_FRONTEND_DIAG_* validity bits
 * @temperature: Signed 8.8 fixed-point degrees Celsius
 * @voltage: Supply voltage in 100 uV units
 * @tx_bias: TX bias current in 2 uA units
 * @tx_power: TX optical power in 0.1 uW units
 * @rx_power: RX optical power in 0.1 uW units
 * @calibration: Device calibration state
 * @tx_gate_enabled: Burst-enable signal is controlled by the PON MAC
 */
struct airoha_pon_frontend_diagnostics {
	u32 valid;
	s16 temperature;
	u16 voltage;
	u16 tx_bias;
	u16 tx_power;
	u16 rx_power;
	enum airoha_pon_frontend_calibration_state calibration;
	bool tx_gate_enabled;
};

/**
 * struct airoha_pon_frontend_link_config - Frontend-to-SoC electrical settings
 * @gepon_xpon_setting: Full 1G PON PHY XPON_SETTING register value
 * @xgpon_sfp_valid_level: Full digital PON PHY SFP_VLD_LEVEL value
 * @xepon_sfp_status: Full 10G-EPON PCS SFP_STATUS value
 * @pma_xpon_setting0: Full Airoha PMA XPON_SETTING_0 value
 * @pma_xpon_setting1: Full Airoha PMA XPON_SETTING_1 value
 */
struct airoha_pon_frontend_link_config {
	u32 gepon_xpon_setting;
	u32 xgpon_sfp_valid_level;
	u32 xepon_sfp_status;
	u32 pma_xpon_setting0;
	u32 pma_xpon_setting1;
};

/**
 * struct airoha_pon_frontend_ops - Optical line operations
 * @prepare: Load settings for a line mode and enter a controllable state
 * @unprepare: Close the TX gate and leave the active mode
 * @get_signal_status: Read the receive signal state for link polling
 * @get_diagnostics: Read optical telemetry and calibration state
 * @get_link_config: Return SoC electrical settings for a line mode
 * @set_tx_enable: Connect or disconnect the PON burst-enable signal
 *
 * set_tx_enable(true) leaves burst timing under PON MAC control. Provider
 * operations may sleep.
 */
struct airoha_pon_frontend_ops {
	int (*prepare)(struct airoha_pon_frontend *frontend,
	               enum airoha_pon_frontend_mode mode);
	int (*unprepare)(struct airoha_pon_frontend *frontend);
	int (*get_signal_status)(
		struct airoha_pon_frontend *frontend,
		struct airoha_pon_frontend_signal_status *status);
	int (*get_diagnostics)(
		struct airoha_pon_frontend *frontend,
		struct airoha_pon_frontend_diagnostics *diagnostics);
	int (*get_link_config)(struct airoha_pon_frontend *frontend,
	                       enum airoha_pon_frontend_mode mode,
	                       struct airoha_pon_frontend_link_config *config);
	int (*set_tx_enable)(struct airoha_pon_frontend *frontend, bool enable);
};

struct airoha_pon_frontend {
	struct device *dev;
	struct module *owner;
	const struct airoha_pon_frontend_ops *ops;
	void *priv;
	u32 supported_modes;
	struct list_head node;
	bool registered;
};

void airoha_pon_frontend_init(struct airoha_pon_frontend *frontend,
                              struct device *dev, struct module *owner,
                              const struct airoha_pon_frontend_ops *ops,
                              void *priv, u32 supported_modes);
int airoha_pon_frontend_register(struct airoha_pon_frontend *frontend);
void airoha_pon_frontend_unregister(struct airoha_pon_frontend *frontend);
struct airoha_pon_frontend *airoha_pon_frontend_get(struct device *consumer,
                                                    const char *phandle_name);
void airoha_pon_frontend_put(struct airoha_pon_frontend *frontend);
void *airoha_pon_frontend_priv(struct airoha_pon_frontend *frontend);
struct device *airoha_pon_frontend_device(struct airoha_pon_frontend *frontend);
bool airoha_pon_frontend_supports(struct airoha_pon_frontend *frontend,
                                  enum airoha_pon_frontend_mode mode);
int airoha_pon_frontend_prepare(struct airoha_pon_frontend *frontend,
                                enum airoha_pon_frontend_mode mode);
int airoha_pon_frontend_unprepare(struct airoha_pon_frontend *frontend);
int airoha_pon_frontend_get_signal_status(
	struct airoha_pon_frontend *frontend,
	struct airoha_pon_frontend_signal_status *status);
int airoha_pon_frontend_get_diagnostics(
	struct airoha_pon_frontend *frontend,
	struct airoha_pon_frontend_diagnostics *diagnostics);
int airoha_pon_frontend_get_link_config(
	struct airoha_pon_frontend *frontend,
	enum airoha_pon_frontend_mode mode,
	struct airoha_pon_frontend_link_config *config);
int airoha_pon_frontend_set_tx_enable(struct airoha_pon_frontend *frontend,
                                      bool enable);

#endif /* __AIROHA_PON_FRONTEND_H */
