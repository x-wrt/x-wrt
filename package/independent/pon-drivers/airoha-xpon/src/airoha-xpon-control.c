// SPDX-License-Identifier: GPL-2.0-only
#include <linux/ctype.h>
#include <linux/airoha-eth.h>
#include <linux/device.h>
#include <linux/hex.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "airoha-xpon-private.h"
#include "airoha-xpon-epon.h"

static size_t airoha_xpon_trimmed_length(const char *buf, size_t count)
{
	while (count && isspace(buf[count - 1]))
		count--;
	return count;
}

static ssize_t serial_number_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
	struct airoha_xpon *xpon = dev_get_drvdata(dev);
	struct airoha_xpon_identity pending;

	mutex_lock(&xpon->state_lock);
	pending = xpon->identity.pending_identity;
	mutex_unlock(&xpon->state_lock);

	if (!pending.serial_configured)
		return sysfs_emit(buf, "none\n");
	return sysfs_emit(buf, "%*phN\n", 8, pending.serial_number);
}

static ssize_t serial_number_store(struct device *dev,
                                   struct device_attribute *attr,
                                   const char *buf, size_t count)
{
	struct airoha_xpon *xpon = dev_get_drvdata(dev);
	struct airoha_xpon_identity next;
	char value[17];
	size_t len = airoha_xpon_trimmed_length(buf, count);

	mutex_lock(&xpon->state_lock);
	next = xpon->identity.pending_identity;
	if (sysfs_streq(buf, "default")) {
		next.serial_configured =
			xpon->identity.default_identity.serial_configured;
		next.serial_source =
			xpon->identity.default_identity.serial_source;
		memcpy(next.serial_number,
		       xpon->identity.default_identity.serial_number,
		       sizeof(next.serial_number));
	} else if (sysfs_streq(buf, "none")) {
		memset(next.serial_number, 0, sizeof(next.serial_number));
		next.serial_configured = false;
		next.serial_source = "none";
	} else {
		if (len != 16) {
			mutex_unlock(&xpon->state_lock);
			return -EINVAL;
		}
		memcpy(value, buf, len);
		value[len] = '\0';
		if (hex2bin(next.serial_number, value,
		            sizeof(next.serial_number))) {
			mutex_unlock(&xpon->state_lock);
			return -EINVAL;
		}
		next.serial_configured = true;
		next.serial_source = "sysfs-override";
	}
	xpon->identity.pending_identity = next;
	mutex_unlock(&xpon->state_lock);

	/* The active identity belongs to the current ndo_open epoch. */
	return count;
}
/* Mode 0600 protects both identity changes and the latched credentials. */
static DEVICE_ATTR(serial_number, 0600, serial_number_show,
                   serial_number_store);

static ssize_t registration_id_show(struct device *dev,
                                    struct device_attribute *attr, char *buf)
{
	struct airoha_xpon *xpon = dev_get_drvdata(dev);
	struct airoha_xpon_identity pending;

	mutex_lock(&xpon->state_lock);
	pending = xpon->identity.pending_identity;
	mutex_unlock(&xpon->state_lock);

	if (!pending.registration_id_configured)
		return sysfs_emit(buf, "default-zero\n");
	return sysfs_emit(buf, "%*phN\n", 36, pending.registration_id);
}

static ssize_t registration_id_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count)
{
	struct airoha_xpon *xpon = dev_get_drvdata(dev);
	struct airoha_xpon_identity next;
	char value[73];
	size_t len = airoha_xpon_trimmed_length(buf, count);

	mutex_lock(&xpon->state_lock);
	next = xpon->identity.pending_identity;
	if (sysfs_streq(buf, "default")) {
		next.registration_id_configured =
			xpon->identity.default_identity
				.registration_id_configured;
		memcpy(next.registration_id,
		       xpon->identity.default_identity.registration_id,
		       sizeof(next.registration_id));
	} else if (sysfs_streq(buf, "none")) {
		memset(next.registration_id, 0, sizeof(next.registration_id));
		next.registration_id_configured = false;
	} else {
		if (len != 72) {
			mutex_unlock(&xpon->state_lock);
			return -EINVAL;
		}
		memcpy(value, buf, len);
		value[len] = '\0';
		if (hex2bin(next.registration_id, value,
		            sizeof(next.registration_id))) {
			mutex_unlock(&xpon->state_lock);
			return -EINVAL;
		}
		next.registration_id_configured = true;
	}
	xpon->identity.pending_identity = next;
	mutex_unlock(&xpon->state_lock);

	return count;
}
static DEVICE_ATTR(registration_id, 0600, registration_id_show,
                   registration_id_store);

