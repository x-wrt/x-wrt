// SPDX-License-Identifier: GPL-2.0-or-later
/* Host-only register mock, included around the production command functions. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((UINT32_MAX >> (31 - (h))) & (UINT32_MAX << (l)))
#define GENMASK_ULL(h, l) ((UINT64_MAX >> (63 - (h))) & (UINT64_MAX << (l)))
#define U64_MAX UINT64_MAX
#define FIELD_GET(mask, val) (((val) & (mask)) >> __builtin_ctz(mask))
#define FIELD_PREP(mask, val) (((val) << __builtin_ctz(mask)) & (mask))
#define lockdep_assert_held(lock) ((void)(lock))
#define udelay(us) ((void)(us))

struct regmap {
	u32 words[0x400000 / 4];
	u32 fail_read, fail_write, slot, host;
	u32 reads, writes, results, hi_reads;
	bool timeout, fail, carry, unstable;
};
struct ppe_data {
	u32 num_flow_entries, num_host_entries;
};
struct qca_ppe_priv {
	struct regmap *regmap;
	const struct ppe_data *data;
	int flow_lock;
	u32 flow_cmd_id, flow_op_errors;
	bool flow_faulted;
};

static int regmap_write(struct regmap *, u32, u32);
static int regmap_read(struct regmap *, u32, u32 *);
static int regmap_update_bits(struct regmap *, u32, u32, u32);

/* DRIVER_UNDER_TEST */

static struct regmap map;
static const struct ppe_data data = { 2048, 768 };
static struct qca_ppe_priv priv;

static int regmap_write(struct regmap *m, u32 reg, u32 value)
{
	u32 result = 0, cmd = 0;

	m->writes++;
	if (reg == m->fail_write)
		return -EIO;
	assert(reg / 4 < sizeof(m->words) / sizeof(m->words[0]));
	m->words[reg / 4] = value;
	if (reg == PPE_FLOW_TBL_OP || reg == PPE_FLOW_TBL_RD_OP) {
		result = reg == PPE_FLOW_TBL_OP ? PPE_FLOW_TBL_OP_RSLT :
			 PPE_FLOW_TBL_RD_OP_RSLT;
		cmd = FIELD_PREP(PPE_FLOW_RSLT_CMD_ID,
				 FIELD_GET(PPE_FLOW_OP_CMD_ID, value)) |
		      FIELD_PREP(PPE_FLOW_RSLT_VALID_CNT, 1) |
		      FIELD_PREP(PPE_FLOW_RSLT_ENTRY_IDX, m->slot) |
		      (m->fail ? PPE_FLOW_RSLT_FAIL : 0);
		m->words[PPE_IN_FLOW_TBL(m->slot) / 4] =
			m->host << PPE_FLOW_E_HOST_IDX_OFF;
	} else if (reg == PPE_HOST_TBL_OP) {
		result = PPE_HOST_TBL_OP_RSLT;
		cmd = FIELD_PREP(PPE_HOST_RSLT_CMD_ID,
				 FIELD_GET(PPE_HOST_OP_CMD_ID, value)) |
		      FIELD_PREP(PPE_HOST_RSLT_VALID_CNT, 1) |
		      (m->fail ? PPE_HOST_RSLT_FAIL : 0);
	}
	if (result && !m->timeout)
		m->words[result / 4] = cmd;
	return 0;
}

static int regmap_read(struct regmap *m, u32 reg, u32 *value)
{
	m->reads++;
	if (reg == m->fail_read)
		return -EIO;
	assert(reg / 4 < sizeof(m->words) / sizeof(m->words[0]));
	*value = m->words[reg / 4];
	if (reg == PPE_FLOW_TBL_OP_RSLT || reg == PPE_FLOW_TBL_RD_OP_RSLT ||
	    reg == PPE_HOST_TBL_OP_RSLT) {
		m->results++;
		m->words[reg / 4] = 0; /* FIFO result is consumed by this read. */
	}
	if (reg == PPE_IN_FLOW_CNT_TBL(5) + 8) {
		if (m->unstable)
			*value = m->hi_reads++;
		else if (m->carry)
			*value = m->hi_reads++ ? 1 : 0;
	}
	return 0;
}

static int regmap_update_bits(struct regmap *m, u32 reg, u32 mask, u32 value)
{
	return regmap_write(m, reg, (m->words[reg / 4] & ~mask) | value);
}

static void reset(void)
{
	memset(&map, 0, sizeof(map));
	map.fail_read = map.fail_write = UINT32_MAX;
	map.slot = 5;
	map.host = 21;
	map.words[PPE_FLOW_CTRL0 / 4] = PPE_FLOW_EN | 0x100;
	priv = (struct qca_ppe_priv){ .regmap = &map, .data = &data };
}

static int add(u32 *slot, u32 *host)
{
	u32 flow[PPE_FLOW_ENTRY_WORDS_V6] = { 1, 2, 3, 4, 5 };
	u32 key[PPE_HOST_ENTRY_WORDS_V6] = { 1, 2, 3 };

	return ppe_flow_op(&priv, PPE_TBL_OP_ADD, flow, 5, key, 3, slot, host);
}

