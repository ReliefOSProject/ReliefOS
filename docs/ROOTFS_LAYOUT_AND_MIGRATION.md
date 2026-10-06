# ReliefOS Alpine 风格 rootfs 约定

本项目采用 Alpine 的非 usr-merge 目录结构，不复制宿主 Arch/CachyOS 的
usr-merge 链接。按维护者决定，不支持从旧目录布局自动迁移；旧镜像和旧路径
应用需要全新安装或重新构建。本文文件名保留便于已有文档引用，运行时没有迁移流程。

## 布局来源与实现真源

目录基线为 [Alpine baselayout 3.7.2 的 APKBUILD](https://gitlab.alpinelinux.org/alpine/aports/-/blob/master/main/alpine-baselayout/APKBUILD)。
共享目录、模式和符号链接表在 `kernel/reliefnt/include/uapi/reliefos/rootfs.h`，由 C 安装器直接展开。
应用路径常量在 `include/reliefos/layout.h` 及 SDK 镜像头文件中。主机侧
staging helper 从 `tools/reliefos_layout.py` 消费共享表；旧导入路径
`tools/leonos_layout.py` 仅转发到规范模块。
修改目录清单应修改共享 UAPI 表，不能在各镜像脚本另写列表。

`/bin`、`/sbin`、`/lib`、`/usr/bin`、`/usr/sbin`、`/usr/lib` 均为真实目录。
不生成 `/lib64`、`/usr/lib64`。标准链接为：

```text
/var/run                 -> ../run
/var/lock                -> ../run/lock
/var/spool/mail          -> ../mail
/var/spool/cron/crontabs  -> ../../../etc/crontabs
/etc/mtab                -> ../proc/mounts
```

BusyBox 在 `/bin/busybox`，按实际编译配置生成 applet 链接，分别放入上游指定的
`/bin`、`/sbin`、`/usr/bin`、`/usr/sbin`。独立打包的同名命令优先，禁用 applet
会清理上次构建登记的链接。`/bin/sh` 和 `/bin/ash` 使用 BusyBox ash。

## 目录、权限与生命周期

所有镜像载荷默认 root:root。标准目录为 0755，以下特殊模式来自共享表。

| 路径 | 用途与约束 |
| --- | --- |
| `/boot` | FAT32 ESP 挂载点；当前内核 `/boot/reliefos/kernel.sys`，旧 `/boot/leonos` 载荷用于识别与回滚 |
| `/dev`、`/dev/pts` | 设备和 PTY 命名空间由内核提供，设备不以普通文件模拟 |
| `/dev/shm` | 真实可写目录，1777；每次启动在创建用户进程前清空 |
| `/etc` | passwd/group、hosts、hostname、shells、profile、网络数据库等 |
| `/etc/reliefos` | ReliefOS 专有持久配置；旧品牌配置由迁移逻辑处理 |
| `/etc/profile.d`、`/etc/opt` | shell 配置与第三方套件配置位置 |
| `/home/<name>` | 用户家目录，0700，属主为用户 UID/GID |
| `/root` | root 家目录，0700 |
| `/proc` | 内核只读进程信息；mounts 内容从实际就绪卷表生成 |
| `/sys` | 预留挂载点；目前没有 sysfs 实现 |
| `/run`、`/run/lock`、`/run/reliefos` | 每次启动先清空，再以 root:root 0755 创建；冻结的 `/run/leonos/*.sock` IPC 值由兼容链接承接 |
| `/tmp`、`/var/tmp` | 1777 + sticky；本次没有增加启动清理，`/var/tmp` 保持持久 |
| `/usr/local/{bin,sbin,include,lib,share}` | 本机管理员软件位置 |
| `/usr/include`、`/usr/src`、`/usr/share/man` | 头文件、源码、手册位置，不代表已填入完整开发包 |
| `/usr/lib/reliefos/apps/<id>` | 应用包、manifest、图标和私有资源 |
| `/usr/lib/reliefos/drivers` | Ring-0 模块 |
| `/usr/lib/reliefos/tests` | 来宾探针 |
| `/usr/share/reliefos/resources` | ReliefOS 桌面资源 |
| `/usr/share/fonts/reliefos`、`/usr/share/doc/reliefos` | 字体与帮助 |
| `/usr/share/licenses` | 许可证和源码归属 |
| `/var/lib/reliefos` | 持久状态，0750；users.db 为 0600 |
| `/var/cache`、`/var/log`、`/var/local`、`/var/opt` | 缓存、日志及可变数据 |
| `/var/spool`、`/var/mail` | 队列和邮件位置 |
| `/var/empty` | 空目录，0555 |
| `/opt/{dyne,python,tcc,lua}` | 自包含工具套件，命令入口在 `/usr/bin` |
| `/media/{cdrom,floppy,usb}`、`/mnt`、`/srv` | 介质挂载、临时挂载和服务数据位置 |

`/run` 和 `/dev/shm` 当前由 ext2 承载，**没有伪称 tmpfs**。启动清理不递归调用，
不跟随符号链接，不越过卷边界；目录枚举/删除失败时阻止启动用户进程并记录错误。
清理在正常系统与安装器首次 spawn 之前完成。它提供启动期隔离，不提供 tmpfs 的
纯内存存储、容量统计或掉电后不可恢复特性。`/sys` 只是 FHS 挂载点，不提供设备树。

`/proc/mounts`、`/proc/self/mounts` 和存活 PID 的 mounts 从存储卷表读取，按读偏移
输出，转义空格、制表符、换行和反斜杠；不把 `/run` 或 `/dev/shm` 列成虚构挂载。
`/proc/mounts` 是指向 `self/mounts` 的符号链接；`/proc/self` 指向调用者 TGID。
`/proc/<pid>/mountinfo` 输出挂载 ID、父挂载、与 stat 一致的设备号、根、挂载点、
文件系统及来源；根卷/ESP 使用实际 GPT UUID，RAM 根保留 ramdisk 来源。
尚无独立 mount namespace、绑定挂载和传播属性。

## 基础配置与账户

`system/rootfs/` 是随构建发布的基础文件。`os-release` 明确标识 ReliefOS，
不冒充 Alpine 发行版。Live/installer 的 `fstab` 不填写虚构的块设备；磁盘镜像和
全新安装写入真实 `/dev/disk/by-partuuid/<UUID>` 的 ext2 根与 vfat ESP 两项。
同布局更新保留已有 fstab。引导层仍只挂载根与 ESP，尚无读取任意 fstab 条目的启动服务。
不生成全零 machine-id。默认 PATH 包含 `/usr/local/sbin`、`/usr/local/bin`，
以及标准 `/usr/sbin`、`/usr/bin`、`/sbin`、`/bin`。

passwd/group 提供 root、nobody 和 authd 管理的用户身份；普通新用户 UID 从 1000 起，
root/nobody 名称保留。基础账户使用锁定口令字段；ReliefOS 登录仍由 authd/AUS1
负责，没有声称实现 shadow/PAM 登录。authd 发布 passwd/group 使用独占临时文件、
fsync 和 rename，启动时即导出系统身份，避免 OOBE 前 root 查询为空。

`/etc/protocols`、`/etc/services` 使用 Alpine baselayout 选择的
[Debian netbase v6.4](https://salsa.debian.org/md/netbase/-/tree/v6.4) 原文件，导入时
核对 Alpine APKBUILD 的 SHA-512；GPL-2 许可证、版权和来源随镜像发布。

## 构建、动态链接和安装

rootfs staging 在 images 管线中先于 installer staging 完成（`$(ROOTFS_RAW_STAMP)`
是 installer stage 的前置），所有写入 staging 的生产步骤先等待目录骨架。
链接阶段最后统一处理标准链接和命令入口。普通磁盘、Live 与安装器使用同一张
目录表、相同的 ext2 生成器。`fakeroot` 只在子进程中为 mke2fs 提供 root:root
视图，不改变宿主工作区文件所有者；硬链接、符号链接及模式保持原样。
构建依赖包含 `fakeroot` 和 `e2fsprogs`，CI 安装清单同步更新。

所有动态程序的 PT_INTERP 仍为 `/lib/ld-musl-x86_64.so.1`。
`/etc/ld-musl-x86_64.path` 搜索 ReliefOS 私有库目录 `/usr/lib/reliefos` 与兼容目录
`/usr/lib/leonos`，再搜索 `/lib:/usr/lib`。规范扩展库为
`/usr/lib/reliefos/libreliefos.so.2`，`libleonos.so.2` 保留为 ABI 兼容对象名；
冻结的 ELF note 值 `libleonos.so.1` 保留在协议定义中；发行物不提供该旧别名或旧 native
加载器。glibc 的加载器路径常量仅用于识别与诊断，不打包 glibc。

安装器的 `/install/root` 复制到 `/target`，`/install/esp` 复制到 `/target/boot`。
更新仅接受现行非 usr-merge ext2 结构与新的 ESP 路径；不迁移 `/system`、
`/programs`、`/users`，不扫描或归档旧用户数据，不自动改写旧 API 包目的路径。

同布局更新保留已有配置与持久数据，遵守可选应用选择及其命令入口。
普通文件/符号链接均先创建同目录临时对象再替换；根载荷写完后更新 ESP，
最后写 loader。已存在的目录组件必须是真实目录，不能经符号链接逃出目标树。
这不是整个系统更新的原子事务，也没有承诺设备断电后的完整恢复。

## 先前目录迁移的静态自审（2026-09-11）

已修复的源码问题还包括：载荷单元素 tuple 导致的 staging 越界删除风险、
ncurses 删除共享 usr/bin、未生成声明的 stamp、安装器过早写 users.db、
Python launcher 排除真实来宾路径长度、ESP 缺 display.conf、目标文件被先截断、
更新时忽略可选应用选择，以及本轮目录清单漂移、宿主 UID/GID 泄漏和旧状态重用。

该阶段按当时约定，只修改代码并自行静态审查。已做 Python AST 解析、相关 C 的
clang `-fsyntax-only` 与差异空白检查；没有运行回归测试，没有编译/重打包镜像，
没有启动 QEMU 或 VMware。已有 ISO 和预编译 SDK 仍是之前的产物，必须重新构建。
这份文档不声明 Linux ABI 已完整兼容，也不将 sysfs/tmpfs 或 Alpine 软件服务视为已实现。

## 常用 Linux 文件接口补全（2026-09-11）

- `/dev/disk/by-partuuid` 的 lookup、readdir、readlink 从经校验的 GPT 缓存生成。
  GPT 写入/重读会使缓存失效。UUID 使用 UEFI 混合字节序；重复身份返回 ENOTUNIQ，
  不任意选择磁盘；磁盘 I/O 错误继续上报。安装器与宿主镜像工具读取同一真实标识。
- proc 路径跟随接入内核权限解析器；`self`、`mounts`、每 PID 的 `exe/cwd/root`
  具有符号链接类型。跨进程 exe/cwd/root 沿用 FSCREDS/dumpable 检查。
  `exe/cwd` 仍使用任务路径字符串，尚不具备 Linux magic-link 的 inode 引用、
  rename/unlink 后跟踪、已删除文件重开等完整语义；没有 chroot 或进程私有根。
- `/proc/filesystems` 列出实际存在的后端，未列 tmpfs/sysfs。目录存在不代表
  `mount -t proc/devfs`、`mount -a` 的所有 flags 和挂载目标都已支持。
- 启动在 spawn 之前读取 `/etc/hostname`；`uname`、`sethostname`、`setdomainname`
  共享短锁保护的 UTS 状态。setter 按 native int 长度与 CAP_SYS_ADMIN 检查；
  uname 检查输出可写。hostname 配置无效或读取失败会记录错误并保留默认主机名，
  不阻止桌面启动。新增只读 `/proc/sys/kernel/{hostname,domainname,ostype,osrelease,version}`。
  sysctl 写入、UTS namespace 尚未实现。默认 NIS domain 为 `(none)`。
- 移除返回全零的非标准 `/proc/machine-id`。尚未提供持久机器身份文件；不存在的
  身份不能伪装成所有系统相同的有效 ID。
- stat 正确报告 synthetic symlink、block 类型及 proc/devfs 设备号，statx 同步
  major/minor。尚未全面修复块设备 rdev、所有字符设备号与挂载设备身份的 Linux 关系。

验证入口：宿主 rootfs 测试已并入 `make test`（C 单元测试与 `tests/build/`
Shell check 契约）。宿主 ASan/UBSan 执行真实
mount 渲染、procfs、UTS、权限与安装器 GPT 读取代码；覆盖短读、转义、目录类型、
坏参数、重复 UUID 和 I/O 失败。保留任务管理器消费者回归；两处旧测试接线按已经完成的
musl 迁移修正（声明 fixture 函数，forkpty 检查指向实际 musl provider）。
这些是宿主模块回归；来宾验证另见下表。

实际验证记录：

| 层次 | 结果 |
| --- | --- |
| `build.py run test-linux-rootfs` | 9 个宿主测试通过，含实际权限解析器跨 ext2/proc 链接跟随；`build/rootfs-host-tests.log` |
| `build.py run kernel` | 0 errors；`build/rootfs-kernel-build.log` |
| `build.py run userland` | 0 errors，包含 installer 与 blockdev 消费者；`build/rootfs-userland-build.log` |
| ISO、QEMU/KVM | `build/linux-inventory/leonos4-fastfetch.iso`；Fastfetch 原始二进制直接执行及经 Terminal 执行均通过；`build/linux-inventory/guest-serial.log` |
| VMware | 未运行 |

依据：本地固定 Linux v6.12 `fs/proc_namespace.c` 的 mounts/mountinfo 字段，
`kernel/sys.c` 的 sethostname/setdomainname，以及 musl `src/misc/mntent.c` 的
fstab/mntent 解析。Alpine 风格描述目录与配置惯例，并非宣称运行 Alpine 内核或 OpenRC。


## Fastfetch 硬件接口验证（2026-09-11）

内核在 CPU bring-up 时缓存 CPUID 拓扑和品牌信息，并通过 `/proc/cpuinfo` 输出 Linux 字段；SMBIOS type 0/1/2/3 字符串和 UUID 经过边界检查后提供给 `/sys/class/dmi/id`。PCI 配置空间设备从真实总线枚举，提供 vendor/device/class、revision、subsystem、modalias、config 和 uevent；固件/native framebuffer 的当前分辨率通过只读 graphics/DRM sysfs 节点导出。没有硬件来源的温度、刷新率、EDID、驱动绑定和写控制接口保持未知。

`/proc/<pid>/stat` 采用 Linux 52 字段顺序，comm 按最后一个右括号解析；`cmdline` 从 exec 后的实际用户栈 argv/env 范围按 NUL 字节读取，进程地址字段遵守 PTRACE_MODE_READ_FSCREDS/dumpable 检查。原有任务管理器已改读标准字段。

`python3 tools/test_linux_inventory.py --guest` 会将 `/home/xiaobai/下载/fastfetch-musl` 原样放入 ext2 Live ISO，在 QEMU/KVM 中检查直接执行和真实 Terminal/Shell 执行的 JSON。该二进制 SHA256 为 `2ef1384e8eca57e7f163851f1989286b6d2b4638170eed85465cc1ecc604d47f`。QEMU 配置两个 vCPU，但当前 scheduler 实际启用一个；断言使用内核报告的实际数量。此项不等价于 VMware 或完整 Linux sysfs/DRM ABI 认证。
