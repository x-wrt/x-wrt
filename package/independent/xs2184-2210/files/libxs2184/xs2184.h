#ifndef _XS2184_H_
#define _XS2184_H_

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <linux/types.h>
#include <sys/stat.h>
#include <uci.h>
#include <math.h>
#include <time.h>
#include "./i2cbusses.h"
#include "./i2c-dev.h"

typedef unsigned char u8;

enum {
	CMD_ERROR = -1,
	CMD_SUCCESS = 0,
};

enum {
	MANUAL = 0,
	AUTO,
};

#define MODE                   MANUAL
#define CHIP_ADDR              0x74
#define LAN_PORT_NUM           8
#define WAN_PORT_NUM           2
#define PORT_NUM               LAN_PORT_NUM + WAN_PORT_NUM
#define foreach_port(i)        for(i=1; i<PORT_NUM+1; i++)
#define MAX_CHIPS              2
#define PAGE_REG               0x00
#define PsE_REG_PAGE0          0
#define PsE_REG_PAGE1          1
#define PoE_ENABLE             1
#define PoE_DISABLE            0

#define  POWER_STATUS_REG      0x82

#define P0_CRT_MSB_REG         0xa0
// #define P0_VRT_MSB_REG         0xb0
#define SUPPLY_VOLTAGE_MSB_REG 0xe0

#define PORT0_EN_EN_REG        0x98
#define PORT_EN_REG(vp)        PORT0_EN_EN_REG+((vp-1)%8)

#define E_FUSE_CTEL            0x50
#define E_FUSE_ADDR            0x51
#define E_FUSE_R_DAT           0x53

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr)        (sizeof(arr)/sizeof((arr)[0]))
#endif

int open_chip(u8 file, u8 chip_addr);
int enable_port(char port_num);
int disable_port(char port_num);

#endif /* _XS2184_H_ */
