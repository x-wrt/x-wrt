// SPDX-License-Identifier: GPL-2.0-only
#include "airoha-paged-bosa.h"

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#define AIROHA_BOSA_CAL_MAGIC       "APONCAL\0"
#define AIROHA_BOSA_CAL_VERSION     1
#define AIROHA_BOSA_CAL_HEADER_SIZE 0x80
#define AIROHA_BOSA_CAL_NATIVE      1

static const struct airoha_paged_bosa_chip *const bosa_chips[] = {
	&airoha_bosa_gn28l95,
	&airoha_bosa_ux3363,
};

/* The PRX30 transceiver settings describe the BOSA-to-PMA/PCS connection.
 * The board's BEN polarity determines BURST_EN_INV.
 */
void airoha_paged_bosa_prx30_link_config(
	struct airoha_paged_bosa *bosa,
	struct airoha_pon_frontend_link_config *config)
{
	config->gepon_xpon_setting = 0x00000000;
	config->xgpon_sfp_valid_level = 0x00000009;
	config->xepon_sfp_status = 0x9f000000;
	config->pma_xpon_setting0 = 0x00010101;
	if (bosa->ben_active_high)
		config->pma_xpon_setting0 &= ~BIT(8);
	config->pma_xpon_setting1 = 0x01010100;
}

int airoha_paged_bosa_read(struct airoha_paged_bosa *bosa, u8 reg, void *data,
                           size_t length)
{
	struct i2c_msg messages[] = {
		{ .addr = bosa->client->addr, .len = 1, .buf = &reg },
		{ .addr = bosa->client->addr,
		  .flags = I2C_M_RD,
		  .len = length,
		  .buf = data },
	};
	int ret;

	ret = i2c_transfer(bosa->client->adapter, messages,
	                   ARRAY_SIZE(messages));
	return ret == ARRAY_SIZE(messages) ? 0 : ret < 0 ? ret : -EIO;
}

int airoha_paged_bosa_write(struct airoha_paged_bosa *bosa, u8 reg, u8 value)
{
	u8 data[] = { reg, value };
	int ret;

	ret = i2c_master_send(bosa->client, data, sizeof(data));
	return ret == sizeof(data) ? 0 : ret < 0 ? ret : -EIO;
}

int airoha_paged_bosa_select_page(struct airoha_paged_bosa *bosa, u8 page)
{
	return airoha_paged_bosa_write(bosa, AIROHA_BOSA_PAGE_SELECT, page);
}

int airoha_paged_bosa_write_range(struct airoha_paged_bosa *bosa, u8 page,
                                  u8 first, const u8 *data, size_t length,
                                  unsigned int delay_us)
{
	size_t offset;
	int ret;

	if (length > 0x100 - first)
		return -EINVAL;
	ret = airoha_paged_bosa_select_page(bosa, page);
	if (ret)
		return ret;
	if (delay_us)
		usleep_range(delay_us, delay_us + 500);
	for (offset = 0; offset < length; offset++) {
		ret = airoha_paged_bosa_write(bosa, first + offset,
		                              data[offset]);
		if (ret)
			return ret;
		if (delay_us)
			usleep_range(delay_us, delay_us + 500);
	}
	return 0;
}

static const struct airoha_paged_bosa_chip *bosa_chip_by_id(u32 id)
{
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(bosa_chips); index++)
		if (bosa_chips[index]->calibration_id == id)
			return bosa_chips[index];
	return NULL;
}

/* The 128-byte container header precedes the native payload. Separate
 * CRC-32 checks gate register writes on both header and payload integrity.
 */