static void faulted(void)
{
	u32 writes = map.writes, reads = map.reads;

	assert(priv.flow_faulted && priv.flow_op_errors == 1);
	assert(map.words[PPE_FLOW_CTRL0 / 4] == 0x100);
	assert(add(NULL, NULL) == -EIO);
	assert(ppe_flow_entry_delete(&priv, 5) == -EIO);
	assert(map.writes == writes && map.reads == reads);
}

int main(void)
{
	u32 slot, host, words[9], writes;
	u64 packets, bytes;

	reset();
	assert(!add(&slot, &host) && slot == 5 && host == 21);
	assert(map.results == 1); /* A second read would have lost the index. */
	assert(map.words[PPE_FLOW_TBL_OP / 4] & PPE_FLOW_OP_HOST_EN);
	assert(!(map.words[PPE_FLOW_TBL_OP / 4] & PPE_FLOW_OP_INDEX_MODE));
	assert(FIELD_GET(PPE_FLOW_OP_HASH_BLOCK,
			 map.words[PPE_FLOW_TBL_OP / 4]) == 3);
	priv.flow_cmd_id = 15;
	assert(!ppe_flow_entry_delete(&priv, 5) && priv.flow_cmd_id == 0);
	assert(map.words[PPE_FLOW_TBL_OP / 4] & PPE_FLOW_OP_INDEX_MODE);
	map.words[PPE_FLOW_TBL_RD_RSLT_DATA(8) / 4] = 0xfeedface;
	assert(!ppe_flow_entry_read(&priv, 5, words, 9));
	assert(words[8] == 0xfeedface);
	assert(!ppe_host_del(&priv, 21));

	reset();
	map.fail = true;
	assert(add(NULL, NULL) == -ENOENT && !priv.flow_faulted);
	assert(ppe_flow_entry_read(&priv, 5, words, 9) == -ENOENT);
	assert(ppe_host_del(&priv, 21) == -ENOENT);

	reset();
	assert(ppe_flow_entry_delete(&priv, 2048) == -EINVAL);
	assert(ppe_host_del(&priv, 768) == -EINVAL);
	assert(ppe_flow_entry_read(&priv, 5, words, 10) == -EINVAL);
	assert(ppe_flow_counter_clear(&priv, 2048) == -EINVAL);
	assert(!map.reads && !map.writes);

	reset(); map.timeout = true;
	assert(add(NULL, NULL) == -ETIMEDOUT); faulted();
	reset(); map.timeout = true;
	assert(ppe_flow_entry_delete(&priv, 5) == -ETIMEDOUT); faulted();
	reset(); map.timeout = true;
	assert(ppe_host_del(&priv, 21) == -ETIMEDOUT); faulted();
	reset(); map.fail_read = PPE_FLOW_TBL_OP_RSLT;
	assert(add(NULL, NULL) == -EIO); faulted();
	reset(); map.fail_write = PPE_FLOW_TBL_OP;
	assert(add(NULL, NULL) == -EIO); faulted();
	reset(); map.fail_read = PPE_FLOW_TBL_RD_RSLT_DATA(3);
	assert(ppe_flow_entry_read(&priv, 5, words, 9) == -EIO); faulted();
	reset(); map.slot = 2048;
	assert(add(&slot, &host) == -EIO); faulted();
	reset(); map.host = 768;
	assert(add(&slot, &host) == -EIO); faulted();

	reset();
	map.words[PPE_IN_FLOW_CNT_TBL(5) / 4] = UINT32_MAX;
	map.words[(PPE_IN_FLOW_CNT_TBL(5) + 4) / 4] = 7;
	map.carry = true;
	assert(!ppe_flow_counter_read(&priv, 5, &packets, &bytes));
	assert(packets == UINT32_MAX && bytes == (1ULL << 32) + 7);
	assert(map.hi_reads == 3);
	map.unstable = true; map.hi_reads = 0;
	assert(ppe_flow_counter_read(&priv, 5, &packets, &bytes) == -EAGAIN);
	assert(packets == UINT32_MAX && bytes == (1ULL << 32) + 7);
	assert(!priv.flow_faulted && map.hi_reads == 5);
	writes = map.writes;
	assert(!ppe_flow_counter_clear(&priv, 5));
	assert(map.writes - writes == PPE_FLOW_CNT_WORDS);
	reset(); map.fail_write = PPE_IN_FLOW_CNT_TBL(5) + 4;
	assert(ppe_flow_counter_clear(&priv, 5) == -EIO); faulted();
	reset(); map.fail_read = PPE_IN_FLOW_CNT_TBL(5);
	packets = bytes = 99;
	assert(ppe_flow_counter_read(&priv, 5, &packets, &bytes) == -EIO);
	assert(packets == 99 && bytes == 99); faulted();

	puts("PPE flow engine: consuming results, command IDs, bounds, failures, timeouts and counters PASS");
	return 0;
}
