#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

rescue=$(dirname "$0")/../zx279133/base-files/usr/libexec/zte-overlay-rescue
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export KEEP_D="$tmp/keep.d"
dir="$tmp/plugin/.owrt"

mkdir -p "$KEEP_D" "$tmp/backup/etc"
printf '/etc/config/network\n/etc/missing\n' > "$KEEP_D/base"
printf 'settings\n' > "$tmp/backup/etc/example"
tar -C "$tmp/backup" -czf "$tmp/settings.tgz" etc/example

reset_fixture() {
	rm -rf "$dir"
	mkdir -p "$dir/upper/etc/config" "$dir/work"
	printf 'network config\n' > "$dir/upper/etc/config/network"
	touch "$dir/upper/stale" "$dir/..stale" "$tmp/plugin/vendor"
}

reset_fixture
printf '/etc/custom*/*.conf\n/etc/custom*/\n/etc/absent*\n' > "$dir/upper/etc/sysupgrade.conf"
mkdir "$dir/upper/etc/custom files"
echo 'extra settings' > "$dir/upper/etc/custom files/a.conf"
ln -s "$tmp/backup/etc" "$dir/upper/etc/custom-link"
"$rescue" "$dir"
[ "$(tar -xOzf "$dir/sysupgrade.tgz" ./etc/config/network)" = 'network config' ]
[ "$(tar -xOzf "$dir/sysupgrade.tgz" './etc/custom files/a.conf')" = 'extra settings' ]
[ "$(tar -tzf "$dir/sysupgrade.tgz" | grep -Fxc './etc/custom files/a.conf')" = 1 ]
[ "$(tar -tzf "$dir/sysupgrade.tgz" | grep -Fxc './etc/custom-link')" = 1 ]
if tar -tzf "$dir/sysupgrade.tgz" | grep -q '^\./etc/custom-link/'; then exit 1; fi
[ ! -e "$dir/upper" ]
[ ! -e "$dir/..stale" ]

for pending in sysupgrade.tgz.tmp .sysupgrade.pending; do
	reset_fixture
	cp "$tmp/settings.tgz" "$dir/$pending"
	"$rescue" "$dir"
	cmp "$tmp/settings.tgz" "$dir/sysupgrade.tgz"
	[ ! -e "$dir/$pending" ]
	[ ! -e "$dir/upper" ]
	[ ! -e "$dir/..stale" ]
done

reset_fixture
printf 'partial backup\n' > "$dir/sysupgrade.tgz.tmp"
if "$rescue" "$dir" 2>/dev/null; then
	exit 1
fi
[ -f "$dir/upper/stale" ]
[ -f "$dir/..stale" ]

rm -rf "$dir"
ln -s "$tmp/backup" "$dir"
if "$rescue" "$dir"; then
	exit 1
fi
[ -f "$tmp/backup/etc/example" ]
[ -f "$tmp/plugin/vendor" ]
printf 'overlay recovery tests passed\n'