static int bosa_load_calibration(struct airoha_paged_bosa *bosa)
{
	struct device *dev = &bosa->client->dev;
	struct nvmem_cell *cell;
	u8 *data;
	u32 header_crc, payload_crc, payload_length;
	u16 header_size, version;
	size_t cell_size;
	int ret = 0;

	cell = devm_nvmem_cell_get(dev, "calibration");
	if (IS_ERR(cell))
		return PTR_ERR(cell);
	data = nvmem_cell_read(cell, &cell_size);
	if (IS_ERR(data))
		return PTR_ERR(data);
	if (cell_size < AIROHA_BOSA_CAL_HEADER_SIZE ||
	    memcmp(data, AIROHA_BOSA_CAL_MAGIC, 8)) {
		ret = -EINVAL;
		goto out;
	}
	version = get_unaligned_le16(data + 0x08);
	header_size = get_unaligned_le16(data + 0x0a);
	bosa->calibration_id = get_unaligned_le32(data + 0x0c);
	payload_length = get_unaligned_le32(data + 0x14);
	header_crc = get_unaligned_le32(data + 0x7c);
	if (version != AIROHA_BOSA_CAL_VERSION ||
	    header_size != AIROHA_BOSA_CAL_HEADER_SIZE ||
	    get_unaligned_le32(data + 0x10) != AIROHA_BOSA_CAL_NATIVE ||
	    payload_length > cell_size - header_size ||
	    (crc32_le(~0U, data, 0x7c) ^ ~0U) != header_crc) {
		ret = -EINVAL;
		goto out;
	}
	payload_crc = get_unaligned_le32(data + 0x18);
	if ((crc32_le(~0U, data + header_size, payload_length) ^ ~0U) !=
	    payload_crc) {
		ret = -EBADMSG;
		goto out;
	}
	if (!bosa_chip_by_id(bosa->calibration_id)) {
		ret = -ENODEV;
		goto out;
	}
	bosa->calibration = devm_kmemdup(dev, data + header_size,
	                                 payload_length, GFP_KERNEL);
	if (!bosa->calibration) {
		ret = -ENOMEM;
		goto out;
	}
	bosa->calibration_length = payload_length;
out:
	kfree(data);
	return ret;
}

/* Mixed-material boards probe known chip IDs. The calibration header supplies
 * the material type when identification is absent; a chip ID takes precedence.
 */
static int bosa_detect_chip(struct airoha_paged_bosa *bosa,
                            const struct airoha_paged_bosa_chip **detected,
                            char *revision, size_t revision_size)
{
	const struct airoha_paged_bosa_chip *match = NULL;
	char id_text[16];
	unsigned int index;
	int ret;

	for (index = 0; index < ARRAY_SIZE(bosa_chips); index++) {
		memset(id_text, 0, sizeof(id_text));
		ret = bosa_chips[index]->identify(bosa, id_text,
		                                  sizeof(id_text));
		if (ret < 0)
			return ret;
		if (!ret)
			continue;
		if (match)
			return -EEXIST;
		match = bosa_chips[index];
		strscpy(revision, id_text, revision_size);
	}
	*detected = match;
	return 0;
}

static int bosa_soft_disable(struct airoha_paged_bosa *bosa, bool disable)
{
	u8 target, value;
	int ret;

	ret = airoha_paged_bosa_read(bosa, AIROHA_BOSA_TX_CONTROL, &value, 1);
	if (ret)
		return ret;
	target = disable ? value | AIROHA_BOSA_TX_DISABLE :
	                   value & ~AIROHA_BOSA_TX_DISABLE;
	ret = airoha_paged_bosa_write(bosa, AIROHA_BOSA_TX_CONTROL, target);
	if (!ret)
		usleep_range(1000, 1500);
	return ret;
}

static int bosa_prepare(struct airoha_pon_frontend *frontend,
                        enum airoha_pon_frontend_mode mode)
{
	struct airoha_paged_bosa *bosa = airoha_pon_frontend_priv(frontend);
	int ret;

	mutex_lock(&bosa->lock);
	gpiod_set_value_cansleep(bosa->tx_disable, 1);
	usleep_range(2000, 2500);
	bosa->prepared = false;
	bosa->tx_enabled = false;
	ret = bosa_soft_disable(bosa, true);
	if (!ret && bosa->calibration)
		ret = bosa->chip->apply_calibration(bosa, bosa->calibration,
		                                    bosa->calibration_length);
	if (!ret)
		ret = bosa_soft_disable(bosa, true);
	if (!ret)
		bosa->prepared = true;
	mutex_unlock(&bosa->lock);
	return ret;
}

