#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Run on X-Wrt as root. Also supports: ssh root@HOST 'sh -s --' < this-file
set -eu
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
umask 022

ROOT=/data/ubuntu
SWAP_MB=4096
SOURCE=1
REPAIR=0
MIRROR=https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release
VERSION=24.04.5
LOG=
LOCKED=0

usage() {
	cat <<'EOF'
用法：sh deploy-ubuntu-schroot.sh [选项]
  --root PATH       Ubuntu 根目录，默认 /data/ubuntu；父目录必须位于持久磁盘
  --swap-mb MB      创建的交换文件大小，默认 4096；0 表示不创建或修改 Swap
  --no-source       不检出 X-Wrt 源码
  --repair          升级后修复宿主依赖、配置和入口，不重装 Ubuntu 或更新源码
  --mirror URL      Ubuntu Base 下载目录，必须为 HTTPS；仍使用固定 SHA256
  -h, --help        显示帮助

支持 X-Wrt/OpenWrt 的 x86_64、aarch64，apk 或 opkg。
需已挂载的 ext4/ext3/xfs/btrfs 持久磁盘；btrfs 请使用 --swap-mb 0。
不会清空已有目录、调整磁盘分区或执行固件升级。
远程运行：ssh root@HOST 'sh -s -- --repair' < deploy-ubuntu-schroot.sh
EOF
}

die() { echo "错误：$*" >&2; exit 1; }
log() {
	printf '[%s] %s\n' "$(date '+%F %T')" "$*"
	[ -z "$LOG" ] || printf '[%s] %s\n' "$(date '+%F %T')" "$*" >> "$LOG"
}
run() {
	if "$@" >> "$LOG" 2>&1; then return 0; else
		result=$?
		tail -n 50 "$LOG" >&2
		die "命令失败（$result）：$*；日志：$LOG"
	fi
}
cleanup() {
	[ "$LOCKED" = 0 ] || rm -rf /tmp/xwrt-ubuntu-deploy.lock
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		--root|--swap-mb|--mirror)
			[ "$#" -ge 2 ] || die "$1 缺少参数"
			case "$1" in
				--root) ROOT=$2;;
				--swap-mb) SWAP_MB=$2;;
				--mirror) MIRROR=${2%/};;
			esac
			shift 2;;
		--no-source) SOURCE=0; shift;;
		--repair) REPAIR=1; SOURCE=0; shift;;
		-h|--help) usage; exit 0;;
		*) die "未知选项：$1";;
	esac
done

case "$SWAP_MB" in ''|*[!0-9]*) die "--swap-mb 必须为非负整数";; esac
[ "${#SWAP_MB}" -le 6 ] || die "Swap 最大支持 131072 MiB"
SWAP_MB=$(printf '%s' "$SWAP_MB" | sed 's/^0*//')
SWAP_MB=${SWAP_MB:-0}
[ "$SWAP_MB" -le 131072 ] || die "Swap 最大支持 131072 MiB"
case "$ROOT" in /*) ;; *) die "--root 必须为绝对路径";; esac
ROOT=${ROOT%/}
case "$ROOT" in
	''|*[!a-zA-Z0-9_./-]*|*/../*|*/./*|*/..|*/.) die "根目录路径不合法：$ROOT";;
esac
case "$MIRROR" in https://*) ;; *) die "镜像地址必须使用 HTTPS";; esac
[ "$(id -u)" = 0 ] || die "请在 X-Wrt 宿主机以 root 执行"
[ -f /etc/openwrt_release ] || die "此脚本应在 X-Wrt/OpenWrt 宿主机执行"
case "$(uname -m)" in
	x86_64)
		ARCH=amd64
		SHA256=e77b6f10c2590cef872b33ee9f635a0e3fd1f57fb074c0e52b5c7f56147a0c86;;
	aarch64|arm64)
		ARCH=arm64
		SHA256=a91d5a93010193712d346d761372b7c9db6dfcf093893161c64ca107f05914f2;;
	*) die "仅支持 x86_64 / aarch64 宿主机";;
esac

