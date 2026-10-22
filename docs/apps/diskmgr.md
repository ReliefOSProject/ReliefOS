# 磁盘管理器（Diskmgr）

运行 `diskmgr`，或从系统菜单打开磁盘管理器。程序使用 C++、FLTK 和 X11，
沿用系统 FLTK 主题、中文字体及现有磁盘管理器图标。左侧选择磁盘，右侧选择
分区；工具栏与“分区”菜单提供同一组操作。Ctrl+R 刷新，Ctrl+Q 关闭。

支持查看 GPT 分区、文件系统、容量、PARTUUID 和实际挂载位置；在空闲空间
新建并格式化分区，格式化、删除分区，以及挂载和卸载数据分区。新文件系统
可选 ext4、FAT32、exFAT，也可挂载现有 ext2 分区。现有格式化器要求 FAT32
至少32 MiB，ext4/exFAT至少128 MiB；较小分区不能使用对应格式化器。分区名称支持中文，磁盘上
使用 GPT 规定的 UTF-16，界面使用 UTF-8；超过36个 UTF-16编码单元的名称被拒绝。
界面输入另有71字节上限，分区表显示超出列宽时可调整列宽或水平滚动。

普通用户默认只读；“以管理员身份打开…”使用标准 sudo 验证当前用户密码，
成功后打开 root 窗口。需要该用户已有 sudo 权限；密码通过标准输入传送，
不进入命令行或环境变量。取消或验证失败保留原窗口。

新建、格式化和删除需要输入对话框给出的 CREATE、FORMAT、DELETE，连续点击
两次确认。修改任意字段会取消第一次确认。系统分区和已挂载分区受到保护；
含挂载分区的整盘不能更改 GPT。BIOS Boot 分区不能格式化或挂载。
扫描和磁盘操作在独立子进程执行，期间窗口继续处理 X11 事件，操作完成后
刷新列表；操作进行中不能关闭窗口。新建成功但格式化失败会明确报告部分成功。

使用现有 POSIX 文件操作、Linux 块设备 ioctl 和 mount/umount2，支持512字节
逻辑扇区和有效 GPT；没有引入 windowd 接口。没有 GPT 或分区表损坏的磁盘
仅报告错误，需先用独立工具准备 GPT。挂载点为 `/mnt/disk<编号>p<分区号>`，
已有挂载、符号链接、权限不安全或非空目录会被拒绝。

写入前重新检查设备节点、GPT 头、分区 GUID、范围、只读及挂载状态。运行中的
设备热插拔仍不在 ReliefNT 支持范围内：相同节点和完全相同 GPT 头的媒体替换
无法可靠区分，使用时应保持磁盘连接。普通文件描述符读写按文件权限验证，
重读分区表需要 Linux `CAP_SYS_ADMIN`，与上游
[Linux 块设备 ioctl 实现](https://github.com/torvalds/linux/blob/master/block/ioctl.c)一致。

## 构建和验证

```sh
TMPDIR="$PWD/out/tmp" sh tests/build/test-diskmgr-fltk.sh
TMPDIR="$PWD/out/tmp" python3 tools/test_installer_partitions.py
TMPDIR="$PWD/out/tmp" make O=out/dualboot-build -j4 app-diskmgr image-vmdk
```

`tools/test_diskmgr_qemu.py --template <已安装的测试镜像> --out <新目录>`
创建模板的独立副本和独立测试盘，注入当前构建、编译两个来宾测试辅助程序，
登录真实 XDM/IceWM，再通过标准输入接收 JSON 操作命令。模板需包含标准测试
账户；这与直接启动默认出厂 VMDK 是不同的验收方式。操作记录、构建哈希、
串口日志、GPT 和截图保存在输出目录；模板保持原样。

VMware 验证未完成；物理硬件运行未验证。

2026-10-22 验证：目标应用和 VMDK 构建通过，RAW 与 VMDK 独立比较一致；
镜像内核、完整根分区、应用、运行库及 desktop 文件均已核对。主机 ASan/UBSan
模型、异步管道、GPT 中文名称转换、生产 I/O 函数回归和安装器 GPT 回归通过。
QEMU 中完成 FAT32 挂载、文件读写及卸载；新建128 MiB ext4并通过独立 e2fsck，
挂载写入后再次 fsck 和文件回读通过；重新格式化 FAT32、删除及保留原分区身份
验证通过。64 MiB ext4 不受现有格式化器支持，部分创建失败提示已经实际验证。
内核 root 写入、O_PATH 写入拒绝、撤销 CAP_SYS_ADMIN 后拒绝重读分区表均通过。
仓库总测试仍存在 soundctl APK 归属不一致，内核总测试仍有既有驱动 ABI golden
168→296 差异；没有重生成 golden，也没有修改这些基线策略。

同一次 QEMU 实测还覆盖920×600最小窗口，以及真实普通用户登录、块设备写入
DAC 拒绝、只读窗口、提权对话框取消、错误密码失败后保留原窗口、正确 sudo
密码启动管理员窗口。
