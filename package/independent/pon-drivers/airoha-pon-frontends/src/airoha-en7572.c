// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <airoha-pon-frontend.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#define EN7572_RUNTIME_ADDR 0x51

#define EN7572_FW_PM_NAME "airoha/en7572/A60993.elf.pm"
#define EN7572_FW_DM_NAME "airoha/en7572/A60993.elf.dm"

/* MD32 expects 16 KiB PM, 4 KiB DM, and a 512-byte BOB table. */
#define EN7572_PM_SIZE  SZ_16K
#define EN7572_DM_SIZE  SZ_4K
#define EN7572_BOB_SIZE 512

/* Chip IDs in the 0x51 runtime window use little-endian 16-bit values. */
#define EN7572_CHIP_ID_REG  0x0408
#define EN7572_CHIP_REV_REG 0x040a
#define EN7572_CHIP_ID      0x1388

#define EN7572_BEN_CTRL         0x0100
#define EN7572_APC_ANA_VMON_SEL 0x0124
#define EN7572_IMPD_SINK        0x0128
#define EN7572_RESERVE_TIA      0x0130
#define EN7572_REP_PH_CAP_SEL   0x013c
#define EN7572_OCP_CTRL         0x0160
#define EN7572_APD_CTRL         0x015c
#define EN7572_RESET_CTRL       0x0200
#define EN7572_LOOP_CTRL        0x0208
#define EN7572_DCL_CTRL_2       0x0210

#define EN7572_MD32_PM_CFG  0x3000
#define EN7572_MD32_PM_ADDR 0x3004
#define EN7572_MD32_PM_DATA 0x3008
#define EN7572_MD32_DM_CFG  0x300c
#define EN7572_MD32_DM_ADDR 0x3010
#define EN7572_MD32_DM_DATA 0x3014
#define EN7572_MD32_ENABLE  0x3018

/* MD32 reads BOB from DM word address 0x600. */
#define EN7572_BOB_DM_ADDR 0x0600

/* Runtime threshold and alarm fields follow the SFF-8472 layout. */
#define EN7572_AW_THRESHOLDS_REG     0x0000
#define EN7572_TEMPERATURE_REG       0x0060
#define EN7572_VOLTAGE_REG           0x0062
#define EN7572_TX_BIAS_REG           0x0064
#define EN7572_TX_POWER_REG          0x0066
#define EN7572_ALARM_REG             0x0070
#define EN7572_WARNING_REG           0x0074
#define EN7572_FW_VERSION_REG        0x0082
#define EN7572_MCU_IDLE_REG          0x0083
#define EN7572_RX_POWER_REG          0x0068
#define EN7572_RSSI_ADC_REG          0x00f2
#define EN7572_LOS_STATUS_REG        0x00fa
#define EN7572_TX_DISABLE_STATUS_REG 0x03e0
#define EN7572_SYSTEM_STATUS_REG     0x0488

/*
 * The stock runtime restores these 40 bytes when the threshold table is
 * erased. Address 0x51 is volatile; per-device BOB remains in board storage.
 */
static const u8 en7572_aw_thresholds[] = {
	0x64, 0x00, 0xce, 0x00, 0x64, 0x00, 0xce, 0x00, 0x90, 0x88,
	0x71, 0x48, 0x8e, 0x94, 0x73, 0x3c, 0xa6, 0x05, 0x01, 0xf4,
	0x9c, 0x40, 0x02, 0xee, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff,
	0x00, 0x00, 0x31, 0x24, 0x00, 0x01, 0x27, 0x10, 0x00, 0x03,
};

/* BEN_CTRL[3:2] = 2 closes the burst-enable gate. */
#define EN7572_BEN_MODE_MASK   GENMASK(3, 2)
#define EN7572_BEN_MODE_OFF    FIELD_PREP(EN7572_BEN_MODE_MASK, 2)
#define EN7572_BEN_MODE_NORMAL FIELD_PREP(EN7572_BEN_MODE_MASK, 0)

/* Electrical settings from the ECONET/EN7572 transceiver IOT entry. */
#define EN7572_GEPON_XPON_SETTING    0x0000014f
#define EN7572_XGPON_SFP_VALID_LEVEL 0x00000009
#define EN7572_XEPON_SFP_STATUS      0xdf800000
#define EN7572_PMA_XPON_SETTING0     0x00010001
#define EN7572_PMA_XPON_SETTING1     0x01010100

/* XE/XES use A0; XG/XGS use A2. */
#define EN7572_BOB_A0_BASE    0
#define EN7572_BOB_A2_BASE    256
#define EN7572_BOB_CAL_FIRST  0x80
#define EN7572_BOB_CAL_LAST   0xd7
#define EN7572_BOB_IMOD_CAL   0x86
#define EN7572_BOB_IAV_CAL    0x88
#define EN7572_BOB_APC_CAL    0x8a
#define EN7572_BOB_TIA_CUR    0x8c
#define EN7572_BOB_ERC_CDAC   0x8d
#define EN7572_BOB_ERC_DAC    0x8e
#define EN7572_BOB_TIA_GAIN   0x90
#define EN7572_BOB_TIA_BW     0x91
#define EN7572_BOB_PGA_GAIN   0x92
#define EN7572_BOB_PGA_CAP    0x93
#define EN7572_BOB_TSSI_CAL_1 0xb4

