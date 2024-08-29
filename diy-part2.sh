#!/bin/bash
#
# Copyright (c) 2019-2020 P3TERX <https://p3terx.com>
#
# This is free software, licensed under the MIT License.
# See /LICENSE for more information.
#
# https://github.com/P3TERX/Actions-OpenWrt
# File name: diy-part2.sh
# Description: OpenWrt DIY script part 2 (After Update feeds)
#

# Modify default IP
sed -i 's/192.168.1.1/192.168.1.1/g' package/base-files/files/bin/config_generate
sed -i '/CONFIG_PACKAGE_glib2=y/d' .config
#echo 'CONFIG_FEED_atinout=y' >>.config
#echo 'CONFIG_PACKAGE_luci-app-atinout-mod=y' >>.config
cd feeds/luci/applications/
git clone https://github.com/4IceG/luci-app-3ginfo-lite.git
git clone https://github.com/4IceG/luci-app-atcommands.git
git clone https://github.com/4IceG/luci-app-modemband.git
git clone https://github.com/4IceG/luci-app-sms-tool.git
cd ../../..
./scripts/feeds update -a; ./scripts/feeds install -a
