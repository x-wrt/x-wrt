#!/bin/sh
#
# Licensed under the terms of the GNU GPL License version 2 or later.
#
# Author: Peter Tyser <ptyser@xes-inc.com>
# Modified for multi-DTB and multi-Config support.
#
# U-Boot firmware supports the booting of images in the Flattened Image
# Tree (FIT) format.  The FIT format uses a device tree structure to
# describe a kernel image, device tree blob, ramdisk, etc.  This script
# creates an Image Tree Source (.its file) which can be passed to the
# 'mkimage' utility to generate an Image Tree Blob (.itb file).  The .itb
# file can then be booted by U-Boot (or other bootloaders which support
# FIT images).  See doc/uImage.FIT/howto.txt in U-Boot source code for
# additional information on FIT images.
#

usage() {
	printf "Usage: %s -A arch -C comp -a addr -e entry" "$(basename "$0")"
	printf " -v version -k kernel [-D name -n address -d dtb] -o its_file"

	printf "\n\t-A ==> set architecture to 'arch'"
	printf "\n\t-C ==> set compression type 'comp'"
	printf "\n\t-c ==> set config name 'config' (can be specified more than once)"
	printf "\n\t-a ==> set load address to 'addr' (hex)"
	printf "\n\t-e ==> set entry point to 'entry' (hex)"
	printf "\n\t-f ==> set device tree compatible string"
	printf "\n\t-i ==> include initrd Blob 'initrd'"
	printf "\n\t-v ==> set kernel version to 'version'"
	printf "\n\t-k ==> include kernel image 'kernel'"
	printf "\n\t-D ==> human friendly Device Tree Blob 'name'"
	printf "\n\t-n ==> fdt unit-address 'address'"
	printf "\n\t-d ==> include Device Tree Blob 'dtb' (can be specified more than once)"
	printf "\n\t-r ==> include RootFS blob 'rootfs'"
	printf "\n\t-H ==> specify hash algo instead of SHA1"
	printf "\n\t-l ==> legacy mode character (@ etc otherwise -)"
	printf "\n\t-o ==> create output file 'its_file'"
	printf "\n\t-O ==> create config with dt overlay 'name:dtb'"
	printf "\n\t-s ==> set FDT load address to 'addr' (hex)"
	printf "\n\t\t(can be specified more than once)\n"
	exit 1
}

REFERENCE_CHAR='-'
FDTNUM=1
ROOTFSNUM=1
INITRDNUM=1
HASH=sha1
LOADABLES=
DTOVERLAY=
DTADDR=
DTBS=""
CONFIGS=""

while getopts ":A:a:c:C:D:d:e:f:i:k:l:n:o:O:v:r:s:H:" OPTION
do
	case $OPTION in
		A ) ARCH=$OPTARG;;
		a ) LOAD_ADDR=$OPTARG;;
		c ) CONFIGS="$CONFIGS $OPTARG";; # 累加 Config
		C ) COMPRESS=$OPTARG;;
		D ) DEVICE=$OPTARG;;
		d ) DTBS="$DTBS $OPTARG";;     # 累加 DTB
		e ) ENTRY_ADDR=$OPTARG;;
		f ) COMPATIBLE=$OPTARG;;
		i ) INITRD=$OPTARG;;
		k ) KERNEL=$OPTARG;;
		l ) REFERENCE_CHAR=$OPTARG;;
		n ) FDTNUM=$OPTARG;;
		o ) OUTPUT=$OPTARG;;
		O ) DTOVERLAY="$DTOVERLAY ${OPTARG}";;
		r ) ROOTFS=$OPTARG;;
		s ) FDTADDR=$OPTARG;;
		H ) HASH=$OPTARG;;
		v ) VERSION=$OPTARG;;
		* ) echo "Invalid option passed to '$0' (options:$*)"
		usage;;
	esac
done

# Make sure user entered all required parameters
if [ -z "${ARCH}" ] || [ -z "${COMPRESS}" ] || [ -z "${LOAD_ADDR}" ] || \
	[ -z "${ENTRY_ADDR}" ] || [ -z "${VERSION}" ] || [ -z "${KERNEL}" ] || \
	[ -z "${OUTPUT}" ] || [ -z "${CONFIGS}" ]; then
	usage
fi

if [ -n "${ROOTFS}" ] && [ ! -f "${ROOTFS}".pagesync ]; then
	echo "Missing .pagesync blob for RootFS blob '${ROOTFS}'"
	exit 1
fi

ARCH_UPPER=$(echo "$ARCH" | tr '[:lower:]' '[:upper:]')

if [ -n "${COMPATIBLE}" ]; then
	COMPATIBLE_PROP="compatible = \"${COMPATIBLE}\";"
fi

[ "$FDTADDR" ] && {
	DTADDR="$FDTADDR"
}