struct en7572 {
	struct airoha_pon_frontend frontend;
	struct i2c_client *program;
	struct i2c_client *runtime;
	struct gpio_desc *tx_disable;
	struct mutex lock;
	u8 *pm;
	u8 *dm;
	u8 *bob;
	u16 chip_id;
	u16 chip_rev;
	bool bob_a0_valid;
	bool bob_a2_valid;
	bool prepared;
	enum airoha_pon_frontend_mode prepared_mode;
	bool tx_gate_enabled;
};

static u8 en7572_bob_u8(struct en7572 *en, unsigned int base,
                        unsigned int offset)
{
	return en->bob[base + offset];
}

static u16 en7572_bob_u16(struct en7572 *en, unsigned int base,
                          unsigned int offset)
{
	return get_unaligned_le16(en->bob + base + offset);
}

static bool en7572_bob_table_valid(const u8 *table)
{
	bool has_nonzero = false;
	bool has_non_ff = false;
	unsigned int offset;

	/* BOB has no magic; erased tables contain 0xff or 0x00 throughout. */
	for (offset = EN7572_BOB_CAL_FIRST; offset <= EN7572_BOB_CAL_LAST;
	     offset++) {
		has_nonzero |= table[offset] != 0;
		has_non_ff |= table[offset] != 0xff;
	}

	return has_nonzero && has_non_ff;
}

static void en7572_apply_tx_ddmi_fallback(struct device *dev, struct en7572 *en)
{
	u8 *a0 = en->bob + EN7572_BOB_A0_BASE;
	const u8 *a2 = en->bob + EN7572_BOB_A2_BASE;
	u32 a0_tssi = get_unaligned_le32(a0 + EN7572_BOB_TSSI_CAL_1);
	u32 a2_tssi = get_unaligned_le32(a2 + EN7572_BOB_TSSI_CAL_1);

	/* Keep both wavelength profiles on the same TX DDMI code when A0 is empty. */
	if (a0_tssi != U32_MAX)
		return;

	memcpy(a0 + EN7572_BOB_TSSI_CAL_1, a2 + EN7572_BOB_TSSI_CAL_1,
	       sizeof(a0_tssi));
	dev_warn(dev, "BOB A0 TX DDMI is erased; copied 4 bytes from A2%s\n",
	         a2_tssi == U32_MAX ? " (A2 source is also erased)" : "");
}

/* EN7572 reads use a big-endian 16-bit subaddress and repeated start. */
static int en7572_read(struct i2c_client *client, u16 reg, void *data,
                       size_t len)
{
	u8 address[2] = { reg >> 8, reg };
	struct i2c_msg messages[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(address),
			.buf = address,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = data,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));
	if (ret == ARRAY_SIZE(messages))
		return 0;

	return ret < 0 ? ret : -EIO;
}

static int en7572_write(struct i2c_client *client, u16 reg, const void *data,
                        size_t len)
{
	u8 buffer[2 + sizeof(u32)];
	struct i2c_msg message = {
		.addr = client->addr,
		.flags = 0,
		.buf = buffer,
	};
	int ret;

	if (len > sizeof(u32))
		return -EINVAL;

	buffer[0] = reg >> 8;
	buffer[1] = reg;
	memcpy(buffer + 2, data, len);
	message.len = 2 + len;

	ret = i2c_transfer(client->adapter, &message, 1);
	if (ret == 1)
		return 0;

	return ret < 0 ? ret : -EIO;
}

/*
 * The threshold table uses one I2C transaction with an incrementing
 * subaddress. A dedicated transfer handles payloads beyond one CSR word.
 */
static int en7572_write_aw_thresholds(struct en7572 *en)
{
	u8 buffer[2 + sizeof(en7572_aw_thresholds)];
	struct i2c_msg message = {
		.addr = en->runtime->addr,
		.flags = 0,
		.len = sizeof(buffer),
		.buf = buffer,
	};
	int ret;

	buffer[0] = EN7572_AW_THRESHOLDS_REG >> 8;
	buffer[1] = EN7572_AW_THRESHOLDS_REG;
	memcpy(buffer + 2, en7572_aw_thresholds, sizeof(en7572_aw_thresholds));

	ret = i2c_transfer(en->runtime->adapter, &message, 1);
	if (ret == 1)
		return 0;

	return ret < 0 ? ret : -EIO;
}

static int en7572_read_u16(struct i2c_client *client, u16 reg, u16 *value)
{
	u8 data[2];
	int ret;

	ret = en7572_read(client, reg, data, sizeof(data));
	if (!ret)
		*value = get_unaligned_le16(data);

	return ret;
}

