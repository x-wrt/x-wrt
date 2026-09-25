# SPDX-License-Identifier: GPL-2.0-only

REQUIRE_IMAGE_METADATA=1

platform_check_image() {
	case "$(board_name)" in
	tenda,be6l-pro)
		nand_do_platform_check "$(board_name)" "$1"
		;;
	*)
		return 0
		;;
	esac
}

platform_do_upgrade() {
	case "$(board_name)" in
	tenda,be6l-pro)
		CI_KERNPART="kernel"
		CI_UBIPART="ubi"
		nand_do_upgrade "$1"
		;;
	*)
		default_do_upgrade "$1"
		;;
	esac
}
