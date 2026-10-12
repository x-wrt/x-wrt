RAMFS_COPY_BIN='fitblk fit_check_sign'

REQUIRE_IMAGE_METADATA=1

nokia_initial_setup()
{
	[ "$(rootfs_type)" = "tmpfs" ] || return 0

	fw_setenv bootcmd "flash read 0xc0000 0x800000 0x85000000; bootm 0x85000000"
}

airoha_require_ubi_layout()
{
	local ubidev volume fip_volume

	ubidev="$(nand_find_ubi ubi)"
	[ -n "$ubidev" ] || {
		echo "UBI is not initialized; create the base volumes from U-Boot recovery."
		return 1
	}

	for volume in fip ubootenv ubootenv2 "$@"; do
		if ! nand_find_volume "$ubidev" "$volume" >/dev/null; then
			echo "UBI base volume $volume is missing; repair the layout from U-Boot recovery."
			return 1
		fi
	done

	fip_volume="$(nand_find_volume "$ubidev" fip)"
	[ "$(cat "/sys/class/ubi/$fip_volume/data_bytes")" -gt 0 ] || {
		echo "The fip volume does not contain BL31/U-Boot; install the boot chain first."
		return 1
	}
}


platform_check_image() {
	local board=$(board_name)

	[ "$#" -gt 1 ] && return 1

	case "$board" in
	nokia,xg-040g-md)
		nand_do_platform_check "$board" "$1"
		return $?
		;;
	gemtek,xg2010g|\
	fiberhome,hg5382a|\
	fiberhome,hg5585f-ct|\
	fiberhome,hg5585f-cu|\
	unionman,ung00a|\
	znxt,zn504xg-d|\
	znxt,zn515xg-d|\
	nokia,xg-040g-md-ubi|\
	quantum,q1000k-ubi)
		fit_check_image "$1"
		return $?
		;;
	esac

	return 0
}

platform_do_upgrade() {
	local board=$(board_name)

	case "$board" in
		gemtek,w1700k-ubi|\
		quantum,q1000k-ubi)
			fit_do_upgrade "$1"
			;;
		gemtek,xg2010g|\
		fiberhome,hg5382a|\
		fiberhome,hg5585f-ct|\
		fiberhome,hg5585f-cu|\
		unionman,ung00a|\
		znxt,zn504xg-d|\
		znxt,zn515xg-d)
			airoha_require_ubi_layout factory && fit_do_upgrade "$1"
			;;
		nokia,xg-040g-md-ubi)
			airoha_require_ubi_layout bosa ri && fit_do_upgrade "$1"
			;;
		*)
			nand_do_upgrade "$1"
			;;
	esac
}

platform_pre_upgrade() {
	local board=$(board_name)

	case "$board" in
	nokia,xg-040g-md)
		nokia_initial_setup
		;;
	*)
		;;
	esac
}
