# SPDX-License-Identifier: GPL-2.0-only
#
# SR1010 sysupgrade. A vendor-format package is installed into the inactive
# firmware slot and the vendor bootloader starts it on the next boot. The same
# package also installs from the vendor web interface, so OpenWrt and the
# vendor firmware can be swapped in place.
#
# The overlay lives in the .owrt directory of the shared Plugin partition and
# the vendor files in the rest of that partition are never touched.

RAMFS_COPY_BIN='zte-fwslot mktemp'
RAMFS_COPY_DATA='/usr/libexec/zte-stage-overlay'

platform_check_image() {
	zte-fwslot check "$1"
}

platform_do_upgrade() {
	zte-fwslot sysupgrade "$1" || {
		v "SR1010 firmware upgrade failed"
		exit 1
	}
}
