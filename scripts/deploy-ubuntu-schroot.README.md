# X-Wrt Ubuntu 24.04 编译环境部署

`deploy-ubuntu-schroot.sh` 把 Ubuntu 24.04 schroot、X-Wrt 编译依赖、普通用户和
Swap 部署到持久磁盘，不改分区表、不刷固件、不清空已有目录。

## 条件

- X-Wrt/OpenWrt 宿主机，root 权限，支持 `apk` 或 `opkg`。
- `x86_64` 或 `aarch64`；Ubuntu 分别使用 amd64 或 arm64 镜像。
- `/data` 已挂载到持久磁盘，且已配置开机自动挂载。
- 首次部署至少留出 8 GiB 空间（包括默认 4 GiB Swap）；实际固件编译还需更多空间。
- 可以访问 Ubuntu 软件源、Ubuntu Base 下载站和 GitHub。

实机部署验证使用 x86_64；arm64 分支尚未做实机验证。

## 在服务器本机执行

```sh
sh deploy-ubuntu-schroot.sh
```

默认行为：

- 安装宿主机 `schroot`、`block-mount`、GNU cp/stat、curl 和 CA 证书，包管理器自动安装依赖。
- 将固定版本的 Ubuntu 24.04.5 Base 解包到 `/data/ubuntu`，下载文件必须通过固定 SHA256 校验。
- 创建 `/data/ubuntu.swap`，默认 4096 MiB，并配置开机启用。
- 安装 GCC/G++、Clang/LLVM、Python、CMake、开发库及常用编译工具。
- 创建普通用户 `xwrt`，使用 `/home/xwrt`，默认优先使用 UID 1001，密码锁定。
- 配置 UTF-8 locale、上海时区和 `umask 022`。
- 检出 X-Wrt 到 `/home/xwrt/x-wrt`，已有工作目录不会被重置或更新。
- 验证 C/C++ 编译、Python 模块、LLVM BPF、HTTPS 和 X-Wrt 宿主依赖。

镜像校验值来自 Ubuntu 官方 [SHA256SUMS](https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/SHA256SUMS)。
该清单的签名已使用 Ubuntu CD Image 签名密钥
`843938DF228D22F7B3742BC0D94AA3F0EFE21092` 验证。脚本使用内置 SHA256，
不会因换下载镜像站而跳过校验，也不会自动换成未经校验的新版基础镜像。

## 通过 SSH 部署

在本仓库目录执行。先传完整脚本再执行，服务器会将脚本副本保存到持久磁盘：

```sh
ssh root@SERVER 'cat > /tmp/deploy-ubuntu-schroot.sh && sh /tmp/deploy-ubuntu-schroot.sh' \
  < scripts/deploy-ubuntu-schroot.sh
```

批量部署时，把地址替换为自己的服务器列表。任意一台失败即停止：

```sh
for host in root@SERVER1 root@SERVER2 root@SERVER3; do
  ssh "$host" 'cat > /tmp/deploy-ubuntu-schroot.sh && sh /tmp/deploy-ubuntu-schroot.sh' \
    < scripts/deploy-ubuntu-schroot.sh || break
done
```

需要无人值守执行时，先配置 SSH 密钥认证，再给 SSH 添加 `-o BatchMode=yes`。
日志保存在 `/data/ubuntu-setup/deploy-*.log`，可用另一 SSH 连接查看实时进度。

## 参数

```sh
sh deploy-ubuntu-schroot.sh --help
sh deploy-ubuntu-schroot.sh --root /data/ubuntu --swap-mb 4096
sh deploy-ubuntu-schroot.sh --no-source
sh deploy-ubuntu-schroot.sh --swap-mb 0
```

- `--root PATH`：修改根目录；修改后重跑时需继续使用同一路径。父目录必须已存在。
- `--swap-mb MB`：只在交换文件不存在时创建，已有文件不会被重新格式化或调整大小。
- `--swap-mb 0`：不创建、关闭或修改已有 Swap。btrfs 需要使用此选项，自行配置 Swap。
- `--no-source`：只安装环境，不下载源码。
- `--mirror HTTPS_URL`：指定 Ubuntu Base 下载目录，镜像名和 SHA256 仍固定。
- `--repair`：固件升级后恢复宿主依赖、配置、入口和 Swap，不运行 Ubuntu apt，不更新源码。

脚本可重复运行。完整重跑会重新检查 Ubuntu 软件包并安装可用更新；
修复模式适用于已完整部署的环境。遇到其他系统、错误架构或已有的冲突配置会停止，
不会删除目录重新安装。基础系统解包中断后可以重跑；Swap 创建中断留下的
`/data/ubuntu.swap.part` 需要确认后手动删除再重试。

## 使用环境

```sh
xwrt-shell
cd ~/x-wrt
./scripts/feeds update -a
./scripts/feeds install -a
make menuconfig
make download -j1
make -j1 V=s
```

脚本不选择固件目标。1 GiB 内存的主机先使用 `-j1`。
部分 X-Wrt 内核不能运行原生 i386 程序，因此验证使用 64 位宿主程序；
这不要求启用宿主内核的 IA32 兼容功能。

宿主 root 可以用以下命令管理 Ubuntu 或以普通用户执行命令：

```sh
ubuntu-root
ubuntu-root apt-get update
xwrt-shell gcc --version
xwrt-shell bash -lc 'cd ~/x-wrt && git status --short'
```

## 固件升级后恢复

脚本把入口、schroot 配置和启动脚本加入 `/etc/sysupgrade.conf`。
Ubuntu 本体位于独立的数据磁盘，升级时必须保留数据分区和挂载配置。
“保留配置”不会自动保留新固件未包含的 schroot 二进制和依赖。

通过文件方式部署时，可直接运行保存在数据磁盘中的副本：

```sh
sh /data/ubuntu-setup/deploy-ubuntu-schroot.sh --repair
```

自定义过根目录、Swap 参数的主机，修复时传入同样参数。
如果升级后 `/data` 未挂载，先恢复数据盘挂载，再运行修复。
不要用 `sysupgrade -p` 或整盘重刷来升级需要保留数据分区的服务器。

## 脚本检查

在开发机运行：

```sh
shellcheck -s sh scripts/deploy-ubuntu-schroot.sh
busybox ash -n scripts/deploy-ubuntu-schroot.sh
python3 scripts/tests/deploy-ubuntu-schroot.py -v
```

保护测试需要 bubblewrap 和可用的非特权用户命名空间，在隔离的挂载环境中运行，
检查已有目录不被覆盖、冲突配置不被替换、临时文件系统和错误参数被拒绝等行为。
