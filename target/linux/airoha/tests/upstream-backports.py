#!/usr/bin/env python3
"""Run host regressions against functions extracted from a prepared kernel.

Usage: python3 upstream-backports.py /path/to/prepared/linux-6.18.60
Requires a host C compiler. This exercises error paths and link mode masks;
it does not emulate PHY analogue calibration or kernel RCU scheduling.
"""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def function(source, name):
    match = re.search(r'^static (?:inline )?(?:int|void) ' + name + r'\(', source, re.M)
    if not match:
        raise RuntimeError(f'Cannot find {name}')
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


kernel = Path(sys.argv[1])
phy = (kernel / 'drivers/net/phy/mediatek/airoha-ge-soc.c').read_text()
pcs = (kernel / 'drivers/net/phy/phylink.c').read_text()
header = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint16_t u16;
typedef uint32_t u32;
#define MDIO_MMD_VEND1 1
#define MTK_PHY_RG_AD_CALIN 10
#define MTK_PHY_DA_CALIN_FLAG 1
#define MTK_PHY_RG_AD_CAL_CLK 11
#define MTK_PHY_DA_CAL_CLK 1
#define MTK_PHY_RG_AD_CAL_COMP 12
#define MTK_PHY_AD_CAL_COMP_OUT_MASK 1
#define ANALOG_INTERNAL_OPERATION_MAX_US 20
#define FIELD_GET(mask, v) ((v) & (mask))
#define AIROHA_GPHY_ID_AN7581 1
#define AIROHA_GPHY_ID_AN7583 2
#define AIROHA_DEFAULT_PORT0_ADDR 9
#define CONFIG_AIROHA_GE_SOC_PHY_CALIBRATION calibration
#define IS_ENABLED(x) (x)
#define MII_BMCR 0
#define BMCR_PDOWN 0x800
struct phy_driver { int phy_id; };
struct phy_device {
    int phy_id;
    struct phy_driver *drv;
    struct { void *bus; } mdio;
};
static int regs[4][32], bus_lock, package_lock, calibration;
static int operation, fail_at, poll_error, power_clears, calibrated, cal_result;
static int clear_attempts[4];
static struct phy_device *port0;
static void phy_lock_mdio_bus(struct phy_device *p) { assert(!bus_lock++); }
static void phy_unlock_mdio_bus(struct phy_device *p) { assert(bus_lock-- == 1); }
static void phy_package_lock(struct phy_device *p) { assert(!package_lock++); }
static void phy_package_unlock(struct phy_device *p) { assert(package_lock-- == 1); }
static int __phy_package_read_mmd(struct phy_device *p, unsigned int off, int devad, u32 r)
{
    assert(bus_lock == 1 && off < 4 && r < 32);
    if (++operation == fail_at) return -EIO;
    if (r == MTK_PHY_RG_AD_CAL_CLK && poll_error) return poll_error;
    return regs[off][r];
}
static int __phy_package_write_mmd(struct phy_device *p, unsigned int off, int devad, u32 r, u16 v)
{
    assert(bus_lock == 1 && off < 4 && r < 32);
    if (++operation == fail_at) return -EIO;
    regs[off][r] = v;
    return 0;
}
static int __phy_package_modify_mmd(struct phy_device *p, unsigned int off, int devad, u32 r, u16 m, u16 v)
{
    assert(bus_lock == 1 && off < 4 && r < 32);
    if (r == MTK_PHY_RG_AD_CALIN && !v) clear_attempts[off]++;
    if (++operation == fail_at) return -EIO;
    regs[off][r] = (regs[off][r] & ~m) | v;
    return 0;
}
/* Bounded completion polling with the same error/condition contract. */
#define read_poll_timeout(op, val, cond, sleep_us, timeout_us, delay, ...) \
({ int rc = -ETIMEDOUT; for (int i = 0; i < 3; i++) { \
    val = op(__VA_ARGS__); if (cond) { rc = 0; break; } } rc; })
