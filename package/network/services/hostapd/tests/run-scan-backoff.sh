#!/bin/sh
set -eu

: "${HOSTAPD_TREE:?Set HOSTAPD_TREE to the expanded, fully patched package source}"
: "${UCODE_INCLUDE:?Set UCODE_INCLUDE to the directory containing ucode/lib.h}"

scan_tmp="$(mktemp -d /tmp/xwrt-scan-test.XXXXXX)"
trap 'rm -rf "$scan_tmp"' EXIT HUP INT TERM
scan_test="$(pwd)/package/network/services/hostapd/tests/scan-backoff.c"

cat > "$scan_tmp/config" <<EOF
CONFIG_DRIVER_NONE=y
CONFIG_L2_PACKET=none
CONFIG_TLS=openssl
CONFIG_CTRL_IFACE=y
CONFIG_BACKEND=file
CONFIG_NO_GITVER=y
CONFIG_NO_RRM=y
CONFIG_NO_ROBUST_AV=y
CONFIG_NO_WMM_AC=y
CONFIG_NO_WPA_PASSPHRASE=y
EXTRA_CFLAGS += -I$UCODE_INCLUDE ${APSTA_CFLAGS:-}
EOF
cat > "$scan_tmp/test.conf" <<'EOF'
network={
	ssid="unavailable-scan-test"
	key_mgmt=NONE
}
EOF
cat > "$scan_tmp/test.mk" <<'EOF'
scan_test_objects = $(filter-out %/main.o %/scan.o,$(OBJS))
$(SCAN_TEST_BINARY): $(scan_test_objects) $(SCAN_TEST_SOURCE)
	$(CC) $(CFLAGS) -I$(abspath ..) -c $(SCAN_TEST_SOURCE) -o $(SCAN_TEST_BINARY).o
	$(LDO) $(LDFLAGS) -o $@ $(SCAN_TEST_BINARY).o $(scan_test_objects) $(LIBS) $(EXTRALIBS)
EOF

if ! make -C "$HOSTAPD_TREE/wpa_supplicant" -f Makefile -f "$scan_tmp/test.mk" \
	CONFIG_FILE="$scan_tmp/config" BUILDDIR="$scan_tmp/build" \
	SCAN_TEST_SOURCE="$scan_test" SCAN_TEST_BINARY="$scan_tmp/test" \
	-j"${JOBS:-2}" "$scan_tmp/test" > "$scan_tmp/build.log" 2>&1; then
	cat "$scan_tmp/build.log"
	exit 1
fi
"$scan_tmp/test" "$scan_tmp/test.conf"