static int bosa_set_tx_enable(struct airoha_pon_frontend *frontend, bool enable)
{
	struct airoha_paged_bosa *bosa = airoha_pon_frontend_priv(frontend);
	int ret;

	mutex_lock(&bosa->lock);
	if (enable && !bosa->prepared) {
		ret = -EAGAIN;
		goto out;
	}
	/* The board GPIO holds TX off while the chip's soft gate is updated. */
	gpiod_set_value_cansleep(bosa->tx_disable, 1);
	usleep_range(2000, 2500);
	bosa->tx_enabled = false;
	ret = bosa_soft_disable(bosa, !enable);
	if (!ret && enable) {
		gpiod_set_value_cansleep(bosa->tx_disable, 0);
		bosa->tx_enabled = true;
	}
out:
	mutex_unlock(&bosa->lock);
	return ret;
}

static int bosa_unprepare(struct airoha_pon_frontend *frontend)
{
	struct airoha_paged_bosa *bosa = airoha_pon_frontend_priv(frontend);
	int ret;

	mutex_lock(&bosa->lock);
	gpiod_set_value_cansleep(bosa->tx_disable, 1);
	usleep_range(2000, 2500);
	bosa->prepared = false;
	bosa->tx_enabled = false;
	ret = bosa_soft_disable(bosa, true);
	mutex_unlock(&bosa->lock);
	return ret;
}

static int
bosa_get_diagnostics(struct airoha_pon_frontend *frontend,
                     struct airoha_pon_frontend_diagnostics *diagnostics)
{
	struct airoha_paged_bosa *bosa = airoha_pon_frontend_priv(frontend);
	u8 raw[10];
	int ret;

	mutex_lock(&bosa->lock);
	/* Registers 0x60–0x69 share a window across pages, preserving page selection. */
	ret = airoha_paged_bosa_read(bosa, 0x60, raw, sizeof(raw));
	if (!ret) {
		diagnostics->temperature = (s16)get_unaligned_be16(raw);
		diagnostics->voltage = get_unaligned_be16(raw + 2);
		diagnostics->tx_bias = get_unaligned_be16(raw + 4);
		diagnostics->tx_power = get_unaligned_be16(raw + 6);
		diagnostics->rx_power = get_unaligned_be16(raw + 8);
		diagnostics->valid = AIROHA_PON_FRONTEND_DIAG_TEMPERATURE |
		                     AIROHA_PON_FRONTEND_DIAG_VOLTAGE |
		                     AIROHA_PON_FRONTEND_DIAG_TX_BIAS |
		                     AIROHA_PON_FRONTEND_DIAG_TX_POWER |
		                     AIROHA_PON_FRONTEND_DIAG_RX_POWER;
		diagnostics->calibration = bosa->calibration_state;
		diagnostics->tx_gate_enabled = bosa->tx_enabled;
	}
	mutex_unlock(&bosa->lock);
	return ret;
}

static int bosa_get_link_config(struct airoha_pon_frontend *frontend,
                                enum airoha_pon_frontend_mode mode,
                                struct airoha_pon_frontend_link_config *config)
{
	struct airoha_paged_bosa *bosa = airoha_pon_frontend_priv(frontend);

	bosa->chip->get_link_config(bosa, config);
	return 0;
}

static const struct airoha_pon_frontend_ops bosa_frontend_ops = {
	.prepare = bosa_prepare,
	.unprepare = bosa_unprepare,
	.get_diagnostics = bosa_get_diagnostics,
	.get_link_config = bosa_get_link_config,
	.set_tx_enable = bosa_set_tx_enable,
};

/* Match data selects a fixed chip; a NULL entry selects mixed-material detection. */
static const struct of_device_id bosa_of_match[] = {
	{ .compatible = "semtech,gn28l95", .data = &airoha_bosa_gn28l95 },
	{ .compatible = "uxfastic,ux3363", .data = &airoha_bosa_ux3363 },
	{ .compatible = "fiberhome,hg5x-bosa" },
	{ .compatible = "airoha,paged-bosa" },
	{}
};
MODULE_DEVICE_TABLE(of, bosa_of_match);