/* A2 DDMI stores 16-bit linear values in SFF-8472 network byte order. */
static int en7572_read_be16(struct i2c_client *client, u16 reg, u16 *value)
{
	u8 data[2];
	int ret;

	ret = en7572_read(client, reg, data, sizeof(data));
	if (!ret)
		*value = get_unaligned_be16(data);

	return ret;
}

static int en7572_read_u32(struct i2c_client *client, u16 reg, u32 *value)
{
	u8 data[4];
	int ret;

	ret = en7572_read(client, reg, data, sizeof(data));
	if (!ret)
		*value = get_unaligned_le32(data);

	return ret;
}

static int en7572_write_u16(struct i2c_client *client, u16 reg, u16 value)
{
	u8 data[2];

	put_unaligned_le16(value, data);
	return en7572_write(client, reg, data, sizeof(data));
}

static int en7572_write_u32(struct i2c_client *client, u16 reg, u32 value)
{
	u8 data[4];

	put_unaligned_le32(value, data);
	return en7572_write(client, reg, data, sizeof(data));
}

/* Bit updates in the 0x51 window use little-endian 32-bit read-modify-write. */
static int en7572_update_bits(struct en7572 *en, u16 reg, u32 mask, u32 value)
{
	u8 data[4];
	u32 old, new;
	int ret;

	ret = en7572_read(en->runtime, reg, data, sizeof(data));
	if (ret)
		return ret;

	old = get_unaligned_le32(data);
	new = (old & ~mask) | (value & mask);
	if (new == old)
		return 0;

	put_unaligned_le32(new, data);
	return en7572_write(en->runtime, reg, data, sizeof(data));
}

/* Zero-fill the rest of each PM/DM window for the MD32 loader. */
static int en7572_load_blob(struct device *dev, const char *name, u8 *buffer,
                            size_t capacity)
{
	const struct firmware *firmware;
	int ret;

	ret = request_firmware(&firmware, name, dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to load firmware %s\n",
		                     name);

	if (!firmware->size || firmware->size > capacity) {
		dev_err(dev, "%s has invalid size %zu; maximum is %zu\n", name,
		        firmware->size, capacity);
		ret = -EINVAL;
		goto out_release;
	}

	memcpy(buffer, firmware->data, firmware->size);
	ret = 0;

out_release:
	release_firmware(firmware);
	return ret;
}

static int en7572_load_bob(struct device *dev, struct en7572 *en)
{
	struct nvmem_cell *cell;
	void *data;
	size_t size;
	int ret;
	bool loaded = false;

	/*
	 * MD32 starts with a 512-byte erased BOB when board calibration is absent.
	 * NVMEM replaces this volatile DM table when available.
	 */
	memset(en->bob, 0xff, EN7572_BOB_SIZE);

	/* The NVMEM cell maps the board storage format to MD32's 512-byte BOB. */
	cell = devm_nvmem_cell_get(dev, "calibration");
	if (IS_ERR(cell)) {
		ret = PTR_ERR(cell);
		if (ret == -EPROBE_DEFER)
			return ret;

		dev_warn(
			dev,
			"PON calibration NVMEM is unavailable (%d); using erased stock-compatible BOB\n",
			ret);
		goto validate;
	}

	data = nvmem_cell_read(cell, &size);
	if (IS_ERR(data)) {
		ret = PTR_ERR(data);
		if (ret == -EPROBE_DEFER)
			return ret;

		dev_warn(
			dev,
			"failed to read EN7572 calibration backup (%d); using erased stock-compatible BOB\n",
			ret);
		goto validate;
	}

	if (size != EN7572_BOB_SIZE) {
		dev_warn(
			dev,
			"BOSA data size is %zu, expected %u; using erased stock-compatible BOB\n",
			size, EN7572_BOB_SIZE);
		kfree(data);
		goto validate;
	}

	memcpy(en->bob, data, EN7572_BOB_SIZE);
	kfree(data);
	loaded = true;

validate:
	en->bob_a0_valid = en7572_bob_table_valid(en->bob);
	en->bob_a2_valid = en7572_bob_table_valid(en->bob + EN7572_BOB_A2_BASE);

	if (!en->bob_a0_valid)
		dev_warn(
			dev,
			"BOB A0 calibration area is empty; XE/XES will use erased/default register codes\n");
	if (!en->bob_a2_valid)
		dev_warn(
			dev,
			"BOB A2 calibration area is empty; XG/XGS will use erased/default register codes\n");

	en7572_apply_tx_ddmi_fallback(dev, en);

	dev_info(dev, "BOB %s: A0=%s A2=%s\n",
	         loaded ? "loaded from NVMEM" : "using erased fallback",
	         en->bob_a0_valid ? "valid" : "empty",
	         en->bob_a2_valid ? "valid" : "empty");

	return 0;
}