static struct phy_device *mdiobus_get_phy(void *bus, int addr) { assert(addr == 9); return port0; }
static int phy_clear_bits(struct phy_device *p, int r, int m) { power_clears++; return 0; }
static int an7581_phy_config_init_calibrated(struct phy_device *p)
{ assert(package_lock == 1 && !bus_lock); calibrated++; return cal_result; }
static int an7583_phy_config_init_calibrated(struct phy_device *p)
{ return an7581_phy_config_init_calibrated(p); }
static void reset(void)
{
    assert(!bus_lock && !package_lock);
    memset(regs, 0, sizeof(regs));
    memset(clear_attempts, 0, sizeof(clear_attempts));
    regs[0][MTK_PHY_RG_AD_CAL_CLK] = MTK_PHY_DA_CAL_CLK;
    operation = fail_at = poll_error = power_clears = calibrated = cal_result = 0;
}
'''
body = ''.join(function(phy, name) for name in (
    'airoha_package_read_mmd', 'airoha_package_write_mmd',
    'airoha_package_modify_mmd', 'airoha_cal_cycle', 'airoha_phy_config_init'))
tests = r'''
int main(void)
{
    struct phy_driver drv = {AIROHA_GPHY_ID_AN7581};
    struct phy_device p = {.phy_id = AIROHA_GPHY_ID_AN7581, .drv = &drv};
    for (int off = 0; off <= 3; off++) {
        for (int result = 0; result < 2; result++) {
            reset(); regs[0][MTK_PHY_RG_AD_CAL_COMP] = result;
            assert(airoha_cal_cycle(&p, off, 1, 5, 63, 32) == result);
            assert(!regs[0][10] && !regs[off][10] && !bus_lock);
        }
        reset(); regs[0][11] = 0;
        assert(airoha_cal_cycle(&p, off, 1, 5, 63, 32) == -ETIMEDOUT);
        assert(!regs[0][10] && !regs[off][10] && !bus_lock);
        reset(); poll_error = -ENXIO;
        assert(airoha_cal_cycle(&p, off, 1, 5, 63, 32) == -ENXIO);
        assert(!regs[0][10] && !regs[off][10] && !bus_lock);
        for (int fault = 1; fault <= (off ? 7 : 6); fault++) {
            reset(); fail_at = fault;
            assert(airoha_cal_cycle(&p, off, 1, 5, 63, 32) == -EIO);
            assert(!bus_lock);
            if (fault > 1) {
                assert(clear_attempts[0]);
                if (off) assert(clear_attempts[off]);
            }
        }
    }
    reset(); assert(!airoha_package_write_mmd(&p, 2, 1, 6, 7));
    assert(regs[2][6] == 7 && !bus_lock);
    reset(); calibration = 0; port0 = NULL;
    assert(!airoha_phy_config_init(&p) && !calibrated && !power_clears);
    drv.phy_id = p.phy_id = AIROHA_GPHY_ID_AN7583;
    assert(!airoha_phy_config_init(&p) && power_clears == 1 && !calibrated);
    calibration = 1;
    assert(airoha_phy_config_init(&p) == -ENODEV && !package_lock);
    port0 = &p;
    for (int id = 1; id <= 2; id++) {
        drv.phy_id = p.phy_id = id;
        reset(); cal_result = -EIO;
        assert(airoha_phy_config_init(&p) == -EIO && calibrated == 1 && !package_lock);
        reset(); assert(!airoha_phy_config_init(&p) && calibrated == 1 && !package_lock);
    }
    struct phy_device wrong = {.phy_id = 99}; port0 = &wrong;
    assert(airoha_phy_config_init(&p) == -ENODEV && !package_lock);
    puts("PHY: completion, timeout, MDIO errors, cleanup and package serialization passed");
}
'''
adv_header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define __ETHTOOL_DECLARE_LINK_MODE_MASK(n) unsigned long n[1]
#define linkmode_andnot(d, a, b) (*(d) = *(a) & ~*(b))
#define linkmode_or(d, a, b) (*(d) = *(a) | *(b))
struct phylink { unsigned long advertising[1], user_disabled_advertising[1]; };
'''
adv_tests = r'''
int main(void)
{
    struct phylink pl;
    for (unsigned long initial = 0; initial < 16; initial++)
    for (unsigned long available = 0; available < 16; available++)
    for (unsigned long requested = 0; requested < 16; requested++) {
        /* Start with all capabilities and an explicit user selection. */
        pl.advertising[0] = 15; pl.user_disabled_advertising[0] = 0;
        phylink_update_user_disabled_advertising(&pl, &initial);
        /* A provider disappears; the user changes only available modes. */
        pl.advertising[0] = available;
        phylink_update_user_disabled_advertising(&pl, &requested);
        /* When the provider returns, its old user choices must survive. */
        for (unsigned long mode = 1; mode <= 8; mode <<= 1) {
            bool enabled = !(pl.user_disabled_advertising[0] & mode);
            if (available & mode)
                assert(enabled == !!(requested & mode));
            else
                assert(enabled == !!(initial & mode));
        }
    }
    puts("PCS: 4096 provider loss/change/return sequences preserve user restrictions");
}
'''
with tempfile.TemporaryDirectory(prefix='airoha-backport-tests-') as directory:
    directory = Path(directory)
    for name, code in (
        ('phy', header + body + tests),
        ('pcs', adv_header + function(pcs, 'phylink_update_user_disabled_advertising') + adv_tests),
    ):
        source, binary = directory / f'{name}.c', directory / name
        source.write_text(code)
        subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
