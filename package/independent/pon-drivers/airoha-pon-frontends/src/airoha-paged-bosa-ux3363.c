// SPDX-License-Identifier: GPL-2.0-only
#include "airoha-paged-bosa.h"

#include <linux/delay.h>

#define UX_CALIBRATION_LENGTH 0x780

static int ux3363_identify(struct airoha_paged_bosa *bosa, char *revision,
                           size_t revision_size)
{
	u8 id[7];
	int ret, restore;

	ret = airoha_paged_bosa_select_page(bosa, 0x87);
	if (!ret)
		ret = airoha_paged_bosa_read(bosa, 0x80, id, sizeof(id));
	restore = airoha_paged_bosa_select_page(bosa, 0);
	if (!ret)
		ret = restore;
	if (ret)
		return ret;
	if (memcmp(id, "UX3363", 6))
		return 0;
	snprintf(revision, revision_size, "%7phN", id);
	return 1;
}

static int ux3363_write_range(struct airoha_paged_bosa *bosa, u8 page, u8 first,
                              const u8 *data, size_t length)
{
	return airoha_paged_bosa_write_range(bosa, page, first, data, length,
	                                     1000);
}

static int ux3363_apply(struct airoha_paged_bosa *bosa, const u8 *data,
                        size_t length)
{
	u8 value;
	int ret;

	if (length != UX_CALIBRATION_LENGTH)
		return -EINVAL;

	/* Page 0x80, register 0xb7 bit 0 enables volatile configuration writes.
	 * The payload uses the chip's native 0x780-byte register image.
	 */
	ret = airoha_paged_bosa_select_page(bosa, 0x80);
	if (ret)
		return ret;
	usleep_range(1000, 1500);
	ret = airoha_paged_bosa_read(bosa, 0xb7, &value, 1);
	if (ret)
		return ret;
	if (!(value & BIT(0))) {
		ret = airoha_paged_bosa_write(bosa, 0xb7, value | BIT(0));
		if (ret)
			return ret;
		usleep_range(10000, 11000);
	}

	ret = ux3363_write_range(bosa, 0x81, 0x80, data + 0x280, 105);
	if (ret)
		return ret;
	ret = ux3363_write_range(bosa, 0x82, 0x80, data + 0x300, 105);
	if (ret)
		return ret;
	ret = ux3363_write_range(bosa, 0x83, 0x80, data + 0x400, 105);
	if (ret)
		return ret;
	ret = ux3363_write_range(bosa, 0x84, 0x80, data + 0x380, 84);
	if (ret)
		return ret;
	/* Registers 0xd4–0xd5 and 0xde–0xdf are reserved by the chip. */
	ret = ux3363_write_range(bosa, 0x84, 0xd6, data + 0x3d6, 8);
	if (ret)
		return ret;
	ret = ux3363_write_range(bosa, 0x84, 0xe0, data + 0x3e0, 11);
	if (ret)
		return ret;
	ret = ux3363_write_range(bosa, 0x00, 0x00, data + 0x100, 123);
	if (ret)
		return ret;

	/* Keep page 0x80, register 0xb7 bit 0 set for the completed image. */
	ret = airoha_paged_bosa_select_page(bosa, 0x80);
	if (ret)
		return ret;
	for (value = 0; value < 111; value++) {
		u8 reg = 0x80 + value;
		u8 byte = data[0x200 + value];

		if (reg == 0xb7)
			byte |= BIT(0);
		ret = airoha_paged_bosa_write(bosa, reg, byte);
		if (ret)
			return ret;
		if (reg == 0xb7)
			usleep_range(10000, 11000);
		else
			usleep_range(1000, 1500);
	}

	/* The final 1-to-0 pulse on register 0x6e bit 6 occurs while the board
	 * TX_DISABLE GPIO remains asserted throughout calibration.
	 */
	ret = airoha_paged_bosa_read(bosa, AIROHA_BOSA_TX_CONTROL, &value, 1);
	if (ret)
		return ret;
	ret = airoha_paged_bosa_write(bosa, AIROHA_BOSA_TX_CONTROL,
	                              value | AIROHA_BOSA_TX_DISABLE);
	if (ret)
		return ret;
	usleep_range(10000, 11000);
	ret = airoha_paged_bosa_write(bosa, AIROHA_BOSA_TX_CONTROL,
	                              value & ~AIROHA_BOSA_TX_DISABLE);
	if (ret)
		return ret;
	usleep_range(10000, 11000);
	return 0;
}

const struct airoha_paged_bosa_chip airoha_bosa_ux3363 = {
	.name = "UX3363",
	.calibration_id = AIROHA_BOSA_CAL_UX3363,
	.supported_modes =
		AIROHA_PON_FRONTEND_MODE_BIT(AIROHA_PON_FRONTEND_MODE_XGPON) |
		AIROHA_PON_FRONTEND_MODE_BIT(
			AIROHA_PON_FRONTEND_MODE_EPON_10G_1G),
	.identify = ux3363_identify,
	.apply_calibration = ux3363_apply,
	.get_link_config = airoha_paged_bosa_prx30_link_config,
};