static void en7572_normalize_bob_identity(struct en7572 *en)
{
	/* MD32 expects canonical vendor and device names in these 16-byte fields. */
	memcpy(en->bob + 20, "ECONET          ", 16);
	memcpy(en->bob + 40, "EN7572          ", 16);
}

static int en7572_load_words(struct en7572 *en, u16 data_reg, const u8 *data,
                             size_t size, const char *name)
{
	size_t offset;
	int ret;

	for (offset = 0; offset < size; offset += sizeof(u32)) {
		ret = en7572_write(en->program, data_reg, data + offset,
		                   sizeof(u32));
		if (ret) {
			dev_err(&en->program->dev,
			        "failed to write %s at word %zu: %d\n", name,
			        offset / sizeof(u32), ret);
			return ret;
		}
	}

	return 0;
}

static int en7572_initialize_aw_thresholds(struct en7572 *en)
{
	struct device *dev = &en->program->dev;
	u32 first_thresholds;
	int ret;

	/*
	 * The stock runtime checks the first four bytes for an erased threshold
	 * table. A populated table retains values supplied by MD32 or the module.
	 */
	ret = en7572_read_u32(en->runtime, EN7572_AW_THRESHOLDS_REG,
	                      &first_thresholds);
	if (ret || first_thresholds != U32_MAX)
		return ret;

	ret = en7572_write_aw_thresholds(en);
	if (ret)
		return ret;

	dev_info(
		dev,
		"installed stock alarm/warning thresholds in the volatile runtime table\n");
	return 0;
}

static int en7572_initialize_md32(struct en7572 *en)
{
	int ret;

	/* Close the burst gate before reset and loading begin. */
	ret = en7572_update_bits(en, EN7572_BEN_CTRL, EN7572_BEN_MODE_MASK,
	                         EN7572_BEN_MODE_OFF);
	if (ret)
		return ret;

	ret = en7572_update_bits(en, EN7572_MD32_ENABLE, BIT(0), 0);
	if (ret)
		return ret;

	/* MD32 startup requires OCP_CTRL[30] and APD_CTRL[8] cleared. */
	ret = en7572_update_bits(en, EN7572_OCP_CTRL, BIT(30), 0);
	if (ret)
		return ret;

	ret = en7572_update_bits(en, EN7572_APD_CTRL, BIT(8), 0);
	if (ret)
		return ret;

	msleep(100);

	/* EN7572A releases reset after RESET_CTRL[31:30] transitions 0 to 3. */
	ret = en7572_update_bits(en, EN7572_RESET_CTRL, GENMASK(31, 30), 0);
	if (ret)
		return ret;

	ret = en7572_update_bits(en, EN7572_RESET_CTRL, GENMASK(31, 30),
	                         GENMASK(31, 30));
	if (ret)
		return ret;

	/* The PM load window accepts 4096 sequential 32-bit words from address 0. */
	ret = en7572_update_bits(en, EN7572_MD32_PM_CFG, BIT(0), BIT(0));
	if (ret)
		return ret;

	ret = en7572_write_u32(en->runtime, EN7572_MD32_PM_ADDR, 0);
	if (ret)
		return ret;

	ret = en7572_load_words(en, EN7572_MD32_PM_DATA, en->pm, EN7572_PM_SIZE,
	                        "PM");
	if (ret)
		return ret;

	/* The DM load window accepts 1024 sequential 32-bit words from address 0. */
	ret = en7572_update_bits(en, EN7572_MD32_DM_CFG, BIT(0), BIT(0));
	if (ret)
		return ret;

	ret = en7572_write_u32(en->runtime, EN7572_MD32_DM_ADDR, 0);
	if (ret)
		return ret;

	ret = en7572_load_words(en, EN7572_MD32_DM_DATA, en->dm, EN7572_DM_SIZE,
	                        "DM");
	if (ret)
		return ret;

	/* BOB shares the DM data port at the firmware-defined word address 0x600. */
	en7572_normalize_bob_identity(en);
	ret = en7572_update_bits(en, EN7572_MD32_DM_CFG, BIT(0), BIT(0));
	if (ret)
		return ret;

	ret = en7572_write_u32(en->runtime, EN7572_MD32_DM_ADDR,
	                       EN7572_BOB_DM_ADDR);
	if (ret)
		return ret;

	ret = en7572_load_words(en, EN7572_MD32_DM_DATA, en->bob,
	                        EN7572_BOB_SIZE, "BOB");
	if (ret)
		return ret;

	/* Restore erased runtime thresholds after PM, DM, and BOB loading. */
	ret = en7572_initialize_aw_thresholds(en);
	if (ret)
		return ret;

	/* Reset the volatile alarm and warning images. */
	ret = en7572_write_u16(en->runtime, EN7572_ALARM_REG, 0);
	if (ret)
		return ret;

	ret = en7572_write_u16(en->runtime, EN7572_WARNING_REG, 0);
	if (ret)
		return ret;

	ret = en7572_update_bits(en, EN7572_MD32_ENABLE, BIT(0), BIT(0));
	if (ret)
		return ret;

	/* Keep BEN off across MD32 startup and its register initialization. */
	msleep(20);
	return en7572_update_bits(en, EN7572_BEN_CTRL, EN7572_BEN_MODE_MASK,
	                          EN7572_BEN_MODE_OFF);
}

