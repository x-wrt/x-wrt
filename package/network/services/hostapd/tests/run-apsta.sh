#!/bin/sh
set -eu

# HOSTAPD_TREE must contain the exact package sources plus the full patch stack.
: "${HOSTAPD_TREE:?Set HOSTAPD_TREE to the expanded hostapd source tree}"
: "${UCODE_INCLUDE:?Set UCODE_INCLUDE to the directory containing ucode/lib.h}"
: "${UCODE_LIB_DIR:?Set UCODE_LIB_DIR to the directory containing libucode.so}"
: "${UCODE:?Set UCODE to the host ucode executable}"

apsta_tmp="$(mktemp -d /tmp/xwrt-apsta.XXXXXX)"
apsta_cc="${CC:-cc}"
apsta_test="package/network/services/hostapd/tests"

for apsta_source in src/utils/ucode.c src/common/ieee802_11_common.c; do
	# APSTA_CFLAGS supplies host dependency headers (ubus, libubox, udebug, json-c).
	# Intentional word splitting, like conventional compiler flag variables.
	"$apsta_cc" ${APSTA_CFLAGS:-} -c -O2 -fPIC -fvisibility=hidden \
		-ffunction-sections -fdata-sections -D_GNU_SOURCE -DUCODE_SUPPORT \
		-DUBUS_SUPPORT -DCONFIG_IEEE80211AX -DCONFIG_IEEE80211BE \
		-I"$HOSTAPD_TREE/src" -I"$HOSTAPD_TREE/src/utils" -I"$UCODE_INCLUDE" \
		"$HOSTAPD_TREE/$apsta_source" -o "$apsta_tmp/$(basename "$(dirname "$apsta_source")").o"
done
"$apsta_cc" ${APSTA_CFLAGS:-} -shared -fPIC -Wl,--gc-sections -Wl,-z,defs \
	-D_GNU_SOURCE -DUCODE_SUPPORT -I"$HOSTAPD_TREE/src" \
	-I"$HOSTAPD_TREE/src/utils" -I"$UCODE_INCLUDE" \
	"$apsta_test/apsta_freq.c" "$apsta_tmp/utils.o" "$apsta_tmp/common.o" \
	-L"$UCODE_LIB_DIR" -lucode -o "$apsta_tmp/apsta_freq.so"
LD_LIBRARY_PATH="$UCODE_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
	"$UCODE" -L "$apsta_tmp" -L "$UCODE_LIB_DIR" "$apsta_test/apsta.uc"

# GCC host sanitizer flags can be overridden, e.g. for another compiler.
apsta_work_flags="${APSTA_WORK_FLAGS--fsanitize=address,undefined --param asan-globals=0 -fno-omit-frame-pointer}"
"$apsta_cc" ${APSTA_CFLAGS:-} $apsta_work_flags -c -O1 -g \
	-ffunction-sections -fdata-sections -D_GNU_SOURCE -DUCODE_SUPPORT \
	-DUBUS_SUPPORT -DCONFIG_SME -DCONFIG_IEEE80211AX \
	-DCONFIG_CTRL_IFACE -DCONFIG_CTRL_IFACE_UNIX -DCONFIG_SHA256 \
	-I"$HOSTAPD_TREE/src" -I"$HOSTAPD_TREE/src/utils" \
	-I"$HOSTAPD_TREE/wpa_supplicant" -I"$UCODE_INCLUDE" \
	"$HOSTAPD_TREE/wpa_supplicant/wpa_supplicant.c" -o "$apsta_tmp/work-native.o"
# Place the harness first: its daemon-edge mocks override the full source
# object's definitions, while connect_work_done/free remain production code.
"$apsta_cc" ${APSTA_CFLAGS:-} $apsta_work_flags -O1 -g \
	-ffunction-sections -fdata-sections -Wl,--gc-sections \
	-Wl,--allow-multiple-definition -D_GNU_SOURCE -DUCODE_SUPPORT \
	-DCONFIG_SME -DCONFIG_IEEE80211AX -DCONFIG_CTRL_IFACE -DCONFIG_CTRL_IFACE_UNIX \
	-I"$HOSTAPD_TREE" -I"$HOSTAPD_TREE/src" -I"$HOSTAPD_TREE/src/utils" \
	-I"$HOSTAPD_TREE/wpa_supplicant" -I"$UCODE_INCLUDE" \
	"$apsta_test/apsta-work.c" "$apsta_tmp/work-native.o" \
	-L"$UCODE_LIB_DIR" -lucode -o "$apsta_tmp/apsta-work"
LD_LIBRARY_PATH="$UCODE_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
	"$apsta_tmp/apsta-work"
printf 'Native test module: %s\n' "$apsta_tmp/apsta_freq.so"
