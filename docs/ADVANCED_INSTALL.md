# ReliefOS 高级安装教程

Installer ISO 的 TTY 模式直接进入 root Ash shell，允许使用标准 Linux 命令手动
完成 GPT 分区、格式化、检查、挂载、系统复制和 UEFI 启动文件安装。它不会自动
启动安装器或选择磁盘；如需使用 TTY 安装器，可在 shell 中运行 `installer`。

当前介质提供上游 util-linux 2.41.6、e2fsprogs 1.47.3、dosfstools 4.2、
exfatprogs 1.4.3 和 BusyBox 1.36.1。本文使用这些工具的上游命令行，不再使用
旧版私有 `fdisk`/`mkfs` 参数。存储操作通过 `/dev/*`、Linux block
ioctl、标准文件 I/O 和 `mount(2)`/`umount2(2)` 完成。

> [!WARNING]
> 手动复制 payload 不会调用安装器的账户事务，也不会创建安装完成标记。
> 它适合底层安装、修复和开发；若需要由 ReliefOS 正式支持的 root/普通账户、
> wheel/sudo 策略和可直接登录的系统，请使用普通安装器。账户规则参见
> [安装器账户与组件](INSTALLER_ACCOUNTS.md)。

安装介质中另有一份可在 TTY shell 直接阅读的英文纯文本版本：

```sh
less /root/ADVANCED_INSTALL.txt
```

## 推荐磁盘布局

| 分区 | 文件系统 | GPT 类型 | 建议名称 | 用途 |
| --- | --- | --- | --- | --- |
| 1 | FAT32 | EFI System | `LeonOS 4 ESP` | UEFI、GRUB、loader 和内核 |
| 2 | ext4 | Linux filesystem | `LEONOS4_ROOT` | ReliefOS 根文件系统 |

ESP 建议至少 128 MiB。根分区应使用剩余空间，并确保能容纳 `/install/root`
及后续用户数据。分区名称只是便于识别，不参与启动；GPT 类型和文件系统才是
必要条件。

## 重要警告

- `g`、`d`、`w`、`mkfs.*` 会破坏目标磁盘上的数据。
- 根据容量和控制器信息确认目标磁盘；不要把安装 ISO 或其他数据盘当成目标盘。
- ReliefOS 磁盘命名为 `/dev/sda`、`/dev/sda1`；NVMe 设备使用
  `/dev/nvme0n1`、`/dev/nvme0n1p1` 这类 Linux 名称。
- 以下示例假定目标是 `/dev/sda`，实际编号不同时必须替换所有相关命令。
- Installer ISO 根目录是可写的临时 ext4 ramdisk，但重启后其中的修改会丢失；
  已写入目标磁盘的内容会保留。

## 1. 进入 TTY 模式并检查 payload

从 Installer ISO 的 GRUB 菜单选择：

```text
Install ReliefOS (TTY mode)
```

进入 shell 后，可以运行自动 TTY 安装器：

```sh
installer
```

如需手动安装，则直接检查安装源：

```sh
ls /install/root
ls /install/esp
```

TTY shell 以 root 身份运行，默认 `PATH` 已包含 `/usr/sbin`、`/usr/bin`、
`/sbin` 和 `/bin`。

## 2. 识别目标磁盘

```sh
lsblk
blkid
fdisk -l
```

也可以只查看候选盘：

```sh
fdisk -l /dev/sda
```

空白磁盘没有有效分区表属于正常情况。新版 util-linux `fdisk` 可以直接创建
GPT，不需要先运行 `gptinit`。

## 3. 使用 fdisk 创建 GPT 和分区

启动分区工具：

```sh
fdisk /dev/sda
```

依次输入以下内容。空行表示接受默认值：

```text
g
n
1

+128M
n
2


t
1
1
p
```

此时 `p` 应显示：

- 分区 1：约 128 MiB，类型为 `EFI System`；
- 分区 2：占用其余可用空间，类型为 `Linux filesystem`。

分区名是可选项。需要命名时，在写盘前进入 expert 菜单：

```text
x
n
1
LeonOS 4 ESP
n
2
LEONOS4_ROOT
r
```

再次输入 `p` 检查结果，确认无误后输入 `w` 写入 GPT 并退出。若任何内容有误，
输入 `q` 可不保存退出。

写入后确认内核已刷新分区节点：

```sh
sync
lsblk /dev/sda
fdisk -l /dev/sda
```

## 4. 格式化 ESP 和 ext4 根分区

```sh
mkfs.fat -F 32 -n LEONOS4ESP /dev/sda1
mkfs.ext4 -F -L LEONOS4ROOT /dev/sda2
```

`mkfs.fat32` 和 `mkfs.vfat` 只是指向上游 `mkfs.fat` 的兼容链接，不会自动添加
参数。即使使用这些名称，也必须显式指定 `-F 32`：

```sh
mkfs.fat32 -F 32 /dev/sda1
```

不要把整盘 `/dev/sda` 传给格式化工具，也不要格式化已挂载的分区。

## 5. 检查文件系统和标识

新建文件系统后可以做一次只读检查：

```sh
fsck.fat -n /dev/sda1
fsck.ext4 -f -n /dev/sda2
blkid /dev/sda1 /dev/sda2
```

`fsck.fat32`/`fsck.vfat` 是 `fsck.fat` 的别名。通用 `fsck`、`blkid` 和
`lsblk` 来自 util-linux；各文件系统检查器来自对应的上游文件系统项目。

## 6. 挂载目标文件系统

```sh
mkdir -p /mnt/root /mnt/esp
mount -t ext4 /dev/sda2 /mnt/root
mount -t vfat /dev/sda1 /mnt/esp
mount
```