static int en7572_bob_base(enum airoha_pon_frontend_mode mode)
{
	switch (mode) {
	case AIROHA_PON_FRONTEND_MODE_GPON:
	case AIROHA_PON_FRONTEND_MODE_XGPON:
	case AIROHA_PON_FRONTEND_MODE_XGSPON:
	case AIROHA_PON_FRONTEND_MODE_EPON_1G:
		/*
		 * AdaptivePon(0) selects the A2 startup profile for GPON and 1G EPON.
		 * XG/XGS and 10G EPON select their profiles explicitly.
		 */
		return EN7572_BOB_A2_BASE;
	case AIROHA_PON_FRONTEND_MODE_EPON_10G_1G:
	case AIROHA_PON_FRONTEND_MODE_EPON_10G_10G:
		return EN7572_BOB_A0_BASE;
	default:
		return -EOPNOTSUPP;
	}
}

static const char *en7572_mode_name(enum airoha_pon_frontend_mode mode)
{
	switch (mode) {
	case AIROHA_PON_FRONTEND_MODE_GPON:
		return "gpon-a2";
	case AIROHA_PON_FRONTEND_MODE_XGPON:
		return "xgpon-a2";
	case AIROHA_PON_FRONTEND_MODE_XGSPON:
		return "xgspon-a2";
	case AIROHA_PON_FRONTEND_MODE_EPON_1G:
		return "epon-1g-a2";
	case AIROHA_PON_FRONTEND_MODE_EPON_10G_1G:
		return "epon-10g-1g-a0";
	case AIROHA_PON_FRONTEND_MODE_EPON_10G_10G:
		return "epon-10g-10g-a0";
	default:
		return "unsupported";
	}
}

/* AdaptivePon() selects A2 for XG/XGS and A0 for XE/XES. IMOD/IAV/APC/ERC
 * affect TX bias, while TIA/PGA affect RX gain.
 */
static int en7572_prepare(struct airoha_pon_frontend *frontend,
                          enum airoha_pon_frontend_mode mode)
{
	struct en7572 *en = airoha_pon_frontend_priv(frontend);
	int base;
	u32 tia;
	int ret;

	base = en7572_bob_base(mode);
	if (base < 0)
		return base;

	/*
	 * AdaptivePon() truncates erased bytes to each register's field width.
	 * PON MAC retains control of burst timing with these default codes.
	 */
	if ((base == EN7572_BOB_A0_BASE && !en->bob_a0_valid) ||
	    (base == EN7572_BOB_A2_BASE && !en->bob_a2_valid))
		dev_warn(
			&en->program->dev,
			"preparing %s with an empty BOB table using stock-compatible register codes\n",
			en7572_mode_name(mode));

	mutex_lock(&en->lock);
	en->prepared = false;
	en->tx_gate_enabled = false;

	ret = en7572_update_bits(en, EN7572_BEN_CTRL, EN7572_BEN_MODE_MASK,
	                         EN7572_BEN_MODE_OFF);
	if (ret)
		goto out_unlock;

	ret = en7572_update_bits(
		en, EN7572_DCL_CTRL_2, GENMASK(27, 16),
		FIELD_PREP(GENMASK(27, 16),
	                   en7572_bob_u16(en, base, EN7572_BOB_IMOD_CAL)));
	if (ret)
		goto out_unlock;

	ret = en7572_update_bits(
		en, EN7572_DCL_CTRL_2, GENMASK(12, 0),
		FIELD_PREP(GENMASK(12, 0),
	                   en7572_bob_u16(en, base, EN7572_BOB_IAV_CAL)));
	if (ret)
		goto out_unlock;

	ret = en7572_update_bits(
		en, EN7572_APC_ANA_VMON_SEL, GENMASK(15, 8),
		FIELD_PREP(GENMASK(15, 8),
	                   en7572_bob_u8(en, base, EN7572_BOB_APC_CAL)));
	if (ret)
		goto out_unlock;

	ret = en7572_update_bits(
		en, EN7572_RESERVE_TIA, BIT(0),
		en7572_bob_u8(en, base, EN7572_BOB_TIA_CUR) ? BIT(0) : 0);
	if (ret)
		goto out_unlock;

	ret = en7572_update_bits(
		en, EN7572_IMPD_SINK, GENMASK(15, 8),
		FIELD_PREP(GENMASK(15, 8),
	                   en7572_bob_u8(en, base, EN7572_BOB_ERC_CDAC)));
	if (ret)
		goto out_unlock;

	ret = en7572_update_bits(
		en, EN7572_IMPD_SINK, GENMASK(27, 16),
		FIELD_PREP(GENMASK(27, 16),
	                   en7572_bob_u16(en, base, EN7572_BOB_ERC_DAC)));
	if (ret)
		goto out_unlock;

