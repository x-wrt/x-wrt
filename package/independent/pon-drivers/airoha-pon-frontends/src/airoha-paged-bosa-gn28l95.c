// SPDX-License-Identifier: GPL-2.0-only
#include "airoha-paged-bosa.h"

#include <linux/delay.h>

#define GN_CALIBRATION_LENGTH 0x780

static int gn28l95_identify(struct airoha_paged_bosa *bosa, char *revision,
                            size_t revision_size)
{
	u8 id;
	int ret, restore;

	ret = airoha_paged_bosa_select_page(bosa, 0xff);
	if (!ret)
		ret = airoha_paged_bosa_read(bosa, 0x80, &id, 1);
	restore = airoha_paged_bosa_select_page(bosa, 0);
	if (!ret)
		ret = restore;
	if (ret)
		return ret;
	if (id != 0xa1)
		return 0;
	snprintf(revision, revision_size, "0x%02x", id);
	return 1;
}

static int gn28l95_wait_ready(struct airoha_paged_bosa *bosa)
{
	unsigned int retry;
	u8 status;
	int ret;

	ret = airoha_paged_bosa_select_page(bosa, 2);
	if (ret)
		return ret;
	for (retry = 0; retry < 1000; retry++) {
		ret = airoha_paged_bosa_read(bosa, 0xc7, &status, 1);
		if (ret)
			return ret;
		if (status & BIT(1))
			return 0;
		usleep_range(10000, 11000);
	}
	return -ETIMEDOUT;
}

static int gn28l95_apply(struct airoha_paged_bosa *bosa, const u8 *data,
                         size_t length)
{
	int ret;

	if (length != GN_CALIBRATION_LENGTH)
		return -EINVAL;
	ret = gn28l95_wait_ready(bosa);
	if (ret)
		return ret;

	/* The payload follows the chip's native 0x780-byte register image. */
	ret = airoha_paged_bosa_write_range(bosa, 4, 0x80, data + 0x280, 0x80,
	                                    0);
	if (ret)
		return ret;
	ret = airoha_paged_bosa_write_range(bosa, 5, 0x80, data + 0x300, 0x80,
	                                    0);
	if (ret)
		return ret;
	ret = airoha_paged_bosa_write_range(bosa, 6, 0x80, data + 0x380, 0x80,
	                                    0);
	if (ret)
		return ret;
	ret = airoha_paged_bosa_write_range(bosa, 2, 0x80, data + 0x200, 8, 0);
	if (ret)
		return ret;
	ret = airoha_paged_bosa_write_range(bosa, 2, 0x89, data + 0x209, 7, 0);
	if (ret)
		return ret;
	ret = airoha_paged_bosa_write_range(bosa, 2, 0x90, data + 0x210, 0x70,
	                                    0);
	if (ret)
		return ret;
	ret = airoha_paged_bosa_write_range(bosa, 0, 0x00, data + 0x100, 0x60,
	                                    0);
	if (ret)
		return ret;
	ret = airoha_paged_bosa_write_range(bosa, 0, 0x6f, data + 0x16f, 1, 0);
	if (ret)
		return ret;
	ret = airoha_paged_bosa_write_range(bosa, 0, 0x72, data + 0x172, 2, 0);
	if (ret)
		return ret;
	ret = airoha_paged_bosa_write_range(bosa, 0, 0x78, data + 0x178, 2, 0);
	if (ret)
		return ret;
	ret = airoha_paged_bosa_select_page(bosa, 2);
	if (ret)
		return ret;
	return airoha_paged_bosa_write(bosa, 0x88, 0x6a);
}

const struct airoha_paged_bosa_chip airoha_bosa_gn28l95 = {
	.name = "GN28L95",
	.calibration_id = AIROHA_BOSA_CAL_GN28L95,
	.supported_modes =
		AIROHA_PON_FRONTEND_MODE_BIT(AIROHA_PON_FRONTEND_MODE_XGPON) |
		AIROHA_PON_FRONTEND_MODE_BIT(
			AIROHA_PON_FRONTEND_MODE_EPON_10G_1G),
	.identify = gn28l95_identify,
	.apply_calibration = gn28l95_apply,
	.get_link_config = airoha_paged_bosa_prx30_link_config,
};