static int bosa_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	const struct airoha_paged_bosa_chip *configured;
	const struct airoha_paged_bosa_chip *detected = NULL;
	const struct airoha_paged_bosa_chip *calibrated = NULL;
	struct airoha_paged_bosa *bosa;
	char revision[16] = "unavailable";
	int calibration_ret, ret;

	if (client->addr != 0x51)
		return -EINVAL;
	bosa = devm_kzalloc(dev, sizeof(*bosa), GFP_KERNEL);
	if (!bosa)
		return -ENOMEM;
	bosa->client = client;
	mutex_init(&bosa->lock);
	bosa->ben_active_high =
		device_property_read_bool(dev, "fiberhome,ben-active-high");
	/* The board TX_DISABLE gate covers the soft-gate pulse during initialization. */
	bosa->tx_disable = devm_gpiod_get(dev, "tx-disable", GPIOD_OUT_HIGH);
	if (IS_ERR(bosa->tx_disable))
		return dev_err_probe(dev, PTR_ERR(bosa->tx_disable),
		                     "failed to acquire TX_DISABLE GPIO\n");

	calibration_ret = bosa_load_calibration(bosa);
	if (!calibration_ret)
		calibrated = bosa_chip_by_id(bosa->calibration_id);
	configured = device_get_match_data(dev);
	if (configured) {
		bosa->chip = configured;
		ret = configured->identify(bosa, revision, sizeof(revision));
		if (ret < 0)
			return dev_err_probe(
				dev, ret, "frontend identification failed\n");
		if (!ret)
			strscpy(revision, "not readable", sizeof(revision));
	} else {
		ret = bosa_detect_chip(bosa, &detected, revision,
		                       sizeof(revision));
		if (ret)
			return dev_err_probe(dev, ret,
			                     "frontend detection failed\n");
		bosa->chip = detected ?: calibrated;
		if (!bosa->chip)
			return dev_err_probe(dev, -ENODEV,
			                     "frontend model is unavailable\n");
	}
	if (calibrated && calibrated != bosa->chip)
		return dev_err_probe(dev, -EINVAL,
		                     "calibration is for %s, detected %s\n",
		                     calibrated->name, bosa->chip->name);
	if (!calibration_ret) {
		bosa->calibration_state = AIROHA_PON_FRONTEND_CALIBRATION_READY;
	} else if (calibration_ret == -ENOENT || calibration_ret == -ENODEV ||
	           calibration_ret == -ENODATA) {
		bosa->calibration_state =
			AIROHA_PON_FRONTEND_CALIBRATION_MISSING;
	} else {
		bosa->calibration_state =
			AIROHA_PON_FRONTEND_CALIBRATION_INVALID;
	}

	airoha_pon_frontend_init(&bosa->frontend, dev, THIS_MODULE,
	                         &bosa_frontend_ops, bosa,
	                         bosa->chip->supported_modes);
	ret = airoha_pon_frontend_register(&bosa->frontend);
	if (ret)
		return dev_err_probe(dev, ret,
		                     "frontend registration failed\n");
	i2c_set_clientdata(client, bosa);
	dev_info(dev, "%s frontend registered, ID %s, calibration %s\n",
	         bosa->chip->name, revision,
	         bosa->calibration_state ==
	                         AIROHA_PON_FRONTEND_CALIBRATION_READY ?
	                 "ready" :
	         bosa->calibration_state ==
	                         AIROHA_PON_FRONTEND_CALIBRATION_MISSING ?
	                 "missing" :
	                 "invalid");
	return 0;
}

static void bosa_remove(struct i2c_client *client)
{
	struct airoha_paged_bosa *bosa = i2c_get_clientdata(client);

	airoha_pon_frontend_unregister(&bosa->frontend);
	bosa_unprepare(&bosa->frontend);
}

static void bosa_shutdown(struct i2c_client *client)
{
	struct airoha_paged_bosa *bosa = i2c_get_clientdata(client);

	bosa_unprepare(&bosa->frontend);
}

static struct i2c_driver bosa_driver = {
	.driver = {
		.name = "airoha-paged-bosa",
		.of_match_table = bosa_of_match,
	},
	.probe = bosa_probe,
	.remove = bosa_remove,
	.shutdown = bosa_shutdown,
};
module_i2c_driver(bosa_driver);

MODULE_DESCRIPTION("Paged I2C PON optical frontend provider");
MODULE_LICENSE("GPL");