PARENT=$(dirname "$ROOT")
[ -d "$PARENT" ] || die "父目录不存在：$PARENT；请先挂载数据磁盘"
[ "$(cd "$PARENT" && pwd -P)" = "$PARENT" ] || die "父目录不能包含软链接或重复斜线"
[ ! -L "$ROOT" ] || die "Ubuntu 根目录不能是软链接"
# Find the longest matching mountpoint, including when /data is not mounted.
FSTYPE=$(awk -v path="$ROOT" '
	$2 == "/" || path == $2 || index(path, $2 "/") == 1 {
		if (length($2) > longest) { longest=length($2); type=$3 }
	} END { print type }' /proc/mounts)
case "$FSTYPE" in ext4|ext3|xfs|btrfs) ;; *) die "$ROOT 不在受支持的持久磁盘上（$FSTYPE）；拒绝写入根 overlay 或 tmpfs";; esac
[ "$FSTYPE" != btrfs ] || [ "$SWAP_MB" -eq 0 ] || die "btrfs 请自行配置 Swap，并使用 --swap-mb 0"

mkdir /tmp/xwrt-ubuntu-deploy.lock 2>/dev/null || die "另一部署可能正在运行；锁：/tmp/xwrt-ubuntu-deploy.lock"
LOCKED=1
trap cleanup 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
echo "$$" > /tmp/xwrt-ubuntu-deploy.lock/pid
SETUP=${ROOT}-setup
SWAP=${ROOT}.swap
mkdir -p "$SETUP"
chmod 700 "$SETUP"
LOG=$SETUP/deploy-$(date '+%Y%m%d-%H%M%S')-$$.log
: > "$LOG"
log "开始：$ARCH，Ubuntu $VERSION，根目录 $ROOT；日志：$LOG"
if [ -f "$0" ]; then
	cp -p "$0" "$SETUP/deploy-ubuntu-schroot.sh.new"
	chmod 700 "$SETUP/deploy-ubuntu-schroot.sh.new"
	mv "$SETUP/deploy-ubuntu-schroot.sh.new" "$SETUP/deploy-ubuntu-schroot.sh"
fi

existing_ubuntu() {
	[ -f "$ROOT/etc/os-release" ] &&
		grep -Eq '^ID="?ubuntu"?$' "$ROOT/etc/os-release" &&
		grep -Eq '^VERSION_ID="?24\.04"?$' "$ROOT/etc/os-release" &&
		[ -x "$ROOT/usr/bin/dpkg" ] && [ -x "$ROOT/bin/bash" ]
}

# Reject unrelated data before installing packages or changing configuration.
if [ -e "$ROOT" ]; then
	[ -d "$ROOT" ] || die "$ROOT 不是目录"
	if [ -n "$(ls -A "$ROOT")" ] && [ ! -f "$SETUP/bootstrap-in-progress" ]; then
		existing_ubuntu || die "$ROOT 非空，且不是完整的 Ubuntu 24.04；不会覆盖"
	fi
fi
if [ "$REPAIR" = 1 ]; then
	existing_ubuntu || die "--repair 需要已有完整 Ubuntu 24.04"
	[ ! -f "$SETUP/bootstrap-in-progress" ] || die "基础系统解包未完成，请先不带 --repair 重试"
fi
if [ -f /etc/schroot/chroot.d/ubuntu.conf ]; then
	configured_root=$(sed -n 's/^directory=//p' /etc/schroot/chroot.d/ubuntu.conf)
	[ "$configured_root" = "$ROOT" ] || die "已有 ubuntu 配置指向 $configured_root，拒绝替换"