也可以省略 `-t` 让系统检测文件系统。高级模式下不要把手动分区挂载到 `/dev`、
`/target` 或 `/target/boot`，这些路径属于系统和普通安装器。

## 7. 复制系统并保留元数据

必须复制 `/install/root` 的内容，而不是再创建一层 `root` 目录。使用 `cp -a`
保留符号链接、权限和时间戳：

```sh
cp -a /install/root/. /mnt/root/
```

检查核心文件：

```sh
ls -l /mnt/root/usr/lib/reliefos/apps/desktop/desktop.elf
ls -l /mnt/root/lib/ld-musl-x86_64.so.1
```

不要用不保留元数据的普通递归复制替代 `cp -a`，否则 set-ID 程序、目录权限或
符号链接可能损坏。

## 8. 写入 `/etc/fstab`

当前正式安装器使用 GPT PARTUUID，而不是可能重复的文件系统 label。先读取两项
PARTUUID：

```sh
ROOT_PARTUUID="$(blkid -s PARTUUID -o value /dev/sda2)"
ESP_PARTUUID="$(blkid -s PARTUUID -o value /dev/sda1)"
printf 'root=%s\nesp=%s\n' "$ROOT_PARTUUID" "$ESP_PARTUUID"
```

两项都必须为非空 UUID。确认后写入目标系统：

```sh
cat > /mnt/root/etc/fstab <<EOF
# <source> <mountpoint> <type> <options> <dump> <pass>
/dev/disk/by-partuuid/$ROOT_PARTUUID / ext4 defaults 0 1
/dev/disk/by-partuuid/$ESP_PARTUUID /boot vfat defaults 0 2
EOF
```

检查最终内容：

```sh
cat /mnt/root/etc/fstab
```

## 9. 安装 ReliefOS UEFI/GRUB payload

确认 ESP 的实际挂载点是 `/mnt/esp`：

```sh
mount
leonos-grub-installer /mnt/esp
```

该工具复制已经构建好的 ReliefOS 启动 payload，并不是 GNU `grub-install`。它会
验证并复制：

- `EFI/BOOT/BOOTX64.EFI`；
- `reliefos/kernel.sys`；
- `leonos/kernel.sys`（旧布局回滚入口，已存在则保留）；
- 完整的 `grub/` 配置、字体和主题。

工具不会格式化或挂载 ESP，也不会写入固件 NVRAM。它安装标准 UEFI fallback
路径；固件没有自动识别时，在固件菜单选择目标盘的
`EFI/BOOT/BOOTX64.EFI`。

## 10. 同步、卸载和重启

```sh
sync
cd /
umount /mnt/esp
umount /mnt/root
reboot
```

根分区应最后卸载。若 `umount` 返回 busy，确保 shell 不在挂载点内，并关闭仍在
访问目标文件的程序。重启前移除 Installer ISO，或在固件菜单中选择目标磁盘。

## 可选：使用 exFAT 根分区

ext4 是当前新安装默认值。ext2 仅用于兼容旧镜像。确需 exFAT 时，分区 2 的 GPT 类型应改为
`Microsoft basic data`，并替换以下命令：

```sh
mkfs.exfat -L LEONOS4ROOT /dev/sda2
fsck.exfat -n /dev/sda2
mount -t exfat /dev/sda2 /mnt/root
```

`/etc/fstab` 中根分区一行的类型也要从 `ext4` 改为 `exfat`。其余复制、ESP、
启动文件和卸载步骤不变。

## 高级模式可用的存储工具

| 命令 | 来源 | 用途 |
| --- | --- | --- |
| `fdisk`, `sfdisk` | util-linux | GPT 查看、创建和修改 |
| `lsblk`, `blkid` | util-linux | 块设备、文件系统和 UUID 查询 |
| `mount`, `umount` | util-linux | 标准挂载和卸载命令 |
| `mkfs.ext4`, `fsck.ext4` | e2fsprogs | ext4 创建和检查 |
| `mkfs.fat`, `fsck.fat` | dosfstools | FAT32 创建和检查 |
| `mkfs.exfat`, `fsck.exfat` | exfatprogs | exFAT 创建和检查 |
| `sync`, `cp`, `mkdir`, `cat`, `less` | BusyBox | 文件复制、同步和教程阅读 |
| `leonos-grub-installer` | ReliefOS | 复制预构建 UEFI/GRUB payload |

## 常见失败原因

- `fdisk` 看不到磁盘：先用 `lsblk` 确认设备路径；高级模式示例中的目标盘不一定
  总是 `/dev/sda`。
- `fdisk` 报没有有效分区表：进入目标盘后用 `g` 创建 GPT；不再需要先运行
  `gptinit`。
- `mkfs.fat` 得到的不是 FAT32：必须提供 `-F 32`。
- `mkfs.*` 报设备忙：确认分区未挂载，并且传入的是 `/dev/sdXN` 或
  `/dev/nvmeXnYpZ` 分区节点。
- `mount` 失败：确认挂载点存在、文件系统类型正确，并检查 `blkid` 输出。
- `blkid -s PARTUUID` 输出为空：不要写入 fstab；返回 `fdisk -l` 检查 GPT，并
  确认分区节点已刷新。
- `leonos-grub-installer` 报缺少文件：确认 `/install/esp` 完整、ESP 已挂载且
  目标不是普通未挂载目录。
- `umount` 报 busy：先执行 `cd /`，关闭占用该挂载点的进程，再重试。
- 启动后没有可登录账户：这是手动 payload 复制的已知边界；使用普通安装器创建
  受支持的 root 和普通账户配置。
