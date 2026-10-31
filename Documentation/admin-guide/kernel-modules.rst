==================
内核模块的系统管理
==================

ReliefNT 的可加载模块采用 Linux 风格的 Kconfig、Kbuild、ELF ``.ko`` 和
模块管理命令。模块必须针对正在使用的 ReliefNT 内核重新编译；这不表示
Linux 原版二进制模块或其全部内核内部 API 可直接使用。POSIX 不定义
内核模块格式，用户态系统调用遵循已实现的 Linux x86_64 ABI。

构建配置
========

从 ReliefOS 根目录执行::

    mkdir -p out/tmp
    export TMPDIR="$PWD/out/tmp"
    make O=out/modules defconfig
    make O=out/modules kernel-menuconfig
    make O=out/modules -j8 image-vmdk iso installer

``defconfig`` 明确重置 OS 和对应内核的配置，默认生产组件均为内置 ``y``。
在内核菜单中可将有独立生命周期的部件改为 ``m``；调度、内存、模块加载器
和 VFS 等启动核心保持内置。``kernel-allmodconfig`` 将可模块化部件选为 m；
``kernel-olddefconfig`` 同步已有内核配置。普通构建不会把 m 偷改回 y。
关闭 ``CONFIG_MODULES`` 时仍可生成全内置系统。

模块与 initramfs 消费内核实际 install manifest。镜像打包检查根控制器、
根文件系统及其依赖闭包，拒绝缺失必需功能、模块错 ABI 或成对载荷不一致，
不会为让构建成功而静默修改配置。产物位置沿用 :doc:`../kbuild/building`。

启动与自动加载
==============

GRUB 同时加载内核和薄 newc initramfs。``/init`` 挂载 proc/sys/dev，使用
真实 BusyBox modprobe 加载根控制器和文件系统，再根据 ``root=`` 挂载真实根
并 switch_root。Live/Installer 的大根镜像是独立的内存块设备，不能替代
initramfs。普通 OpenRC 模块服务先于音频、网络和 Xorg；PCI 设备通过 sysfs
modalias 选择模块，平台模块来自当前内核版本的生成清单。

管理员可使用 ``/etc/modules-load.d/*.conf`` 和 ``/etc/modprobe.d/*.conf``。
同名配置的覆盖、屏蔽、blacklist 和参数由实际模块工具与加载策略处理。
不要把旧内核版本的生成平台清单复制成所有版本共享的静态配置。

查询与装卸
==========

在目标 ReliefOS 系统内执行::

    uname -r
    lsmod
    modinfo module_name
    sudo modprobe module_name
    sudo modprobe -r module_name

模块安装在 ``/lib/modules/<uname -r>/``。modprobe 读取目标兼容 depmod
生成的依赖、别名和内置数据库；内置模块的 modprobe 是有效的空操作。
修改内核模块需要内核检查的 ``CAP_SYS_MODULE``，仅在 UI 中获得管理员
身份不能绕过该检查。错误版本、未解析符号和损坏 ELF 必须明确失败。

活跃挂载、打开的设备、用户映射、IRQ、DMA 和在途回调会保留模块 owner。
卸载忙碌的根磁盘、文件系统、DRM 或 PCM 实现被拒绝属于正常行为。
不能为了让卸载命令返回成功而释放仍在使用的代码。

更新与恢复
==========

签名内核包及 RPR 的完整事务见 :doc:`packages`。A/B 选择记录绑定完整
模块 ABI、内核 SHA-256 和 initramfs SHA-256；版本目录还包含成对清单。
首次安装的残缺零分配或同一候选版本记录可重试，不明旧记录不会被覆盖。

故障模型包括任一步写入/同步失败、未同步数据或目录条目丢失，以及非当前
选择记录撕裂。同步成功的数据被设备丢弃、任意 FAT 元数据损坏，以及更新
MBR/EFI 固件加载器本体，超出日常内核包 A/B 发布的保证范围。
QEMU、VMware 和真实硬件的运行证据分别记录，不能互相替代。

固件选择器的回归测试使用独立、无可执行内核的 ISO，在真实 BIOS 和 UEFI
GRUB 中检查摘要参数、成对载荷和回退选择::

    TMPDIR="$PWD/out/tmp" python3 tools/test_kernel_slots_qemu.py \
        --stage out/x86_64/release --output out/grub-slot-check

先在同一 ``--stage`` 构建 ``grub-host grub-efi``，输出目录必须尚不存在。
GRUB 的本地补丁为上游 ``hashsum`` 增加单文件 ``--expected`` 比对；选择器
独立验证内核和 initramfs 的实际字节，避免把缺项校验清单当作载荷验证。
升级内核包不替换固件加载器；更新此选择器时，必须同时安装同版 BIOS/EFI
GRUB 模块，不能仅复制新的配置文件到旧版 GRUB。

内核内部机制、模块开发和构建 SDK 由 ReliefNT 手册持有；本页只说明
ReliefOS 配置、包管理及启动策略。
