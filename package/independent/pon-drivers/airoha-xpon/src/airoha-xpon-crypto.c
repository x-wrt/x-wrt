// SPDX-License-Identifier: GPL-2.0-only
#include <crypto/skcipher.h>
#include <linux/device.h>
#include <linux/iopoll.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "airoha-xpon-private.h"

int airoha_xpon_crypto_init(struct airoha_xpon *xpon)
{
	xpon->crypto.aes = crypto_alloc_sync_skcipher("ecb(aes)", 0, 0);
	if (IS_ERR(xpon->crypto.aes))
		return PTR_ERR(xpon->crypto.aes);
	return 0;
}

void airoha_xpon_crypto_cleanup(void *data)
{
	struct airoha_xpon *xpon = data;

	if (!IS_ERR_OR_NULL(xpon->crypto.aes))
		crypto_free_sync_skcipher(xpon->crypto.aes);
	xpon->crypto.aes = NULL;
}

static int airoha_xpon_key_generate(struct airoha_xpon *xpon, u32 type)
{
	u32 status;
	int ret;

	/* INT_STATUS[20] is a W1C completion bit owned by this KEY_GEN transaction. */
	airoha_xgpon_write(xpon, AIROHA_XGPON_INT_STATUS,
	                   AIROHA_XGPON_INT_KEY_CAL_DONE);
	airoha_xgpon_write(xpon, AIROHA_XGPON_KEY_GEN, type);
	xpon->crypto.generation_runs++;
	ret = readl_poll_timeout_atomic(
		xpon->xgpon_base + AIROHA_XGPON_INT_STATUS, status,
		status & AIROHA_XGPON_INT_KEY_CAL_DONE, 1, 100);
	if (ret) {
		xpon->crypto.generation_failed_type = type;
		return ret;
	}

	airoha_xgpon_write(xpon, AIROHA_XGPON_INT_STATUS,
	                   AIROHA_XGPON_INT_KEY_CAL_DONE);
	return 0;
}

static void airoha_xpon_copy_key_words(struct airoha_xpon *xpon, u32 source,
                                       u32 destination)
{
	unsigned int i;

	/* Key banks use the raw 32-bit MAC register representation. */
	for (i = 0; i < 4; i++)
		airoha_xgpon_write(xpon, destination + i * sizeof(u32),
		                   airoha_xgpon_read(xpon,
		                                     source + i * sizeof(u32)));
}