static ssize_t active_serial_number_show(struct device *dev,
                                         struct device_attribute *attr,
                                         char *buf)
{
	struct airoha_xpon *xpon = dev_get_drvdata(dev);
	struct airoha_xpon_identity active;

	/*
	 * PLOAM and userspace authentication must observe the identity latched
	 * by the same ndo_open epoch.
	 */
	mutex_lock(&xpon->state_lock);
	active = xpon->identity.active_identity;
	mutex_unlock(&xpon->state_lock);
	if (!active.serial_configured)
		return sysfs_emit(buf, "none\n");
	return sysfs_emit(buf, "%*phN\n", 8, active.serial_number);
}
static DEVICE_ATTR_RO(active_serial_number);

static ssize_t omci_msk_store(struct device *dev, struct device_attribute *attr,
                              const char *buf, size_t count)
{
	struct airoha_xpon *xpon = dev_get_drvdata(dev);
	u8 key[16];
	char value[33];
	size_t len = airoha_xpon_trimmed_length(buf, count);
	int ret;

	if (len != 32)
		return -EINVAL;
	memcpy(value, buf, len);
	value[len] = '\0';
	if (hex2bin(key, value, sizeof(key)))
		return -EINVAL;

	/*
	 * KEY_GEN and PLOAM data-key exchange share a hardware engine. Preserve
	 * state_lock -> ploam_lock ordering during O5 Class 332 derivation.
	 */
	mutex_lock(&xpon->state_lock);
	if (!xpon->active_mode_valid ||
	    airoha_xpon_mode_is_epon(xpon->active_mode) ||
	    xpon->onu_state != AIROHA_XGPON_O5) {
		ret = -ENOLINK;
		goto out_state;
	}
	mutex_lock(&xpon->ploam_lock);
	ret = airoha_xpon_install_omci_msk(xpon, key);
	mutex_unlock(&xpon->ploam_lock);

out_state:
	mutex_unlock(&xpon->state_lock);
	memzero_explicit(key, sizeof(key));
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(omci_msk);

static ssize_t data_path_show(struct device *dev, struct device_attribute *attr,
                              char *buf)
{
	struct airoha_xpon *xpon = dev_get_drvdata(dev);
	ssize_t len;
	unsigned int i;

	len = sysfs_emit(buf, "configured=%d count=%u pending=%u",
	                 READ_ONCE(xpon->data_path.configured),
	                 READ_ONCE(xpon->data_path.count),
	                 READ_ONCE(xpon->data_path.pending_count));
	for (i = 0; i < READ_ONCE(xpon->data_path.count); i++) {
		const struct airoha_xpon_data_path_entry *path =
			&xpon->data_path.entries[i];

		len += sysfs_emit_at(buf, len, " path=%u:%u:%u:%02x:%u",
		                     READ_ONCE(path->alloc_id),
		                     READ_ONCE(path->gem_id),
		                     READ_ONCE(path->vlan_id),
		                     READ_ONCE(path->pbit_mask),
		                     READ_ONCE(path->multicast));
	}
	return len + sysfs_emit_at(buf, len, "\n");
}

static ssize_t data_path_store(struct device *dev,
                               struct device_attribute *attr, const char *buf,
                               size_t count)
{
	struct airoha_xpon *xpon = dev_get_drvdata(dev);
	struct airoha_xpon_data_path_entry paths[AIROHA_XPON_MAX_DATA_PATHS];
	char *input, *cursor, *token;
	unsigned int alloc_id, gem_id, vlan_id;
	unsigned int pbit_mask, multicast;
	unsigned int path_count = 0;
	int fields, parsed_len, ret;

	/*
	 * PLOAM/data-path code owns O5 gating and hardware submission under
	 * state_lock -> ploam_lock.
	 */
	if (sysfs_streq(buf, "clear")) {
		ret = airoha_xpon_clear_data_path(xpon);
		return ret ? ret : count;
	}
	if (str_has_prefix(buf, "replace ")) {
		input = kstrdup(buf + strlen("replace "), GFP_KERNEL);
		if (!input)
			return -ENOMEM;
		cursor = input;
		while ((token = strsep(&cursor, " \t\n"))) {
			if (!*token)
				continue;
			multicast = 0;
			parsed_len = 0;
			fields = sscanf(token, "%u:%u:%u:%x:%u%n", &alloc_id,
			                &gem_id, &vlan_id, &pbit_mask,
			                &multicast, &parsed_len);
			if (fields != 5 || token[parsed_len]) {
				/* Older ponctl releases use a four-field plain GEM format. */
				multicast = 0;
				parsed_len = 0;
				fields = sscanf(token, "%u:%u:%u:%x%n",
				                &alloc_id, &gem_id, &vlan_id,
				                &pbit_mask, &parsed_len);
			}
			if (path_count == AIROHA_XPON_MAX_DATA_PATHS ||
			    (fields != 4 && fields != 5) || token[parsed_len] ||
			    multicast > 1 ||
			    (!multicast && alloc_id > 0x3fff) ||
			    (multicast && alloc_id != 0xffff) ||
			    gem_id > 0xffff ||
			    (vlan_id > 4094 && vlan_id != AIROHA_PON_VID_ANY) ||
			    !pbit_mask || pbit_mask > 0xff) {
				kfree(input);
				return -EINVAL;
			}
			paths[path_count].alloc_id = alloc_id;
			paths[path_count].gem_id = gem_id;
			paths[path_count].vlan_id = vlan_id;
			paths[path_count].pbit_mask = pbit_mask;
			paths[path_count].multicast = multicast;
			path_count++;
		}
		kfree(input);
		ret = airoha_xpon_replace_data_paths(xpon, paths, path_count);
		return ret ? ret : count;
	}
	if (sscanf(buf, "%u %u", &alloc_id, &gem_id) != 2 ||
	    alloc_id > 0x3fff || gem_id > 0xffff)
		return -EINVAL;

	ret = airoha_xpon_configure_data_path(xpon, alloc_id, gem_id);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(data_path);

static ssize_t service_ready_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
	struct airoha_xpon *xpon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", READ_ONCE(xpon->service_ready));
}

static ssize_t service_ready_store(struct device *dev,
                                   struct device_attribute *attr,
                                   const char *buf, size_t count)
{
	struct airoha_xpon *xpon = dev_get_drvdata(dev);
	bool ready;
	int ret;

	ret = kstrtobool(buf, &ready);
	if (ret)
		return ret;

	mutex_lock(&xpon->state_lock);
	if (ready && (!xpon->active_mode_valid || !xpon->optical_signal)) {
		ret = -ENOLINK;
	} else if (ready && airoha_xpon_mode_is_epon(xpon->active_mode) &&
	           xpon->epon.mpcp_state != AIROHA_EPON_MPCP_REGISTERED) {
		ret = -ENOLINK;
	} else if (ready && !airoha_xpon_mode_is_epon(xpon->active_mode) &&
	           (xpon->onu_state != AIROHA_XGPON_O5 ||
	            !xpon->data_path.configured)) {
		ret = -ENOLINK;
	} else {
		xpon->service_ready = ready;
		airoha_xpon_leds_update(xpon);
		ret = 0;
	}
	mutex_unlock(&xpon->state_lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(service_ready);

static struct attribute *airoha_xpon_attrs[] = {
	&dev_attr_serial_number.attr,
	&dev_attr_active_serial_number.attr,
	&dev_attr_registration_id.attr,
	&dev_attr_omci_msk.attr,
	&dev_attr_data_path.attr,
	&dev_attr_service_ready.attr,
	NULL,
};
static const struct attribute_group airoha_xpon_group = {
	.attrs = airoha_xpon_attrs,
};

const struct attribute_group *airoha_xpon_groups[] = {
	&airoha_xpon_group,
	NULL,
};