	tia = (en7572_bob_u8(en, base, EN7572_BOB_TIA_BW) << 3) |
	      en7572_bob_u8(en, base, EN7572_BOB_TIA_GAIN);
	ret = en7572_update_bits(en, EN7572_RESERVE_TIA, GENMASK(13, 8),
	                         FIELD_PREP(GENMASK(13, 8), tia));
	if (ret)
		goto out_unlock;

	ret = en7572_update_bits(
		en, EN7572_REP_PH_CAP_SEL, GENMASK(18, 16),
		FIELD_PREP(GENMASK(18, 16),
	                   en7572_bob_u8(en, base, EN7572_BOB_PGA_GAIN)));
	if (ret)
		goto out_unlock;

	ret = en7572_update_bits(
		en, EN7572_REP_PH_CAP_SEL, GENMASK(6, 5),
		FIELD_PREP(GENMASK(6, 5),
	                   en7572_bob_u8(en, base, EN7572_BOB_PGA_CAP)));
	if (ret)
		goto out_unlock;

	ret = en7572_write(en->runtime, EN7572_BOB_TSSI_CAL_1,
	                   en->bob + base + EN7572_BOB_TSSI_CAL_1, sizeof(u32));
	if (ret)
		goto out_unlock;

	/* The 0-to-1 rg_loop_en pulse commits the new analog settings with BEN off. */
	ret = en7572_update_bits(en, EN7572_LOOP_CTRL, BIT(0), 0);
	if (!ret)
		ret = en7572_update_bits(en, EN7572_LOOP_CTRL, BIT(0), BIT(0));
	if (!ret) {
		en->prepared_mode = mode;
		en->prepared = true;
	}

out_unlock:
	mutex_unlock(&en->lock);
	return ret;
}

static int
en7572_get_signal_status(struct airoha_pon_frontend *frontend,
                         struct airoha_pon_frontend_signal_status *status)
{
	struct en7572 *en = airoha_pon_frontend_priv(frontend);
	u8 los_raw;
	int ret;

	/* LOS register 0xFA bit 0 is set when optical input is absent. */
	mutex_lock(&en->lock);
	ret = en7572_read(en->runtime, EN7572_LOS_STATUS_REG, &los_raw,
	                  sizeof(los_raw));
	mutex_unlock(&en->lock);
	if (ret)
		return ret;

	status->los_raw = los_raw;
	status->signal_present = !(los_raw & BIT(0));
	status->signal_valid = true;
	return 0;
}

static int
en7572_get_diagnostics(struct airoha_pon_frontend *frontend,
                       struct airoha_pon_frontend_diagnostics *diagnostics)
{
	struct en7572 *en = airoha_pon_frontend_priv(frontend);
	u16 temperature;
	int bob_base;
	int ret;

	/* Registers 0x60–0x69 use SFF-8472 linear DDMI units. */
	mutex_lock(&en->lock);
	ret = en7572_read_be16(en->runtime, EN7572_TEMPERATURE_REG,
	                       &temperature);
	if (!ret)
		ret = en7572_read_be16(en->runtime, EN7572_VOLTAGE_REG,
		                       &diagnostics->voltage);
	if (!ret)
		ret = en7572_read_be16(en->runtime, EN7572_TX_BIAS_REG,
		                       &diagnostics->tx_bias);
	if (!ret)
		ret = en7572_read_be16(en->runtime, EN7572_TX_POWER_REG,
		                       &diagnostics->tx_power);
	if (!ret)
		ret = en7572_read_be16(en->runtime, EN7572_RX_POWER_REG,
		                       &diagnostics->rx_power);
	if (!ret) {
		diagnostics->temperature = (s16)temperature;
		diagnostics->valid = AIROHA_PON_FRONTEND_DIAG_TEMPERATURE |
		                     AIROHA_PON_FRONTEND_DIAG_VOLTAGE |
		                     AIROHA_PON_FRONTEND_DIAG_TX_BIAS |
		                     AIROHA_PON_FRONTEND_DIAG_TX_POWER |
		                     AIROHA_PON_FRONTEND_DIAG_RX_POWER;
		diagnostics->tx_gate_enabled = en->tx_gate_enabled;
		bob_base = en->prepared ? en7572_bob_base(en->prepared_mode) :
		                          -EINVAL;
		if ((bob_base == EN7572_BOB_A0_BASE && en->bob_a0_valid) ||
		    (bob_base == EN7572_BOB_A2_BASE && en->bob_a2_valid) ||
		    (bob_base < 0 && en->bob_a0_valid && en->bob_a2_valid))
			diagnostics->calibration =
				AIROHA_PON_FRONTEND_CALIBRATION_READY;
		else if (bob_base >= 0 ||
		         (!en->bob_a0_valid && !en->bob_a2_valid))
			diagnostics->calibration =
				AIROHA_PON_FRONTEND_CALIBRATION_MISSING;
		else
			diagnostics->calibration =
				AIROHA_PON_FRONTEND_CALIBRATION_UNKNOWN;
	}
	mutex_unlock(&en->lock);

