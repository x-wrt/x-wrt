#!/bin/bash
# End-to-end test of zte-fwslot on file-backed fake flash (see fakemtd.c).
#   run.sh <zte-fwslot> <fakemtd.so> <vendor.bin> <openwrt.bin> <workdir>
set -u
TOOL=$1 SHIM=$2 VENDOR=$3 OWRT=$4 WORK=$5
R=$WORK/fake
S0=0x600000 S1=0x2f00000
pass=0 fail=0

ok()   { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
bad()  { fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; }
t()    { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

fw()   { LD_PRELOAD=$SHIM FAKE_ROOT=$R "$TOOL" "$@"; }
run_in() { echo "$1" > "$R/sys/firmware/zx279133/slot_offset"; }   # which slot "booted"
json() { fw info -j | python3 -c "import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$1"; }

fresh() {
	rm -rf "$R"; mkdir -p "$R/dev" "$R/sys/class/mtd/mtd1" "$R/sys/class/mtd/mtd2" "$R/sys/firmware/zx279133"
	for n in 1 2; do
		python3 - "$R/dev/mtd$n" <<'EOF'
import sys
with open(sys.argv[1], "wb") as f:
    f.write(b"\xff" * 0x2900000)
EOF
	done
	echo 6291456  > "$R/sys/class/mtd/mtd1/offset"
	echo 49283072 > "$R/sys/class/mtd/mtd2/offset"
	unset FAKE_BAD FAKE_WRITE_FAIL FAKE_CORRUPT FAKE_WRITE_RO
}

echo "== fresh flash, nothing installed"
fresh; run_in $S0
t "info on empty flash reports two invalid slots"  '[ "$(json "[s[\"valid\"] for s in d[\"slots\"]]")" = "[False, False]" ]'
t "switch with nothing to switch to is refused"     '! fw switch 2>/dev/null'

echo "== install the vendor firmware into slot 0 (booted from slot 1), then OpenWrt into slot 1"
run_in $S1
fw flash "$VENDOR" 2>/dev/null; t "flash vendor package" '[ $? -eq 0 ]'
run_in $S0
fw flash "$OWRT" 2>/dev/null;   t "flash OpenWrt package" '[ $? -eq 0 ]'
v0=$(json 'd["slots"][0]["version"]'); echo "        slot 0 version: $v0"
t "slot 0 holds the vendor firmware"  '[ -n "$v0" ] && [[ "${v0,,}" != *wrt* ]]'
t "slot 1 holds X-Wrt"          'json "d[\"slots\"][1][\"version\"]" | grep -qi X-Wrt'
t "newest header wins: next boot is slot 1" '[ "$(json "d[\"next\"]")" = 1 ]'
t "running slot is reported"          '[ "$(json "d[\"running\"]")" = 0 ]'
fw info | sed 's/^/        /'

echo "== switch back to the vendor system"
run_in $S1
fw switch 2>&1 | sed 's/^/        /'
t "switch succeeded"                   '[ ${PIPESTATUS[0]} -eq 0 ]'
t "next boot is now slot 0"            '[ "$(json "d[\"next\"]")" = 0 ]'
t "serial of slot 0 is the highest"    '[ "$(json "d[\"slots\"][0][\"serial\"] > d[\"slots\"][1][\"serial\"]")" = True ]'

echo "== corruption is caught before the serial is touched"
for what in "uboot:100" "kernel-magic:$((0x180000))" "kernel:$((0x180020 + 4096))" "rootfs:$((0x6e0000 + 100000))"; do
	name=${what%%:*} ofs=${what##*:}
	fresh; run_in $S1; fw flash "$VENDOR" 2>/dev/null; run_in $S0; fw flash "$OWRT" 2>/dev/null
	before=$(json 'd["slots"][0]["serial"]')
	printf '\x5a' | dd of="$R/dev/mtd1" bs=1 seek=$ofs conv=notrunc 2>/dev/null
	run_in $S1
	out=$(fw switch 2>&1); rc=$?
	t "damaged $name: switch refused ($out)" '[ $rc -ne 0 ] && echo "$out" | grep -q damaged'
	t "damaged $name: serial untouched"      '[ "$(json "d[\"slots\"][0][\"serial\"]")" = "$before" ]'
done

echo "== bad blocks in the payload are skipped consistently"
fresh; run_in $S1; fw flash "$VENDOR" 2>/dev/null
export FAKE_BAD="2:0x200000,2:0x400000"       # mtd2 is slot 1: blocks 16 and 32
run_in $S0; n=$(fw flash "$OWRT" 2>&1 | grep -c "skip bad block"); echo "        skipped $n bad blocks"
t "both bad blocks were skipped while writing"           '[ "$n" -eq 2 ]'
t "a slot with bad blocks verifies when they are known"  'fw switch 2>/dev/null'
unset FAKE_BAD
t "read without skipping it fails, so they really were skipped" '! fw switch 2>/dev/null'

echo "== a block that refuses a write is marked bad, and the slot still verifies"
fresh; run_in $S1; fw flash "$VENDOR" 2>/dev/null
export FAKE_WRITE_FAIL="2:0x300000"
run_in $S0; fw flash "$OWRT" 2>&1 | grep -E "write failed" | head -2 | sed 's/^/        /'
t "the failed block is in the bad block list"            'grep -q "^2 300000" "$R/badlist"'
unset FAKE_WRITE_FAIL
t "the slot written around the failed block verifies"    'fw switch 2>/dev/null'

echo "== unknown running slot is refused rather than guessed"
fresh; run_in $S1; fw flash "$VENDOR" 2>/dev/null
rm -f "$R/sys/firmware/zx279133/slot_offset"
t "flash refuses"  '! fw flash "$OWRT" 2>/dev/null'
t "switch refuses" '! fw switch 2>/dev/null'

echo "== payload readback is verified before publishing the next boot header"
fresh; run_in $S0
export FAKE_CORRUPT="2:0x0"
out=$(fw flash "$OWRT" 2>&1); rc=$?
t "failed payload verification is reported" '[ "$rc" -ne 0 ] && echo "$out" | grep -q "U-Boot CRC mismatch"'
next=$(json 'd["next"]')
t "no header is published for the corrupted payload" '[ "$next" = -1 ]'

echo "== insufficient good blocks leave the previous target intact"
fresh; run_in $S0
fw flash "$VENDOR" 2>/dev/null
before=$(sha256sum "$R/dev/mtd2")
export FAKE_BAD
FAKE_BAD=$(python3 -c 'print(",".join(f"2:{ofs:x}" for ofs in range(0x400000, 0x2900000, 0x20000)))')
out=$(fw flash "$OWRT" 2>&1); rc=$?
t "capacity is checked before erase" '[ "$rc" -ne 0 ] && echo "$out" | grep -q "not enough good blocks"'
t "the target slot has not been erased" '[ "$(sha256sum "$R/dev/mtd2")" = "$before" ]'

echo "== an overlay staging failure leaves the running slot selected"
fresh; run_in $S1; fw flash "$VENDOR" 2>/dev/null; run_in $S0
mkdir -p "$R/usr/libexec"
helper="$R/usr/libexec/zte-stage-overlay"
printf '#!/bin/sh\nexit 1\n' > "$helper"; chmod +x "$helper"
t "sysupgrade propagates the staging failure" '! fw sysupgrade "$OWRT" 2>/dev/null'
t "failed staging does not activate the new firmware" '[ "$(json "d[\"next\"]")" = 0 ]'
printf '#!/bin/sh\nexit 0\n' > "$helper"
t "sysupgrade publishes after successful staging" 'fw sysupgrade "$OWRT" 2>/dev/null && [ "$(json "d[\"next\"]")" = 1 ]'

echo "== non-media write failures do not consume good blocks"
fresh; run_in $S0
export FAKE_WRITE_RO="2:0x0"
t "read-only error aborts the write" '! fw flash "$OWRT" 2>/dev/null'
t "no block was marked bad" '[ ! -s "$R/badlist" ]'

echo
echo "passed $pass, failed $fail"
[ "$fail" -eq 0 ]
