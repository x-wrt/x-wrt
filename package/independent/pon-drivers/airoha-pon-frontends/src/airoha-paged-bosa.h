/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AIROHA_PAGED_BOSA_H
#define __AIROHA_PAGED_BOSA_H

#include <airoha-pon-frontend.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mutex.h>

#define AIROHA_BOSA_PAGE_SELECT 0x7f
#define AIROHA_BOSA_TX_CONTROL  0x6e
#define AIROHA_BOSA_TX_DISABLE  BIT(6)

enum airoha_bosa_calibration_id {
	AIROHA_BOSA_CAL_GN28L95 = 1,
	AIROHA_BOSA_CAL_UX3363 = 2,
};

struct airoha_paged_bosa;

/**
 * struct airoha_paged_bosa_chip - Chip operations for a paged optical frontend
 * @name: Chip family shown in diagnostics
 * @calibration_id: Stable chip type in the calibration container
 * @supported_modes: Supported line modes
 * @identify: Read-only identification; 1 on match, 0 on another chip, errno on failure
 * @apply_calibration: Apply the native payload to volatile registers
 * @get_link_config: Electrical settings for the Airoha PMA/PCS link
 */
struct airoha_paged_bosa_chip {
	const char *name;
	u32 calibration_id;
	u32 supported_modes;
	int (*identify)(struct airoha_paged_bosa *bosa, char *revision,
	                size_t revision_size);
	int (*apply_calibration)(struct airoha_paged_bosa *bosa, const u8 *data,
	                         size_t length);
	void (*get_link_config)(struct airoha_paged_bosa *bosa,
	                        struct airoha_pon_frontend_link_config *config);
};

struct airoha_paged_bosa {
	struct airoha_pon_frontend frontend;
	struct i2c_client *client;
	struct gpio_desc *tx_disable;
	struct mutex lock;
	const struct airoha_paged_bosa_chip *chip;
	u8 *calibration;
	size_t calibration_length;
	u32 calibration_id;
	enum airoha_pon_frontend_calibration_state calibration_state;
	bool ben_active_high;
	bool prepared;
	bool tx_enabled;
};

int airoha_paged_bosa_read(struct airoha_paged_bosa *bosa, u8 reg, void *data,
                           size_t length);
int airoha_paged_bosa_write(struct airoha_paged_bosa *bosa, u8 reg, u8 value);
int airoha_paged_bosa_select_page(struct airoha_paged_bosa *bosa, u8 page);
int airoha_paged_bosa_write_range(struct airoha_paged_bosa *bosa, u8 page,
                                  u8 first, const u8 *data, size_t length,
                                  unsigned int delay_us);
void airoha_paged_bosa_prx30_link_config(
	struct airoha_paged_bosa *bosa,
	struct airoha_pon_frontend_link_config *config);

extern const struct airoha_paged_bosa_chip airoha_bosa_gn28l95;
extern const struct airoha_paged_bosa_chip airoha_bosa_ux3363;

#endif