int airoha_xpon_program_activation_keys(struct airoha_xpon *xpon)
{
	static const u32 default_msk[4] = {
		0xb5d432eb,
		0x538bb1b4,
		0xe95e6ee3,
		0x2437be54,
	};
	unsigned int i;
	int ret;

	/* Group 1 holds default MSK/PIK; group 0 derives from Registration-ID
	 * and the PON Tag already installed by the profile handler.
	 */
	xpon->crypto.default_ploam_ik_programmed = false;
	xpon->crypto.activation_keys_programmed = false;
	xpon->crypto.o4_key_switch_observed = false;
	xpon->crypto.generation_failed_type = 0;

	for (i = 0; i < ARRAY_SIZE(default_msk); i++)
		airoha_xgpon_write(xpon, AIROHA_XGPON_MSK_0 + i * 4,
		                   default_msk[i]);

	ret = airoha_xpon_key_generate(xpon, AIROHA_XGPON_KEY_GEN_SK);
	if (ret)
		goto fail;
	ret = airoha_xpon_key_generate(xpon, AIROHA_XGPON_KEY_GEN_OMCI_IK);
	if (ret)
		goto fail;
	airoha_xpon_copy_key_words(xpon, AIROHA_XGPON_HW_GENK_0,
	                           AIROHA_XGPON_OIK1_0);

	for (i = 0; i < 4; i++)
		airoha_xgpon_write(xpon, AIROHA_XGPON_PIK1_0 + i * 4,
		                   0x55555555);
	xpon->crypto.default_ploam_ik_programmed = true;

	ret = airoha_xpon_key_generate(xpon, AIROHA_XGPON_KEY_GEN_KEK);
	if (ret)
		goto fail;
	airoha_xpon_copy_key_words(xpon, AIROHA_XGPON_HW_GENK_0,
	                           AIROHA_XGPON_KEK1_0);

	ret = airoha_xpon_key_generate(xpon, AIROHA_XGPON_KEY_GEN_REGMSK);
	if (ret)
		goto fail;
	airoha_xpon_copy_key_words(xpon, AIROHA_XGPON_REGMSK_0,
	                           AIROHA_XGPON_MSK_0);
	ret = airoha_xpon_key_generate(xpon, AIROHA_XGPON_KEY_GEN_SK);
	if (ret)
		goto fail;
	ret = airoha_xpon_key_generate(xpon, AIROHA_XGPON_KEY_GEN_OMCI_IK);
	if (ret)
		goto fail;
	airoha_xpon_copy_key_words(xpon, AIROHA_XGPON_HW_GENK_0,
	                           AIROHA_XGPON_OIK0_0);
	ret = airoha_xpon_key_generate(xpon, AIROHA_XGPON_KEY_GEN_PLOAM_IK);
	if (ret)
		goto fail;
	airoha_xpon_copy_key_words(xpon, AIROHA_XGPON_HW_GENK_0,
	                           AIROHA_XGPON_PIK0_0);
	ret = airoha_xpon_key_generate(xpon, AIROHA_XGPON_KEY_GEN_KEK);
	if (ret)
		goto fail;
	airoha_xpon_copy_key_words(xpon, AIROHA_XGPON_HW_GENK_0,
	                           AIROHA_XGPON_KEK0_0);

	/* O2/O3 use group 1; the MAC selects group 0 at O4. */
	airoha_xgpon_write(xpon, AIROHA_XGPON_SW_SET_KIDX,
	                   AIROHA_XGPON_SW_SET_PIK_ENABLE |
	                           AIROHA_XGPON_SW_SET_PIK_INDEX |
	                           AIROHA_XGPON_SW_SET_OIK_ENABLE |
	                           AIROHA_XGPON_SW_SET_OIK_INDEX);
	xpon->crypto.activation_pon_tag0 =
		airoha_xgpon_read(xpon, AIROHA_XGPON_PON_TAG_0);
	xpon->crypto.activation_pon_tag1 =
		airoha_xgpon_read(xpon, AIROHA_XGPON_PON_TAG_1);
	xpon->crypto.activation_keys_programmed = true;
	dev_info(
		xpon->dev,
		"XG-PON activation key groups generated: PON-Tag=%08x%08x, group1 active; hardware switches to group0 in O4\n",
		xpon->crypto.activation_pon_tag1,
		xpon->crypto.activation_pon_tag0);
	return 0;

fail:
	dev_err(xpon->dev,
	        "XG-PON activation key generation timed out: type=0x%x ret=%d; upstream remains disabled\n",
	        xpon->crypto.generation_failed_type, ret);
	return ret;
}

static void airoha_xpon_read_key_bytes(struct airoha_xpon *xpon, u32 base,
                                       u8 key[16])
{
	unsigned int i;

	/* key0..key3 hold four logical big-endian words in reverse register order. */
	for (i = 0; i < 4; i++)
		put_unaligned_be32(airoha_xgpon_read(xpon, base + (3 - i) * 4),
		                   key + i * 4);
}

static void airoha_xpon_write_key_bytes(struct airoha_xpon *xpon, u32 base,
                                        const u8 key[16])
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		airoha_xgpon_write(xpon, base + i * 4,
		                   get_unaligned_be32(key + (3 - i) * 4));
}