	return ret;
}

static int
en7572_get_link_config(struct airoha_pon_frontend *frontend,
                       enum airoha_pon_frontend_mode mode,
                       struct airoha_pon_frontend_link_config *config)
{
	/* The PON PHY and PCS consume chip-specific electrical settings. */
	config->gepon_xpon_setting = EN7572_GEPON_XPON_SETTING;
	config->xgpon_sfp_valid_level = EN7572_XGPON_SFP_VALID_LEVEL;
	config->xepon_sfp_status = EN7572_XEPON_SFP_STATUS;
	config->pma_xpon_setting0 = EN7572_PMA_XPON_SETTING0;
	config->pma_xpon_setting1 = EN7572_PMA_XPON_SETTING1;

	return 0;
}

static int en7572_set_tx_enable(struct airoha_pon_frontend *frontend,
                                bool enable)
{
	struct en7572 *en = airoha_pon_frontend_priv(frontend);
	int ret;

	if (enable && !en->prepared)
		return -EAGAIN;

	mutex_lock(&en->lock);
	if (enable) {
		/* Enter burst mode before releasing the board TX_DISABLE gate. */
		ret = en7572_update_bits(en, EN7572_BEN_CTRL,
		                         EN7572_BEN_MODE_MASK,
		                         EN7572_BEN_MODE_NORMAL);
		if (!ret) {
			if (en->tx_disable)
				gpiod_set_value_cansleep(en->tx_disable, 0);
			en->tx_gate_enabled = true;
		}
	} else {
		/* Assert board TX_DISABLE before closing the internal BEN gate. */
		if (en->tx_disable)
			gpiod_set_value_cansleep(en->tx_disable, 1);
		en->tx_gate_enabled = false;
		ret = en7572_update_bits(en, EN7572_BEN_CTRL,
		                         EN7572_BEN_MODE_MASK,
		                         EN7572_BEN_MODE_OFF);
	}
	mutex_unlock(&en->lock);

	return ret;
}

static int en7572_unprepare(struct airoha_pon_frontend *frontend)
{
	struct en7572 *en = airoha_pon_frontend_priv(frontend);
	int ret;

	/* The board GPIO closes the external TX path before BEN changes. */
	mutex_lock(&en->lock);
	if (en->tx_disable)
		gpiod_set_value_cansleep(en->tx_disable, 1);
	en->tx_gate_enabled = false;
	en->prepared = false;
	ret = en7572_update_bits(en, EN7572_BEN_CTRL, EN7572_BEN_MODE_MASK,
	                         EN7572_BEN_MODE_OFF);
	mutex_unlock(&en->lock);

	return ret;
}

static const struct airoha_pon_frontend_ops en7572_frontend_ops = {
	.prepare = en7572_prepare,
	.unprepare = en7572_unprepare,
	.get_signal_status = en7572_get_signal_status,
	.get_diagnostics = en7572_get_diagnostics,
	.get_link_config = en7572_get_link_config,
	.set_tx_enable = en7572_set_tx_enable,
};

