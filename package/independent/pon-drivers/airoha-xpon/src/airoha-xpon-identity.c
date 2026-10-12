// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ctype.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "airoha-xpon-private.h"

static char *serial_number;
module_param(serial_number, charp, 0400);
MODULE_PARM_DESC(serial_number,
                 "XG-PON ONU serial number, exactly 16 hex digits");

static char *registration_id;
module_param(registration_id, charp, 0400);
MODULE_PARM_DESC(registration_id,
                 "XG-PON Registration-ID, exactly 72 hex digits");

static bool airoha_xpon_identity_is_blank(const u8 *data, size_t len)
{
	bool all_zero = true;
	bool all_ff = true;
	size_t i;

	for (i = 0; i < len; i++) {
		all_zero &= data[i] == 0x00;
		all_ff &= data[i] == 0xff;
	}

	return all_zero || all_ff;
}

int airoha_xpon_identity_init(struct device *dev, struct airoha_xpon *xpon)
{
	struct airoha_xpon_identity *identity_default =
		&xpon->identity.default_identity;
	struct nvmem_cell *cell;
	void *identity;
	size_t identity_len;
	int ret;

	identity_default->serial_source = "none";
	if (serial_number && *serial_number) {
		if (strlen(serial_number) !=
		            sizeof(identity_default->serial_number) * 2 ||
		    hex2bin(identity_default->serial_number, serial_number,
		            sizeof(identity_default->serial_number)))
			return dev_err_probe(
				dev, -EINVAL,
				"serial_number must contain exactly 16 hexadecimal characters\n");
		identity_default->serial_configured = true;
		identity_default->serial_source = "module-parameter";
	} else {
		/* The board NVMEM cell maps the stored PON serial number. */
		cell = devm_nvmem_cell_get(dev, "pon-serial");
		if (IS_ERR(cell)) {
			ret = PTR_ERR(cell);
			if (ret == -EPROBE_DEFER)
				return ret;
			if (ret != -ENOENT)
				dev_warn(
					dev,
					"default PON serial is unavailable: %pe\n",
					ERR_PTR(ret));
		} else {
			identity = nvmem_cell_read(cell, &identity_len);
			if (IS_ERR(identity)) {
				dev_warn(
					dev,
					"failed to read the default PON serial: %pe\n",
					identity);
				goto registration;
			}
			if (airoha_xpon_identity_is_blank(identity,
			                                  identity_len))
				goto invalid_nvmem;

			/* A 12-byte serial stores its four-byte VSSN as eight ASCII hex digits. */
			if (identity_len ==
			    sizeof(identity_default->serial_number)) {
				memcpy(identity_default->serial_number,
				       identity, identity_len);
			} else if (identity_len == 12) {
				const u8 *display = identity;
				size_t i;

				for (i = 0; i < 4; i++) {
					if (!isprint(display[i]))
						goto invalid_nvmem;
				}
				for (i = 4; i < 12; i++) {
					if (!isxdigit(display[i]))
						goto invalid_nvmem;
				}
				memcpy(identity_default->serial_number, display,
				       4);
				if (hex2bin(identity_default->serial_number + 4,
				            display + 4, 4))
					goto invalid_nvmem;
			} else {
				goto invalid_nvmem;
			}
			kfree(identity);
			identity_default->serial_configured = true;
			identity_default->serial_source = "nvmem";
			goto registration;

invalid_nvmem:
			dev_warn(
				dev,
				"ignoring invalid default PON serial (%zu bytes)\n",
				identity_len);
			kfree(identity);
		}
	}

registration:
	if (registration_id && *registration_id) {
		if (strlen(registration_id) !=
		            sizeof(identity_default->registration_id) * 2 ||
		    hex2bin(identity_default->registration_id, registration_id,
		            sizeof(identity_default->registration_id)))
			return dev_err_probe(
				dev, -EINVAL,
				"registration_id must contain exactly 72 hexadecimal characters\n");
		identity_default->registration_id_configured = true;
	}

	/* ndo_open latches the pending identity for the active line epoch. */
	xpon->identity.pending_identity = *identity_default;
	return 0;
}