fi
# A different definition of [ubuntu] would make schroot configuration ambiguous.
for conf in /etc/schroot/schroot.conf /etc/schroot/chroot.d/*; do
	[ -f "$conf" ] || continue
	[ "$conf" = /etc/schroot/chroot.d/ubuntu.conf ] && continue
	if grep -Eq '^\[ubuntu\][[:space:]]*$' "$conf"; then die "$conf 已定义 ubuntu，请先解决命名冲突"; fi
done

if command -v apk >/dev/null 2>&1; then PM=apk
elif command -v opkg >/dev/null 2>&1; then PM=opkg
else die "未找到 apk 或 opkg"; fi
installed() {
	if [ "$PM" = apk ]; then apk info -e "$1" >/dev/null 2>&1
	else opkg status "$1" 2>/dev/null | grep -q '^Status: .* installed$'; fi
}
missing=
for package in schroot block-mount coreutils-cp coreutils-stat curl ca-bundle; do
	installed "$package" || missing="$missing $package"
done
if [ "$SWAP_MB" -gt 0 ] && ! command -v mkswap >/dev/null 2>&1; then missing="$missing swap-utils"; fi
if [ -n "$missing" ]; then
	log "安装宿主软件包：$missing"
	run "$PM" update
	# Package names are fixed above; intentional word splitting.
	# shellcheck disable=SC2086
	if [ "$PM" = apk ]; then run apk add $missing; else run opkg install $missing; fi
fi
for command in schroot curl sha256sum tar gzip chroot; do
	command -v "$command" >/dev/null 2>&1 || die "缺少命令：$command"
done

if [ -f "$SETUP/bootstrap-in-progress" ] || ! existing_ubuntu; then
	free_kb=$(df -Pk "$PARENT" | awk 'NR == 2 {print $4}')
	needed_kb=$((4 * 1024 * 1024 + SWAP_MB * 1024))
	[ "$free_kb" -ge "$needed_kb" ] || die "首次安装需至少 $((needed_kb / 1024)) MiB 可用空间"
	IMAGE=ubuntu-base-${VERSION}-base-${ARCH}.tar.gz
	archive=$SETUP/$IMAGE
	# These pins come from Ubuntu's SHA256SUMS, whose signature was verified
	# with CD signing key 843938DF228D22F7B3742BC0D94AA3F0EFE21092.
	if [ ! -f "$archive" ] || [ "$(sha256sum "$archive" | awk '{print $1}')" != "$SHA256" ]; then
		log "下载官方基础系统：$IMAGE"
		run curl -fL --retry 3 --connect-timeout 20 --max-time 900 --proto '=https' "$MIRROR/$IMAGE" -o "$archive.part"
		[ "$(sha256sum "$archive.part" | awk '{print $1}')" = "$SHA256" ] || die "镜像 SHA256 不匹配，未解包"
		mv "$archive.part" "$archive"
	fi
	printf '%s  %s\n' "$SHA256" "$IMAGE" > "$SETUP/image.sha256"
	mkdir -p "$ROOT"
	: > "$SETUP/bootstrap-in-progress"
	log "解包基础系统；中断后可以重新运行同一命令"
	run tar -xzpf "$archive" -C "$ROOT"
	existing_ubuntu || die "解包后的 Ubuntu 不完整；请重新运行脚本"
	rm -f "$SETUP/bootstrap-in-progress"
fi
[ "$(chroot "$ROOT" /usr/bin/dpkg --print-architecture)" = "$ARCH" ] || die "已有 Ubuntu 的架构与宿主机不一致"

# Keep original configuration once, including the previous manually installed setup.
mkdir -p "$SETUP/config-before-deploy"
for file in /etc/sysupgrade.conf /etc/schroot/chroot.d/ubuntu.conf /usr/bin/xwrt-shell /usr/bin/ubuntu-root; do
	if [ -f "$file" ] && [ ! -f "$SETUP/config-before-deploy/$(basename "$file")" ]; then
		cp -p "$file" "$SETUP/config-before-deploy/$(basename "$file")"
	fi
done
mkdir -p /etc/schroot/chroot.d /etc/schroot/xwrt "$ROOT/tmp/shm" "$ROOT/dev/pts" "$ROOT/dev/shm"
chmod 1777 "$ROOT/tmp" "$ROOT/tmp/shm"
cat > /etc/schroot/chroot.d/ubuntu.conf <<EOF
[ubuntu]
description=Ubuntu 24.04 LTS for X-Wrt builds
type=directory
directory=$ROOT
profile=xwrt
users=root
root-users=root
aliases=ubuntu-24.04,noble
shell=/bin/bash
preserve-environment=false
EOF
cat > /etc/schroot/xwrt/fstab <<'EOF'
/proc    /proc     none   rw,bind  0 0
/sys     /sys      none   ro,bind  0 0
/dev     /dev      none   rw,bind  0 0
/dev/pts /dev/pts  none   rw,bind  0 0
/tmp/shm /tmp/shm  none   rw,bind  0 0
EOF
# OpenWrt's /dev/shm -> /tmp/shm must resolve INSIDE the chroot too.
mkdir -p /tmp/shm
chmod 1777 /tmp/shm
printf '%s\n' /etc/resolv.conf > /etc/schroot/xwrt/copyfiles
# Preserve Ubuntu's users instead of copying OpenWrt's passwd/shadow databases.
: > /etc/schroot/xwrt/nssdatabases
chmod 644 /etc/schroot/chroot.d/ubuntu.conf /etc/schroot/xwrt/*
cat > "$ROOT/usr/sbin/policy-rc.d" <<'EOF'
#!/bin/sh
exit 101
EOF
chmod 755 "$ROOT/usr/sbin/policy-rc.d"
run /etc/init.d/schroot enable
run /etc/init.d/schroot start

if [ "$SWAP_MB" -gt 0 ]; then
	if [ -e "$SWAP" ]; then
		[ -f "$SWAP" ] || die "$SWAP 必须是普通文件"
		[ ! -L "$SWAP" ] || die "$SWAP 不能是软链接"
		# Reuse existing swap without resizing or formatting it. swapon rejects
		# non-swap files, so an existing unrelated file is never overwritten.
		log "保留已有交换文件：$SWAP（不调整大小）"
	else
		[ ! -e "$SWAP.part" ] || die "$SWAP.part 是上次未完成的交换文件；确认后手动删除再重试"
		free_kb=$(df -Pk "$PARENT" | awk 'NR == 2 {print $4}')
		[ "$free_kb" -ge "$((SWAP_MB * 1024 + 512 * 1024))" ] || die "磁盘不足以创建 Swap"
		log "创建 ${SWAP_MB} MiB 交换文件：$SWAP"
		(umask 077; run dd if=/dev/zero of="$SWAP.part" bs=1048576 count="$SWAP_MB")
		run mkswap "$SWAP.part"
		mv "$SWAP.part" "$SWAP"
	fi
	cat > /etc/init.d/ubuntu-build-swap <<EOF
#!/bin/sh /etc/rc.common
START=99
STOP=15
start() {
    [ -f '$SWAP' ] || return 1
    awk -v path='$SWAP' '\$1 == path {found=1} END {exit !found}' /proc/swaps && return 0
    /usr/sbin/swapon '$SWAP'
}
stop() {
    awk -v path='$SWAP' '\$1 == path {found=1} END {exit !found}' /proc/swaps || return 0
    /usr/sbin/swapoff '$SWAP'
}
EOF
	chmod 755 /etc/init.d/ubuntu-build-swap
	run /etc/init.d/ubuntu-build-swap enable
	run /etc/init.d/ubuntu-build-swap start
fi

cat > /usr/bin/xwrt-shell <<'EOF'
#!/bin/sh
if [ "$#" -eq 0 ]; then
    exec schroot -c ubuntu -u root -d / -- /bin/su --login xwrt
fi
exec schroot -c ubuntu -u root -d /home/xwrt -- /usr/sbin/runuser -u xwrt -- /bin/bash -lc 'cd /home/xwrt; exec "$@"' xwrt-shell "$@"
EOF
cat > /usr/bin/ubuntu-root <<'EOF'
#!/bin/sh
if [ "$#" -eq 0 ]; then
    exec schroot -c ubuntu -u root -d / -- /bin/bash -l
fi
exec schroot -c ubuntu -u root -d / -- "$@"
EOF
chmod 755 /usr/bin/xwrt-shell /usr/bin/ubuntu-root
touch /etc/sysupgrade.conf
for file in /etc/schroot/chroot.d/ubuntu.conf /etc/schroot/xwrt/ /etc/init.d/schroot /etc/init.d/ubuntu-build-swap /etc/rc.d/S94schroot /etc/rc.d/S99ubuntu-build-swap /etc/rc.d/K15ubuntu-build-swap /usr/bin/xwrt-shell /usr/bin/ubuntu-root; do
	[ -e "$file" ] || [ -L "$file" ] || continue
	grep -qxF "$file" /etc/sysupgrade.conf || printf '%s\n' "$file" >> /etc/sysupgrade.conf
done

if [ "$REPAIR" = 0 ]; then
	cat > "$SETUP/install-build-packages.sh" <<'UBUNTU'
#!/bin/bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive TZ=Asia/Shanghai LC_ALL=C
umask 022
cat > /etc/apt/apt.conf.d/80-build-retries <<'EOF'
Acquire::Retries "3";
Acquire::http::Timeout "30";
Acquire::https::Timeout "30";
EOF
apt-get update
apt-get -y upgrade
extra=()
if [ "$(dpkg --print-architecture)" = amd64 ]; then extra=(gcc-multilib g++-multilib); fi
apt-get install -y --no-install-recommends \
  build-essential clang llvm lld "${extra[@]}" \
  autoconf automake libtool libtool-bin cmake ninja-build meson \
  flex bison gawk gettext gettext-base gperf help2man texinfo \
  git subversion rsync wget curl ca-certificates openssh-client \
  libncurses-dev libssl-dev libelf-dev liblzma-dev libzstd-dev \
  zlib1g-dev libgmp-dev libmpc-dev libmpfr-dev libfl-dev libbsd-dev \
  libfdt-dev libfuse-dev libunistring-dev libyaml-dev uuid-dev \
  python3 python3-dev python3-setuptools python3-pip python3-venv \
  python-is-python3 python3-yaml python3-pyelftools swig \
  bc ccache file patch diffutils findutils grep sed tar gzip bzip2 \
  xz-utils zstd zip unzip p7zip-full cpio bsdextrautils util-linux \
  perl pkg-config time device-tree-compiler dosfstools mtools \
  u-boot-tools genisoimage locales tzdata sudo vim-tiny nano less \
  tmux screen procps iproute2 iputils-ping dnsutils jq
# Switch the official Base image sources to HTTPS after installing CA roots.
find /etc/apt/sources.list.d -name '*.sources' -exec sed -i \
  's|http://archive.ubuntu.com|https://archive.ubuntu.com|g; s|http://security.ubuntu.com|https://security.ubuntu.com|g; s|http://ports.ubuntu.com|https://ports.ubuntu.com|g' {} +
locale-gen en_US.UTF-8 zh_CN.UTF-8
update-locale LANG=en_US.UTF-8
ln -snf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime
printf '%s\n' Asia/Shanghai > /etc/timezone
if ! id xwrt >/dev/null 2>&1; then
  # Prefer the UID used in the original deployment; don't change existing users.
  if getent passwd 1001 >/dev/null; then
    useradd --create-home --user-group --shell /bin/bash xwrt
  else
    useradd --create-home --uid 1001 --user-group --shell /bin/bash xwrt
  fi
fi
[ "$(id -u xwrt)" != 0 ] || { echo 'xwrt must not be root' >&2; exit 1; }
[ "$(getent passwd xwrt | cut -d: -f6)" = /home/xwrt ] || { echo 'Unexpected xwrt home directory' >&2; exit 1; }
cat > /etc/profile.d/xwrt-build.sh <<'EOF'
export LANG=en_US.UTF-8
umask 022
EOF
chmod 644 /etc/profile.d/xwrt-build.sh
apt-get clean
[ -z "$(dpkg --audit)" ]
apt-get check
UBUNTU
	chmod 700 "$SETUP/install-build-packages.sh"
	log "安装 Ubuntu 编译依赖；详细进度：tail -f $LOG"
	run schroot -c ubuntu -u root -d / -- /bin/bash -s < "$SETUP/install-build-packages.sh"
fi

log "验证普通用户、编译器、Python、挂载和 HTTPS"
run ubuntu-root /bin/bash -s <<'CHECK_ROOT'
set -euo pipefail
test "$(id -u xwrt)" -ne 0
test -z "$(dpkg --audit)"
CHECK_ROOT
run xwrt-shell /bin/bash -s <<'CHECK'
set -euo pipefail
[ "$(id -u)" != 0 ]
[ "$HOME" = /home/xwrt ]
[ "$(umask)" = 0022 ]
test -r /proc/cpuinfo
test -c /dev/null
test -d /dev/shm
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
printf 'int main(void) { return 0; }\n' > "$work/test.c"
gcc "$work/test.c" -o "$work/test-c"
"$work/test-c"
printf '#include <iostream>\nint main() { std::cout << 42; }\n' > "$work/test.cc"
g++ "$work/test.cc" -o "$work/test-cpp"
[ "$("$work/test-cpp")" = 42 ]
# Native i386 execution is deliberately not required: X-Wrt kernels may lack it.
python3 -c 'import ssl, setuptools, yaml, elftools'
clang -print-targets | grep bpf >/dev/null
curl -fsSI --connect-timeout 20 --max-time 60 https://github.com/ >/dev/null
printf 'Compiler, Python, BPF and HTTPS checks passed\n'
CHECK

if [ "$SOURCE" = 1 ]; then
	log "检出 X-Wrt（保留已有工作目录），运行宿主编译依赖检查"
	run xwrt-shell /bin/bash -s <<'SOURCE_CHECK'
set -euo pipefail
cd /home/xwrt
if [ ! -e x-wrt ]; then git clone --depth=1 https://github.com/x-wrt/x-wrt.git x-wrt; fi
[ -f x-wrt/include/prereq-build.mk ] || { echo 'Existing x-wrt directory is not a build tree' >&2; exit 1; }
cd x-wrt
# Full `make prereq` opens menuconfig without .config; test host dependencies only.
mkdir -p tmp
make -r -s -f include/prereq-build.mk TOPDIR="$PWD" HOST_OS=Linux prereq
printf 'XWRT_HOST_PREREQ_PASSED\n'
SOURCE_CHECK
fi

run /etc/init.d/schroot enabled
if sysupgrade -l > "$SETUP/upgrade-backup-files.txt" 2>> "$LOG"; then
	cat "$SETUP/upgrade-backup-files.txt" >> "$LOG"
else
	die "无法读取固件升级保留清单；日志：$LOG"
fi
for file in /etc/schroot/chroot.d/ubuntu.conf /etc/schroot/xwrt/fstab /etc/schroot/xwrt/copyfiles /etc/schroot/xwrt/nssdatabases /etc/init.d/schroot /usr/bin/xwrt-shell /usr/bin/ubuntu-root; do
	grep -qxF "$file" "$SETUP/upgrade-backup-files.txt" || die "升级保留清单缺少 $file"
done
if [ "$SWAP_MB" -gt 0 ]; then
	grep -qxF /etc/init.d/ubuntu-build-swap "$SETUP/upgrade-backup-files.txt" || die "Swap 启动脚本未进入升级保留清单"
fi
cat > "$SETUP/README.txt" <<EOF
Ubuntu 24.04 schroot for X-Wrt
Root: $ROOT
Build user: xwrt, /home/xwrt
Enter: xwrt-shell
Administration: ubuntu-root
Log: $LOG
Swap: $SWAP (requested size: $SWAP_MB MiB; existing files are reused)

Source: /home/xwrt/x-wrt (if source checkout was enabled)
Before building, run as xwrt:
  cd ~/x-wrt
  ./scripts/feeds update -a
  ./scripts/feeds install -a
  make menuconfig
  make download -j1
  make -j1 V=s

After a configuration-preserving firmware upgrade, keep the data partition.
Run this deployment script again with --repair if schroot is missing.
The backup preserves scripts/configuration, not installed host binaries.
No automatic firmware upgrade or partition-table change is performed.
Low-memory hosts should start with make -j1.
EOF
printf '%s\n' "$(date '+%F %T') $ARCH $ROOT" > "$SETUP/deployment-ok"
log "部署完成。编译环境：xwrt-shell；管理环境：ubuntu-root"
log "升级后修复：使用相同 --root、--swap-mb 参数，加 --repair；说明：$SETUP/README.txt"
df -h "$PARENT"
