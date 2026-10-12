/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_EPON_H_
#define _AIROHA_XPON_EPON_H_

#include <linux/interrupt.h>
#include <linux/types.h>

#include "airoha-xpon-netlink.h"

struct airoha_xpon;
struct seq_file;

bool airoha_xpon_mode_is_epon(enum airoha_xpon_mode mode);
int airoha_xpon_epon_start_rx(struct airoha_xpon *xpon);
void airoha_xpon_epon_stop_rx(struct airoha_xpon *xpon);
void airoha_xpon_epon_mask_irqs(struct airoha_xpon *xpon);
bool airoha_xpon_epon_los(struct airoha_xpon *xpon);
bool airoha_xpon_epon_pcs_synced(struct airoha_xpon *xpon);
void airoha_xpon_epon_debug_pcs(struct airoha_xpon *xpon, struct seq_file *m);
void airoha_xpon_epon_debug_mac(struct airoha_xpon *xpon, struct seq_file *m);
int airoha_xpon_epon_advance(struct airoha_xpon *xpon, u32 phy_events,
                             unsigned long *delay);
irqreturn_t airoha_xpon_epon_irq_fast(struct airoha_xpon *xpon);
irqreturn_t airoha_xpon_epon_irq(struct airoha_xpon *xpon);
irqreturn_t airoha_xpon_epon_phy_irq(struct airoha_xpon *xpon);
#endif