int airoha_xpon_install_omci_msk(struct airoha_xpon *xpon, const u8 key[16])
{
	u32 key_index, destination, switch_value;
	bool next_oik1, next_pik1;
	int ret;

	lockdep_assert_held(&xpon->state_lock);
	lockdep_assert_held(&xpon->ploam_lock);

	/*
	 * The OLT uses the current integrity key for PLOAM after OMCI
	 * authentication. Switch OIK when MSK derivation completes, leaving the
	 * new PIK/KEK in the alternate group until PLOAM key activation.
	 */
	key_index = airoha_xgpon_read(xpon, AIROHA_XGPON_CUR_KIDX);
	next_oik1 = !(key_index & AIROHA_XGPON_CUR_OIK_INDEX);
	next_pik1 = !(key_index & AIROHA_XGPON_CUR_PIK_INDEX);
	airoha_xpon_write_key_bytes(xpon, AIROHA_XGPON_MSK_0, key);

	ret = airoha_xpon_key_generate(xpon, AIROHA_XGPON_KEY_GEN_SK);
	if (ret)
		return ret;

	ret = airoha_xpon_key_generate(xpon, AIROHA_XGPON_KEY_GEN_OMCI_IK);
	if (ret)
		return ret;
	destination = next_oik1 ? AIROHA_XGPON_OIK1_0 : AIROHA_XGPON_OIK0_0;
	airoha_xpon_copy_key_words(xpon, AIROHA_XGPON_HW_GENK_0, destination);

	ret = airoha_xpon_key_generate(xpon, AIROHA_XGPON_KEY_GEN_PLOAM_IK);
	if (ret)
		return ret;
	destination = next_pik1 ? AIROHA_XGPON_PIK1_0 : AIROHA_XGPON_PIK0_0;
	airoha_xpon_copy_key_words(xpon, AIROHA_XGPON_HW_GENK_0, destination);

	ret = airoha_xpon_key_generate(xpon, AIROHA_XGPON_KEY_GEN_KEK);
	if (ret)
		return ret;
	destination = next_pik1 ? AIROHA_XGPON_KEK1_0 : AIROHA_XGPON_KEK0_0;
	airoha_xpon_copy_key_words(xpon, AIROHA_XGPON_HW_GENK_0, destination);

	/* SW_SET_KIDX holds independent key-domain selectors; preserve the PIK
	 * and data-key selectors when changing the OIK index.
	 */
	switch_value = airoha_xgpon_read(xpon, AIROHA_XGPON_SW_SET_KIDX);
	switch_value &= ~(AIROHA_XGPON_SW_SET_OIK_INDEX |
	                  AIROHA_XGPON_SW_SET_OIK_ENABLE);
	if (next_oik1)
		switch_value |= AIROHA_XGPON_SW_SET_OIK_INDEX;
	switch_value |= AIROHA_XGPON_SW_SET_OIK_ENABLE;
	airoha_xgpon_write(xpon, AIROHA_XGPON_SW_SET_KIDX, switch_value);

	/* Raw OMCI TX descriptors carry the OIK index selected for this epoch. */
	airoha_xpon_update_omci_link(xpon);
	dev_info(
		xpon->dev,
		"OMCI enhanced-security MSK installed; OIK switched to group%u\n",
		next_oik1);
	return 0;
}

int airoha_xpon_aes_encrypt_block(struct airoha_xpon *xpon, const u8 key[16],
                                  const u8 input[16], u8 output[16])
{
	SYNC_SKCIPHER_REQUEST_ON_STACK(request, xpon->crypto.aes);
	struct scatterlist block;
	int ret;

	/* The AES bounce block gives the synchronous skcipher a direct-mapped input
	 * when the caller uses VMAP_STACK.
	 */
	ret = crypto_sync_skcipher_setkey(xpon->crypto.aes, key, 16);
	if (ret)
		return ret;

	memcpy(xpon->crypto.aes_block, input, sizeof(xpon->crypto.aes_block));
	sg_init_one(&block, xpon->crypto.aes_block,
	            sizeof(xpon->crypto.aes_block));
	skcipher_request_set_sync_tfm(request, xpon->crypto.aes);
	skcipher_request_set_callback(request, 0, NULL, NULL);
	skcipher_request_set_crypt(request, &block, &block,
	                           sizeof(xpon->crypto.aes_block), NULL);
	ret = crypto_skcipher_encrypt(request);
	if (!ret)
		memcpy(output, xpon->crypto.aes_block,
		       sizeof(xpon->crypto.aes_block));
	memzero_explicit(xpon->crypto.aes_block,
	                 sizeof(xpon->crypto.aes_block));
	skcipher_request_zero(request);
	return ret;
}

static void airoha_xpon_cmac_double(u8 output[16], const u8 input[16])
{
	u8 carry = 0;
	int i;

	/* RFC 4493 shifts a GF(2^128) bit string starting at input[0]'s MSB. */
	for (i = 15; i >= 0; i--) {
		u8 next = !!(input[i] & BIT(7));

		output[i] = input[i] << 1 | carry;
		carry = next;
	}
	if (input[0] & BIT(7))
		output[15] ^= 0x87;
}

int airoha_xpon_aes_cmac_32(struct airoha_xpon *xpon, const u8 key[16],
                            const u8 message[32], u8 digest[16])
{
	u8 block[16] = {}, k1[16], l[16], x[16] = {};
	unsigned int i;
	int ret;

	/* Key Confirm CMAC covers the data key and a fixed block constant. */
	ret = airoha_xpon_aes_encrypt_block(xpon, key, block, l);
	if (ret)
		goto out;
	airoha_xpon_cmac_double(k1, l);

	for (i = 0; i < 16; i++)
		block[i] = x[i] ^ message[i];
	ret = airoha_xpon_aes_encrypt_block(xpon, key, block, x);
	if (ret)
		goto out;

	for (i = 0; i < 16; i++)
		block[i] = x[i] ^ message[16 + i] ^ k1[i];
	ret = airoha_xpon_aes_encrypt_block(xpon, key, block, digest);

out:
	memzero_explicit(block, sizeof(block));
	memzero_explicit(k1, sizeof(k1));
	memzero_explicit(l, sizeof(l));
	memzero_explicit(x, sizeof(x));
	return ret;
}

