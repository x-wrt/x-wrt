/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AIROHA_XPON_XGPON_H
#define __AIROHA_XPON_XGPON_H

#include <linux/interrupt.h>
#include <linux/types.h>

struct airoha_xpon;

int airoha_xpon_xgpon_prepare(struct airoha_xpon *xpon);
int airoha_xpon_xgpon_start_rx(struct airoha_xpon *xpon);
void airoha_xpon_xgpon_stop_rx(struct airoha_xpon *xpon);
void airoha_xpon_xgpon_mask_irqs(struct airoha_xpon *xpon);
bool airoha_xpon_xgpon_los(struct airoha_xpon *xpon);
irqreturn_t airoha_xpon_xgpon_irq(struct airoha_xpon *xpon);
irqreturn_t airoha_xpon_xgpon_phy_irq(struct airoha_xpon *xpon);
void airoha_xpon_xgpon_restart_mac(struct airoha_xpon *xpon,
                                   const char *reason);
void airoha_xpon_xgpon_advance(struct airoha_xpon *xpon, u32 phy_events,
                               unsigned long *delay);

#endif