if [ -n "${INITRD}" ]; then
	INITRD_NODE="
		initrd${REFERENCE_CHAR}$INITRDNUM {
			description = \"${ARCH_UPPER} OpenWrt ${DEVICE} initrd\";
			${COMPATIBLE_PROP}
			data = /incbin/(\"${INITRD}\");
			type = \"ramdisk\";
			arch = \"${ARCH}\";
			os = \"linux\";
			hash${REFERENCE_CHAR}1 {
				algo = \"crc32\";
			};
			hash${REFERENCE_CHAR}2 {
				algo = \"${HASH}\";
			};
		};
"
	INITRD_PROP="ramdisk=\"initrd${REFERENCE_CHAR}${INITRDNUM}\";"
fi


if [ -n "${ROOTFS}" ]; then
	ROOTFS_NODE="
		rootfs${REFERENCE_CHAR}$ROOTFSNUM {
			description = \"${ARCH_UPPER} OpenWrt ${DEVICE} rootfs\";
			${COMPATIBLE_PROP}
			data = /incbin/(\"${ROOTFS}.pagesync\");
			type = \"filesystem\";
			arch = \"${ARCH}\";
			compression = \"none\";
			hash${REFERENCE_CHAR}1 {
				algo = \"crc32\";
			};
			hash${REFERENCE_CHAR}2 {
				algo = \"${HASH}\";
			};
		};
"
	LOADABLES="${LOADABLES:+$LOADABLES, }\"rootfs${REFERENCE_CHAR}${ROOTFSNUM}\""
fi

# ===================================================================
# 核心修改点：动态处理多个 DTB 和 Config
# ===================================================================
FDT_NODES=""
MAIN_CONFIGS=""

# 提取第一个 config 作为 default 启动项
for c in $CONFIGS; do
	DEFAULT_CONFIG="$c"
	break
done
[ -z "$DEFAULT_CONFIG" ] && DEFAULT_CONFIG="config-1"

# 使用 shell 参数机制来迭代读取 CONFIGS
set -- $CONFIGS
CURRENT_FDTNUM=$FDTNUM

if [ -n "${DTBS}" ]; then
	for dtb in $DTBS; do
		descname="${dtb##*/}"
		descname="${descname#image-}"
		descname="${descname%.dtb}"
		# 一一映射：取出当前循环对应的 config 名字
		cfg=$1
		if [ -z "$cfg" ]; then
			cfg="config-${CURRENT_FDTNUM}"
		else
			shift # 移到下一个 config
		fi

		# 拼接 DTB 节点
		FDT_NODES="${FDT_NODES}
		fdt${REFERENCE_CHAR}${CURRENT_FDTNUM} {
			description = \"${ARCH_UPPER} OpenWrt ${DEVICE}(${descname}) device tree blob\";
			${COMPATIBLE_PROP}
			data = /incbin/(\"${dtb}\");
			type = \"flat_dt\";
			${DTADDR:+load = <${DTADDR}>;}
			arch = \"${ARCH}\";
			compression = \"none\";
			hash${REFERENCE_CHAR}1 {
				algo = \"crc32\";
			};
			hash${REFERENCE_CHAR}2 {
				algo = \"${HASH}\";
			};
		};"

		FDT_PROP="fdt = \"fdt${REFERENCE_CHAR}${CURRENT_FDTNUM}\";"

		# 拼接对应的 Config 节点
		MAIN_CONFIGS="${MAIN_CONFIGS}
		${cfg} {
			description = \"OpenWrt ${DEVICE}(${descname})\";
			kernel = \"kernel${REFERENCE_CHAR}1\";
			${FDT_PROP}
			${LOADABLES:+loadables = ${LOADABLES};}
			${COMPATIBLE_PROP}
			${INITRD_PROP}
		};"

		CURRENT_FDTNUM=$((CURRENT_FDTNUM + 1))
	done
else
	# 如果没有传入 DTB (-d)，仍然需要生成只有 kernel 的 config
	MAIN_CONFIGS="
		${DEFAULT_CONFIG} {
			description = \"OpenWrt ${DEVICE}\";
			kernel = \"kernel${REFERENCE_CHAR}1\";
			${LOADABLES:+loadables = ${LOADABLES};}
			${COMPATIBLE_PROP}
			${INITRD_PROP}
		};"
fi

# add DT overlay blobs
FDTOVERLAY_NODE=""
OVCONFIGS=""
[ "$DTOVERLAY" ] && for overlay in $DTOVERLAY ; do
	overlay_blob=${overlay##*:}
	ovname=${overlay%%:*}
	ovnode="fdt-$ovname"
	ovsize=$(wc -c "$overlay_blob" | awk '{print $1}')
	echo "$ovname ($overlay_blob) : $ovsize" >&2
	FDTOVERLAY_NODE="$FDTOVERLAY_NODE

		$ovnode {
			description = \"${ARCH_UPPER} OpenWrt ${DEVICE} device tree overlay $ovname\";
			${COMPATIBLE_PROP}
			data = /incbin/(\"${overlay_blob}\");
			type = \"flat_dt\";
			arch = \"${ARCH}\";
			compression = \"none\";
			hash${REFERENCE_CHAR}1 {
				algo = \"crc32\";
			};
			hash${REFERENCE_CHAR}2 {
				algo = \"${HASH}\";
			};
		};
"
	OVCONFIGS="$OVCONFIGS

		$ovname {
			description = \"OpenWrt ${DEVICE} overlay $ovname\";
			fdt = \"$ovnode\";
			${COMPATIBLE_PROP}
		};
	"
done

# Create a default, fully populated DTS file
DATA="/dts-v1/;

/ {
	description = \"${ARCH_UPPER} OpenWrt FIT (Flattened Image Tree)\";
	#address-cells = <1>;

	images {
		kernel${REFERENCE_CHAR}1 {
			description = \"${ARCH_UPPER} OpenWrt Linux-${VERSION}\";
			data = /incbin/(\"${KERNEL}\");
			type = \"kernel\";
			arch = \"${ARCH}\";
			os = \"linux\";
			compression = \"${COMPRESS}\";
			load = <${LOAD_ADDR}>;
			entry = <${ENTRY_ADDR}>;
			hash${REFERENCE_CHAR}1 {
				algo = \"crc32\";
			};
			hash${REFERENCE_CHAR}2 {
				algo = \"$HASH\";
			};
		};
${INITRD_NODE}
${FDT_NODES}
${FDTOVERLAY_NODE}
${ROOTFS_NODE}
	};

	configurations {
		default = \"${DEFAULT_CONFIG}\";
${MAIN_CONFIGS}
${OVCONFIGS}
	};
};"

# Write .its file to disk
echo "$DATA" > "${OUTPUT}"
