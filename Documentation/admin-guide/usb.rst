===================
USB U 盘与键盘鼠标
===================

控制器与设备
============

默认内核内置 EHCI 和 UHCI，支持 USB 2.0 高速 BOT/SCSI 存储与 boot HID
键盘、相对鼠标。VMware 中选择“USB Controller / USB Compatibility /
USB 2.0”；使用 USB 3.x/xHCI 控制器的端口不属于这次 EHCI 支持范围。
USB 的 Linux 块设备与 evdev 接口由内核提供，不需要修改 BusyBox。

存储当前要求 512 字节逻辑扇区，支持 GPT、MBR/扩展分区和直接在整个设备上
创建的文件系统。没有介质的读卡器、更换介质需重新拔插。不是所有 USB
协议都受支持，例如 UAS、等时音视频和非 boot HID 设备；文件系统类型还
必须有相应内核实现。

手工挂载
========

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

``make defconfig`` 默认将驱动内置。选择 ``CONFIG_USB_EHCI_HCD=m`` 后，
镜像携带对应 ``ehci.ko`` 和 PCI alias，由现有模块冷插拔策略加载。
低速/全速根端口键鼠还需要 UHCI companion；相应设置和其他模块管理步骤
见 :doc:`kernel-modules`。有挂载或打开的 USB 块设备时，卸载 EHCI 会返回忙，
应先卸载文件系统、关闭使用者，不强制卸载。

集成验证
========

``tools/test_usb2_qemu.py`` 消费 ReliefNT 的原生输入/移除探针，在关闭 PS/2
控制器的 QEMU 中检查 USB 键盘登录、鼠标事件、U 盘读写、写保护、分区布局、
拔插后旧 FD 的错误和可选模块装卸。使用镜像的只读快照与新建的测试 U 盘，
不接管宿主机物理 USB。运行结果和镜像 SHA-256 写到调用者指定的全新输出
目录，不能把 QEMU 的 ``-vga vmware`` 当成真实 VMware 验证。

VMware 的 USB 兼容性和可移动设备操作见
`Workstation Pro 官方手册
<https://techdocs2-prod.adobecqms.net/content/dam/broadcom/techdocs/us/en/pdf/vmware/desktop-hypervisors/workstation/vmware-workstation-pro-17-0.pdf>`_。