static int en7572_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct en7572 *en;
	u32 supported_modes =
		AIROHA_PON_FRONTEND_MODE_BIT(AIROHA_PON_FRONTEND_MODE_GPON) |
		AIROHA_PON_FRONTEND_MODE_BIT(AIROHA_PON_FRONTEND_MODE_XGPON) |
		AIROHA_PON_FRONTEND_MODE_BIT(AIROHA_PON_FRONTEND_MODE_XGSPON) |
		AIROHA_PON_FRONTEND_MODE_BIT(AIROHA_PON_FRONTEND_MODE_EPON_1G) |
		AIROHA_PON_FRONTEND_MODE_BIT(
			AIROHA_PON_FRONTEND_MODE_EPON_10G_1G) |
		AIROHA_PON_FRONTEND_MODE_BIT(
			AIROHA_PON_FRONTEND_MODE_EPON_10G_10G);
	u8 fw_version, los_status;
	int ret;

	if (client->addr != 0x50)
		return dev_err_probe(
			dev, -EINVAL,
			"the device tree must expose the MD32 window at I2C address 0x50\n");

	en = devm_kzalloc(dev, sizeof(*en), GFP_KERNEL);
	if (!en)
		return -ENOMEM;

	en->program = client;
	mutex_init(&en->lock);
	/* GPIO descriptors translate logical assertion to board-level polarity. */
	en->tx_disable =
		devm_gpiod_get_optional(dev, "tx-disable", GPIOD_OUT_HIGH);
	if (IS_ERR(en->tx_disable))
		return dev_err_probe(
			dev, PTR_ERR(en->tx_disable),
			"failed to acquire exclusive ownership of the board TX_DISABLE GPIO\n");

	/* The dummy client reserves EN7572's second I2C address, 0x50. */
	en->runtime = devm_i2c_new_dummy_device(dev, client->adapter,
	                                        EN7572_RUNTIME_ADDR);
	if (IS_ERR(en->runtime))
		return dev_err_probe(
			dev, PTR_ERR(en->runtime),
			"failed to claim the EN7572 runtime address 0x51\n");

	ret = en7572_read_u16(en->runtime, EN7572_CHIP_ID_REG, &en->chip_id);
	if (ret)
		return dev_err_probe(dev, ret,
		                     "failed to read the EN7572 chip ID\n");

	ret = en7572_read_u16(en->runtime, EN7572_CHIP_REV_REG, &en->chip_rev);
	if (ret)
		return dev_err_probe(
			dev, ret, "failed to read the EN7572 chip revision\n");

	if (en->chip_id != EN7572_CHIP_ID)
		return dev_err_probe(
			dev, -ENODEV,
			"chip ID 0x%04x at I2C 0x51 is not EN7572\n",
			en->chip_id);

	en->pm = devm_kzalloc(dev, EN7572_PM_SIZE, GFP_KERNEL);
	en->dm = devm_kzalloc(dev, EN7572_DM_SIZE, GFP_KERNEL);
	en->bob = devm_kzalloc(dev, EN7572_BOB_SIZE, GFP_KERNEL);
	if (!en->pm || !en->dm || !en->bob)
		return -ENOMEM;

	/* MD32 starts after firmware and BOB loading complete. */
	ret = en7572_load_blob(dev, EN7572_FW_PM_NAME, en->pm, EN7572_PM_SIZE);
	if (ret)
		return ret;

	ret = en7572_load_blob(dev, EN7572_FW_DM_NAME, en->dm, EN7572_DM_SIZE);
	if (ret)
		return ret;

	ret = en7572_load_bob(dev, en);
	if (ret)
		return ret;

	ret = en7572_initialize_md32(en);
	if (ret)
		return dev_err_probe(dev, ret,
		                     "EN7572 MD32 initialization failed\n");

	ret = en7572_read(en->runtime, EN7572_FW_VERSION_REG, &fw_version,
	                  sizeof(fw_version));
	if (ret)
		return dev_err_probe(
			dev, ret,
			"failed to read the EN7572 firmware version\n");

	ret = en7572_read(en->runtime, EN7572_LOS_STATUS_REG, &los_status,
	                  sizeof(los_status));
	if (ret)
		return dev_err_probe(dev, ret,
		                     "failed to read the EN7572 LOS status\n");

	/* Line capabilities follow the chip and BOSA type; diagnostics report calibration. */
	airoha_pon_frontend_init(&en->frontend, dev, THIS_MODULE,
	                         &en7572_frontend_ops, en, supported_modes);
	i2c_set_clientdata(client, &en->frontend);
	ret = airoha_pon_frontend_register(&en->frontend);
	if (ret) {
		i2c_set_clientdata(client, NULL);
		return dev_err_probe(
			dev, ret, "failed to register the EN7572 frontend\n");
	}

	dev_info(
		dev,
		"MD32 initialized, chip=0x%04x rev=0x%04x fw=0x%02x los=0x%02x, burst TX disabled%s\n",
		en->chip_id, en->chip_rev, fw_version, los_status,
		en->tx_disable ? ", board TX_DISABLE asserted" : "");

	return 0;
}

static void en7572_remove(struct i2c_client *client)
{
	struct airoha_pon_frontend *frontend = i2c_get_clientdata(client);
	struct en7572 *en = airoha_pon_frontend_priv(frontend);

	airoha_pon_frontend_unregister(frontend);
	/* Close the external TX path before the internal BEN gate. */
	mutex_lock(&en->lock);
	if (en->tx_disable)
		gpiod_set_value_cansleep(en->tx_disable, 1);
	en7572_update_bits(en, EN7572_BEN_CTRL, EN7572_BEN_MODE_MASK,
	                   EN7572_BEN_MODE_OFF);
	en->tx_gate_enabled = false;
	mutex_unlock(&en->lock);
	i2c_set_clientdata(client, NULL);
}

static const struct of_device_id en7572_of_match[] = {
	{ .compatible = "airoha,en7572" },
	{}
};
MODULE_DEVICE_TABLE(of, en7572_of_match);

static const struct i2c_device_id en7572_ids[] = { { "en7572" }, {} };
MODULE_DEVICE_TABLE(i2c, en7572_ids);

static struct i2c_driver en7572_driver = {
	.probe = en7572_probe,
	.remove = en7572_remove,
	.id_table = en7572_ids,
	.driver = {
		.name = "airoha-en7572",
		.of_match_table = en7572_of_match,
	},
};
module_i2c_driver(en7572_driver);

MODULE_AUTHOR("pbs05 <27010143+pbs05@users.noreply.github.com>");
MODULE_DESCRIPTION("Airoha EN7572 MD32 and BOB loader");
MODULE_LICENSE("GPL");
