# ReliefOS 高级安装教程

同一 Installer ISO 支持 Legacy BIOS 和 UEFI。选择 `Install ReliefOS (console session)`
后进入 root Bash shell，
运行 `installer` 使用正式安装器。GUI/TTY 全新安装默认启用 Xorg，并同时安装 BIOS、
UEFI 引导程序，两项可以独立取消。全部取消的新盘需要另行配置引导。更新保留既有
桌面类型和跳过的引导程序，不重新分区。

手动复制不会执行账户事务，也不会创建安装完成标记。需要受支持的 root、普通账户和
sudo 策略时，请使用正式安装器；账户规则见 [安装器账户](INSTALLER_ACCOUNTS.md)。
以下步骤面向修复和开发，要求逻辑扇区为 512 字节。

## 检查源和磁盘

```sh
ls /install/root /install/root-console /install/esp
lsblk
blkid
fdisk -l
```

下列命令会清除目标磁盘数据。`/dev/sda` 只是例子，必须根据容量、控制器核对；
NVMe 整盘为 `/dev/nvme0n1`，分区为 `/dev/nvme0n1p1` 等。不要选择安装介质。

## 创建 GPT

默认布局如下；手动布局可以使用不同编号，后续命令必须对应实际分区。

| 分区 | GPT 类型 | 文件系统 | 用途 |
| --- | --- | --- | --- |
| 1 | BIOS Boot | 不格式化 | 1 MiB GRUB BIOS 嵌入空间 |
| 2 | EFI System | FAT32 | 至少 128 MiB，共用引导文件和内核 |
| 3 | Linux filesystem | ext4 | 其余空间，系统及用户数据 |

通过上游 `sfdisk` 创建布局。这会替换 GPT，不能用来更新需保留数据的系统。

```sh
sfdisk /dev/sda <<'TABLE'
label: gpt
unit: sectors

start=2048,size=2048,type=21686148-6449-6E6F-744E-656564454649,name=RELIEFOS_BIOS
start=4096,size=262144,type=U,name=RELIEFOS_ESP
type=L,name=RELIEFOS_ROOT
TABLE
sync
sfdisk --verify /dev/sda
lsblk /dev/sda
```

BIOS Boot 分区不得格式化、挂载或用于存放普通文件。

## 格式化、挂载、复制

```sh
mkfs.fat -F 32 -n RELIEFOS /dev/sda2
mkfs.ext4 -F -L RELIEFOS_ROOT /dev/sda3
fsck.fat -n /dev/sda2
fsck.ext4 -f -n /dev/sda3
mkdir -p /mnt/root /mnt/esp
mount -t ext4 /dev/sda3 /mnt/root
mount -t vfat /dev/sda2 /mnt/esp
cp -a /install/root/. /mnt/root/
```

纯控制台系统改为复制 `/install/root-console/.`。`cp -a` 保留符号链接、权限和
时间戳。不要格式化整盘、已挂载分区；`mkfs.fat32` 别名仍需 `-F 32`。
不要占用普通安装器使用的 `/target` 路径。

## 配置 fstab 和引导身份

```sh
ROOT_PARTUUID="$(blkid -s PARTUUID -o value /dev/sda3)"
ESP_PARTUUID="$(blkid -s PARTUUID -o value /dev/sda2)"
printf 'root=%s\nesp=%s\n' "$ROOT_PARTUUID" "$ESP_PARTUUID"
```

确认两值均为非空且唯一的 UUID，再写入：

```sh
cat > /mnt/root/etc/fstab <<EOF_FSTAB
/dev/disk/by-partuuid/$ROOT_PARTUUID / ext4 defaults 0 1
/dev/disk/by-partuuid/$ESP_PARTUUID /boot vfat defaults 0 2
EOF_FSTAB
reliefos-boot-install --source /install/esp --boot-directory /mnt/esp \
    --disk /dev/sda --root-partuuid "$ROOT_PARTUUID" --bios=yes --uefi=yes --check
reliefos-boot-install --source /install/esp --boot-directory /mnt/esp \
    --disk /dev/sda --root-partuuid "$ROOT_PARTUUID" --bios=yes --uefi=yes
```

`--check` 只预检；`--bios=no`、`--uefi=no` 分别跳过对应引导程序。
`--update` 保留既有回滚内核和已检测到的引导模式。BIOS 调用 GNU
`grub-install --target=i386-pc`；UEFI 发布 `EFI/BOOT/BOOTX64.EFI`，不注册 NVRAM。
共用的 `root.cfg` 使用 Linux `root=PARTUUID=…` 绑定根分区，避免多盘同名文件选错盘。
完整事务涉及原始磁盘写入，不能保证断电原子性；工具失败时应检查报告的阶段。
只有前置步骤成功，才发布新菜单。

兼容命令 `leonos-grub-installer` 是 UEFI-only 更新薄包装。旧的两分区 UEFI 盘可以
跳过 BIOS 后原地更新；缺少专用 BIOS Boot 分区时，请求添加 BIOS 会在发布前拒绝。
不要为更新盘直接执行上面的重新分区命令。

## 结束

```sh
sync
cd /
umount /mnt/esp
umount /mnt/root
reboot
```

移除 ISO 或在固件菜单选择目标盘。预检失败时核对整盘路径、只读状态、安装源是否
位于目标盘、ESP 挂载归属、GPT 类型和根 UUID。固件必须能读取引导设备；内核支持
NVMe 并不表示所有 Legacy BIOS 固件都带有 NVMe 引导驱动。

介质中的英文文本版可通过 `less /root/ADVANCED_INSTALL.txt` 阅读。
