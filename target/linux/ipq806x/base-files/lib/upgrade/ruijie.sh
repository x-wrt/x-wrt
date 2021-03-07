#
# Copyright (C) 2016 lede-project.org
#

ruijie_do_flash() {
	local tar_file="$1"
	local kernel="$2"
	local rootfs="$3"

	local tar_listing
	local board_dir
	local rootfs_size
	local overlay_block

	tar_listing="$(tar tf "$tar_file")" || return 1
	# use the first found directory in the tar archive
	board_dir="$(printf '%s\n' "$tar_listing" | grep -m 1 '^sysupgrade-.*/$')"
	board_dir=${board_dir%/}
	[ -n "$board_dir" ] || return 1
	tar tf "$tar_file" "$board_dir/kernel" "$board_dir/root" >/dev/null 2>&1 || return 1

	rootfs_size="$(tar tvf "$tar_file" "$board_dir/root")" || return 1
	rootfs_size="$(printf '%s\n' "$rootfs_size" | awk '$1 ~ /^-/ { print $3 }')"
	case "$rootfs_size" in
		''|*[!0-9]*) return 1 ;;
	esac
	[ "$rootfs_size" -gt 0 ] || return 1
	# Match the 64 KiB rootfs padding and fstools overlay alignment.
	overlay_block=$(((rootfs_size + 65535) / 65536))
	if [ -n "$UPGRADE_BACKUP" ]; then
		gzip -t "$UPGRADE_BACKUP" || return 1
	fi

	# keep sure its unbound
	losetup --detach-all || {
		echo "Failed to detach all loop devices." >&2
		return 1
	}

	echo "flashing kernel to $kernel"
	tar xf "$tar_file" "$board_dir/kernel" -O > "$kernel" || return 1

	echo "flashing rootfs to $rootfs"
	tar xf "$tar_file" "$board_dir/root" -O > "$rootfs" || return 1

	if [ -n "$UPGRADE_BACKUP" ]; then
		# fstools restores this archive before formatting the new overlay.
		dd if="$UPGRADE_BACKUP" of="$rootfs" bs=65536 seek="$overlay_block" conv=notrunc || return 1
	else
		# Invalidate the old overlay when upgrading without configuration.
		dd if=/dev/zero of="$rootfs" bs=65536 seek="$overlay_block" count=1 conv=notrunc || return 1
	fi

	# Cleanup
	losetup -d /dev/loop0 >/dev/null 2>&1
	sync
	umount -a
	reboot -f
}

ruijie_do_upgrade() {
	local tar_file="$1"
	local board=$(board_name)
	local kernel=
	local rootfs=

	case "$board" in
	ruijie,rg-mtfi-m520)
		kernel="/dev/mmcblk0p2"
		rootfs="/dev/mmcblk0p3"
		;;
	*)
		return 1
		;;
	esac

	ruijie_do_flash "$tar_file" "$kernel" "$rootfs" || {
		sync
		echo "Ruijie firmware upgrade failed." >&2
		exit 1
	}
}
