===================
USB U 盘与键盘鼠标
===================

控制器与设备
============

默认内核内置 xHCI、EHCI 和 UHCI。xHCI 支持根端口或 Hub 后的 USB 3.x
SuperSpeed BOT/SCSI 存储，以及 USB 2.0/1.x BOT 存储、boot HID 键盘和相对
鼠标。EHCI 支持 USB 2.0 高速 BOT/SCSI 存储；低速/全速键鼠可由 UHCI
companion 提供。VMware 中可选择“USB Controller / USB Compatibility / USB
3.x”使用 xHCI，或选择 USB 2.0 使用 EHCI；两种路径的验证结果分别记录。
USB 的 Linux 块设备与 evdev 接口由内核提供，不需要修改 BusyBox。

xHCI 支持 USB2／USB3 Hub、多级路由和下游设备热插拔；USB2 的 multi-TT
Hub 使用默认 single-TT 模式。拔掉 Hub 时，下游设备一起断开，旧打开不会
转向重新插入的设备。UAS、stream、等时音视频及非 boot HID 不受支持。
USB4、Type-C/PD 和物理电气认证也不在支持范围内。SuperSpeedPlus
可按已验证的控制器协议能力识别速率，但日志中的 ``bps`` 是控制器声明的
链路速率，不是实测吞吐量；10/20 Gb/s 物理链路尚未验证。

接管固件配置的 xHCI 时，驱动只在必要寄存器几何推导出的边界内访问扩展
能力，不对正在使用的 BAR 做破坏性尺寸探测。这依赖固件对必要寄存器和
BAR 范围的描述真实；无法确认边界内的能力会被拒绝。

存储当前要求 512 字节逻辑扇区，支持 GPT、MBR/扩展分区和直接在整个设备上
创建的文件系统。没有介质的读卡器、更换介质需重新拔插。不是所有 USB
协议都受支持，例如 UAS、等时音视频和非 boot HID 设备；文件系统类型还
必须有相应内核实现。

Fileman 桌面操作
================

Fileman 左侧的“设备”列表自动发现可移动存储卷，后台约每两秒刷新一次。
点击卷名或“打开”后挂载并进入文件夹，已挂载卷前显示星号。支持已有驱动可读的
FAT32、exFAT、ext2 和 ext4。分区介质与整盘文件系统均可使用。当前存储驱动的
逻辑扇区要求仍为 512 字节。

挂载点使用 ``/run/media/<数字 UID>/<设备名>-<diskseq>``，由 root 创建。
普通用户通过现有 sudo / ReliefAskpass 密码窗口授权，需要相应 sudo 权限。
FAT32/exFAT 使用 Linux ``uid/gid/fmask/dmask`` 挂载选项，为发起用户提供读写权限。
ext 文件系统保留介质自身的 POSIX 所有权与模式。挂载同时启用 ``nosuid,nodev``。
设备身份通过 sysfs 和 ``BLKGETDISKSEQ`` 复核。拔插后需要重新选择新介质。

拔出前选择卷并点击“弹出”。Fileman 离开当前设备目录，然后后台卸载该设备的
各个挂载点并同步设备缓存。成功提示“现在可以安全拔出设备”后，可拔出 U 盘或
在 VMware 中断开对应 USB 设备。物理供电由设备连接控制。
系统挂载受到保护。文件、工作目录、映射或裸块设备使用者造成占用时，界面报告
失败，并在可枚举时列出相关进程 PID。关闭使用者后可以重试。部分卷已卸载而
后续卷失败时，设备列表按实际挂载状态刷新，仍需完成弹出后再拔出。

设备发现、挂载和卸载均使用独立子进程，Fileman 窗口继续处理事件。
正在执行设备操作时，关闭窗口会提示等待完成。

命令行挂载
==========

设备使用普通 ``/dev/sdX`` 名称，分区带数字后缀；编号取决于机器原有磁盘，
不可假定所有 U 盘都是 ``sdb``。先用 ``lsblk`` 查看设备、容量和文件系统，
确认目标后以 root 执行。重新连接后名称可能改变，应再次确认设备。
下面的 ``sdX1`` 是占位符，必须换成实际分区：

.. code-block:: sh

   lsblk
   mkdir -p /mnt/usb
   mount -t vfat /dev/sdX1 /mnt/usb
   ls /mnt/usb
   sync
   umount /mnt/usb

ext4/exFAT 介质分别使用 ``-t ext4`` 或 ``-t exfat``；整个设备上直接创建的
文件系统使用 ``/dev/sdX``。写保护介质使用 ``mount -o ro``。卸载完成后再
断开 VMware 可移动设备或拔出 U 盘；突然拔出后的旧打开返回 ``ENODEV``，
重新插入不会把旧句柄转向新设备。

如果设备无法证明写入已落盘，并且拒绝缓存同步命令，``umount`` 会报告错误。
应保留设备连接并检查日志；不能把文件能够读回当成安全断电的证明。
此类设备重新连接后，可使用 ``mount -o ro`` 读取；从未尝试写入的设备无需
提交本驱动的写缓存，读取后的卸载不会被不支持的同步命令阻断。

使用模块配置时
==============

