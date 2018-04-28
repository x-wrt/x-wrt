#!/bin/bash

file=./dl/u-boot-2015.10.tar.bz2

### Check file u-boot-2015.10.tar.bz2
if test -e $file
then
  echo "File u-boot-2015.10.tar.bz2 found."
else
 wget -P dl/ ftp://ftp.denx.de/pub/u-boot/u-boot-2015.10.tar.bz2
fi

#Copy config myconfig
cp nitroconf .config

make menuconfig

### Build rom
make -j$(nproc)

cp .config nitroconf
