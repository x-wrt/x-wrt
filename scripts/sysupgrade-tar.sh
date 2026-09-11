#!/bin/sh

. $TOPDIR/scripts/functions.sh

board=""
kernel=""
rootfs=""
dtb=""
outfile=""
err=""

while [ "$1" ]; do
	case "$1" in
	"--board")
		board="$2"
		shift
		shift
		continue
		;;
	"--kernel")
		kernel="$2"
		shift
		shift
		continue
		;;
	"--rootfs")
		rootfs="$2"
		shift
		shift
		continue
		;;
	"--dtb")
		dtb="$2"
		shift
		shift
		continue
		;;
	*)
		if [ ! "$outfile" ]; then
			outfile=$1
			shift
			continue
		fi
		;;
	esac
done

if [ ! -n "$board" -o ! -r "$kernel" -a  ! -r "$rootfs" -o ! "$outfile" -o -n "$dtb" -a ! -r "$dtb" ]; then
	echo "syntax: $0 [--board boardname] [--kernel kernelimage] [--rootfs rootfs] [--dtb dtb] out"
	exit 1
fi

tmpdir="$( mktemp -d 2> /dev/null )"
if [ -z "$tmpdir" ]; then
	# try OSX signature
	tmpdir="$( mktemp -t 'ubitmp' -d )"
fi

if [ -z "$tmpdir" ]; then
	exit 1
fi

trap 'rm -rf "$tmpdir"' EXIT

mkdir -p "${tmpdir}/sysupgrade-${board}" || exit 1
echo "BOARD=${board}" > "${tmpdir}/sysupgrade-${board}/CONTROL" || exit 1
if [ -n "${rootfs}" ]; then
	case "$( get_fs_type ${rootfs} )" in
	"squashfs")
		dd if="${rootfs}" of="${tmpdir}/sysupgrade-${board}/root" bs=1024 conv=sync || exit 1
		;;
	*)
		cp "${rootfs}" "${tmpdir}/sysupgrade-${board}/root" || exit 1
		;;
	esac
fi
[ -z "${kernel}" ] || cp "${kernel}" "${tmpdir}/sysupgrade-${board}/kernel" || exit 1
[ -z "${dtb}" ] || cp "${dtb}" "${tmpdir}/sysupgrade-${board}/dtb" || exit 1

mtime=""
if [ -n "$SOURCE_DATE_EPOCH" ]; then
	mtime="--mtime=@${SOURCE_DATE_EPOCH}"
fi

(cd "$tmpdir"; tar --sort=name --owner=0 --group=0 --numeric-owner -cvf sysupgrade.tar sysupgrade-${board} ${mtime})
err="$?"
if [ "$err" -eq 0 ]; then
	if [ -e "$tmpdir/sysupgrade.tar" ]; then
		cp "$tmpdir/sysupgrade.tar" "$outfile" || err=$?
	else
		err=2
	fi
fi

exit $err