``make defconfig`` 默认将驱动内置。选择 ``CONFIG_USB_XHCI_HCD=m`` 或
``CONFIG_USB_EHCI_HCD=m`` 后，镜像分别携带 ``xhci.ko`` 或 ``ehci.ko`` 和
对应 PCI alias，由现有模块冷插拔策略加载。EHCI 的低速/全速根端口键鼠还
需要 UHCI companion；相应设置和其他模块管理步骤见 :doc:`kernel-modules`。
USB 设备的使用者持有对应控制器模块；有挂载或打开的 USB 块设备时卸载
该控制器会返回忙。应先卸载文件系统、关闭使用者，不强制卸载。重新连接
后的 ``/dev/sdX`` 名称可能变化，旧打开不会被重新绑定。

集成验证
========

``tools/test_usb2_qemu.py`` 消费 ReliefNT 的原生输入/移除探针，在关闭 PS/2
控制器的 QEMU 中检查 USB 键盘登录、相对鼠标精确事件、U 盘读写和写保护、
MBR/GPT/整盘 FAT、拔插后旧 FD 的 ``ENODEV``、新设备名及可选模块装卸。
离线核对写入字节；镜像使用只读快照，测试 U 盘与输出目录新建，不接管宿主机
物理 USB。探针哈希、内核 release、固件、控制器、QMP 设备树及每个设备的
端口与协商速率记录在输出中；xHCI 检查要求每个 U 盘实际协商 ``SUPER``，
不能仅凭设备连接到 xHCI 或其他设备的速率判定成功。

默认仍是 ``--topology highspeed --firmware bios``；``companion`` 保留 EHCI /
UHCI 行为。选择 ``qemu-xhci`` 或 ``nec-usb-xhci`` 时，各配置八个 USB2 和
八个 USB3 根端口；``--hid-usb-version 1`` 和 ``2`` 分别检查 FULL 和 HIGH
键鼠，默认使用 ``2``。``--modules`` 检查所选控制器模块，需使用对应模块化
镜像。xHCI 测试要求独立探针身份 JSON 中的 ``probe_sha256`` 与实际探针
一致；默认查找探针同目录下的 ``probe-identity.json``，也可用
``--probe-identity`` 指定。必须为每行选择全新的输出目录：

.. code-block:: sh

   python3 tools/test_usb2_qemu.py --image /path/to/reliefos.raw \
     --manifest /path/to/install.bundle.json --probe /path/to/usb-input-probe \
     --output /path/to/new-evidence --topology qemu-xhci --hid-usb-version 2

   python3 tools/test_usb2_qemu.py --image /path/to/reliefos.raw \
     --manifest /path/to/install.bundle.json --probe /path/to/usb-input-probe \
     --output /path/to/another-new-evidence --topology nec-usb-xhci \
     --firmware uefi --hid-usb-version 1

UEFI 使用 ``QEMU_FIRMWARE`` 指定的 OVMF 镜像，默认
``/usr/share/edk2/x64/OVMF.4m.fd``，通过 ``-bios`` 只读取共享固件文件。
测试设置 ``opt/ovmf/X-PciMmio64Mb=0``，使 NVMe 64 位 BAR 放入低 MMIO
窗口，与 ``tools/test_dual_boot_qemu.py`` 的低 MMIO 模式一致。BIOS 使用
SeaBIOS 的对应低地址设置。结果文件记录原始镜像运行前后的 SHA-256；
只有 guest 检查、离线字节检查和镜像不变均通过，整行才通过。

完整验收矩阵包括 builtin 的 qemu-xhci/BIOS/HIGH HID、
nec-usb-xhci/UEFI/FULL HID，模块化 qemu-xhci/BIOS/FULL HID，以及原有
EHCI/UHCI companion 回归。各行是否运行和通过以其 ``results.json`` 为准。
真实 VMware xHCI 曾观察到虚拟 SuperSpeed 存储及 FULL 键鼠枚举，但当前集成
验证未完成，不能据此声称存储、输入和缓存同步全部成功；以往 VMware USB2
失败仍按原结果记录。物理 USB 直通和实体硬件尚未验证。QEMU 的
``-vga vmware`` 也不代表真实 VMware 验证。

``tools/test_xhci_hub_qemu.py`` 另行验证两级全速 Hub 后的 U 盘、USB 键盘
登录和相对鼠标、单独 U 盘拔插、整棵 Hub 树移除／重建，以及可选
``--modules`` 的挂载阻止卸载和卸载后重新枚举。使用 console backend 的测试
副本，避免 XDM 在 TTY 键盘验证期间切换焦点。该脚本同样消费匹配镜像的
``--manifest``、内核仓原生 ``--probe``，并要求新建 ``--output``：

.. code-block:: sh

   python3 tools/test_xhci_hub_qemu.py --image /path/to/reliefos.raw \
     --manifest /path/to/install.bundle.json --probe /path/to/usb-input-probe \
     --output /path/to/new-hub-evidence --controller qemu-xhci

QEMU 的 ``usb-hub`` 只支持全速，不能用它证明高速 Hub 的 TT 或 USB3 Hub
物理链路。内核支持边界和硬件协议验证归属 ReliefNT 文档；Hub 的 VMware
验证未完成，物理 USB 直通和实体 Hub 也尚未验证。

VMware 的 USB 兼容性和可移动设备操作见
`Workstation Pro 官方手册
<https://techdocs2-prod.adobecqms.net/content/dam/broadcom/techdocs/us/en/pdf/vmware/desktop-hypervisors/workstation/vmware-workstation-pro-17-0.pdf>`_。