int airoha_xpon_get_active_kek(struct airoha_xpon *xpon, u8 kek[16])
{
	u32 key_index = airoha_xgpon_read(xpon, AIROHA_XGPON_CUR_KIDX);
	bool pik1 = !!(key_index & AIROHA_XGPON_CUR_PIK_INDEX);

	/*
	 * KEK and PIK share a hardware group selector for PLOAM Key_Control.
	 * OMCI authentication switches OIK earlier, so the PIK selector identifies
	 * the current KEK group.
	 */
	airoha_xpon_read_key_bytes(
		xpon, pik1 ? AIROHA_XGPON_KEK1_0 : AIROHA_XGPON_KEK0_0, kek);
	return 0;
}

int airoha_xpon_program_data_key(struct airoha_xpon *xpon, u8 index,
                                 const u8 key[16])
{
	u32 base;

	if (index == 1)
		base = AIROHA_XGPON_AES_UC_IDX0_KEY0;
	else if (index == 2)
		base = AIROHA_XGPON_AES_UC_IDX1_KEY0;
	else
		return -EINVAL;

	airoha_xpon_write_key_bytes(xpon, base, key);
	return 0;
}

int airoha_xpon_set_data_key_rx_valid(struct airoha_xpon *xpon, u8 index)
{
	u32 valid;

	if (index == 1)
		valid = AIROHA_XGPON_DS_AES_UC_IDX0_VALID;
	else if (index == 2)
		valid = AIROHA_XGPON_DS_AES_UC_IDX1_VALID;
	else
		return -EINVAL;

	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_DS_AES_KEY_VLD, valid,
	                         valid);
	return 0;
}

int airoha_xpon_activate_data_key(struct airoha_xpon *xpon, u8 index)
{
	u32 status, value;
	int ret;

	if (index != 1 && index != 2)
		return -EINVAL;

	/* Switch the upstream slot with US valid cleared. INT_STATUS[7] is a W1C
	 * completion bit outside INT_ENABLE.
	 */
	airoha_xgpon_write(xpon, AIROHA_XGPON_INT_STATUS,
	                   AIROHA_XGPON_INT_AES_KEY_SWITCH_DONE);
	value = airoha_xgpon_read(xpon, AIROHA_XGPON_US_AES_KEY_CTRL);
	value &= ~(AIROHA_XGPON_US_AES_KEY_VALID |
	           AIROHA_XGPON_US_AES_KEY_INDEX);
	if (index == 2)
		value |= AIROHA_XGPON_US_AES_KEY_INDEX;
	airoha_xgpon_write(xpon, AIROHA_XGPON_US_AES_KEY_CTRL, value);
	ret = readl_poll_timeout_atomic(
		xpon->xgpon_base + AIROHA_XGPON_INT_STATUS, status,
		status & AIROHA_XGPON_INT_AES_KEY_SWITCH_DONE, 1, 3000);
	if (!ret)
		airoha_xgpon_write(xpon, AIROHA_XGPON_INT_STATUS,
		                   AIROHA_XGPON_INT_AES_KEY_SWITCH_DONE);
	return 0;
}

void airoha_xpon_reset_data_key_exchange(struct airoha_xpon *xpon)
{
	unsigned int i;

	/* An invalid ONU-ID or EqD invalidates the active data key. */
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_US_AES_KEY_CTRL,
	                         AIROHA_XGPON_US_AES_KEY_VALID, 0);
	airoha_xgpon_update_bits(xpon, AIROHA_XGPON_DS_AES_KEY_VLD,
	                         AIROHA_XGPON_DS_AES_UC_VALID_MASK, 0);
	for (i = 0; i < 4; i++) {
		airoha_xgpon_write(xpon, AIROHA_XGPON_AES_UC_IDX0_KEY0 + i * 4,
		                   0);
		airoha_xgpon_write(xpon, AIROHA_XGPON_AES_UC_IDX1_KEY0 + i * 4,
		                   0);
	}
	memzero_explicit(xpon->crypto.data_keys,
	                 sizeof(xpon->crypto.data_keys));
	memzero_explicit(xpon->crypto.encrypted_data_key,
	                 sizeof(xpon->crypto.encrypted_data_key));
	memzero_explicit(xpon->crypto.key_confirm_digest,
	                 sizeof(xpon->crypto.key_confirm_digest));
	xpon->crypto.pending_data_key_index = 0;
	xpon->crypto.active_data_key_index = 0;
	xpon->crypto.exchange_state = AIROHA_XGPON_KEY_IDLE;
}
