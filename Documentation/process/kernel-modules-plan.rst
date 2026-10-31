==========================
内核模块的系统集成实施计划
==========================

状态：设计于 2026-10-24，系统集成功能实施与分层验收已完成。以下记录交付范围，
编译、打包和来宾验证分别说明；既有失败与未验证范围继续保留。父仓调查基线为
``9f7ab3c11869056ac9ad9192e1c3ae682b4d57ec``。
内核机制和 K1-K8 阶段由 ReliefNT 仓库
``Documentation/process/module-support-plan.rst`` 定义；本计划只持有
ReliefOS 配置、用户态、启动载荷、安装更新及验收编排。

用户已确认 .ko 采用 Linux 风格机制，但针对 ReliefNT 重新编译。
当前已授权实现；建立分支、提交或推送须另获明确授权。实施使用
executing-plans；允许委派时使用用户指定的 GPT-6.1-Terra / Max。

系统行为契约
============

* ``make O=... defconfig`` 明确重置 OS 配置，并对同一输出对应的内核
  ``RELIEFNT_SUBBUILD_O`` 执行 defconfig；默认生产组件均为 y。
* ``make kernel-menuconfig``、``kernel-olddefconfig``、``kernel-allmodconfig``
  委派到实际内核输出。OS menuconfig 继续管理 OS 产品；不复制或导入内核
  功能符号到主仓 Kconfig，也不把应用配置放进内核。
* 普通 make/all/kernel/image、自动 config-sync 和缺文件修复不能重置用户
  的 m 设置。首次无配置时允许初始化默认配置，已有配置只同步新增选项。
* 镜像适配消费内核 install manifest，不扫描源码猜模块，也不在主仓再编译
  内核对象。原 kernel.sys/debug、镜像输出位置保持，新增模块树与 initramfs。
* 同一镜像中的内核、模块、模块数据库和 initramfs 必须属于同一 ABI 身份。
  配置遗漏根设备驱动/FS、缺模块、错误版本须在打包阶段失败，不能交给启动试错。

推荐启动链
==========

.. code-block:: text

   BIOS / UEFI / GRUB
     -> kernel.sys + initramfs.cpio
     -> 内核内置的调度/内存/VFS/模块加载器
     -> initramfs /init（PID 1）
     -> modprobe 根磁盘与文件系统模块及其依赖
     -> 挂载 root= 指定的真实根，switch_root
     -> /sbin/init / OpenRC / 设备模块自动加载
     -> XDM / Xorg / IceWM，或纯控制台

初版采用外置、未压缩 newc 档案，减少必须内置的解压依赖；压缩作为独立后续
功能，不提供当前无法使用的压缩选项。OS 镜像统一带一个小 initramfs，即使
生产组件全内置也使用同一系统启动策略；内核自身仍可支持独立无 initramfs 启动。

早期用户态使用现有 musl/BusyBox，包含 /init、mount/switch_root/modprobe、
最小设备节点和当前内核启动依赖。普通镜像的初始根不包含完整桌面。
安装/live 载荷仍是独立的大型文件系统模块：GRUB 同时传递 initramfs 和原有
安装根载荷，早期程序加载 FS 后从内存块设备挂载。不可把 ext4 镜像简单改名
成 initramfs，或要求尚未可见的根分区提供读取它自己必需的驱动。

可移植发行镜像打包所选存储驱动和支持的根 FS 的模块依赖闭包；不能只根据
构建宿主机的硬件选择启动模块。root=PARTUUID 等根身份和 BIOS/UEFI loader
选择保持现有契约，不能随磁盘枚举顺序改变目标。

工具与安装布局
==============

沿用现有 BusyBox 的 insmod、rmmod、lsmod、modinfo、modprobe、depmod。
设计调查时启用了 MODPROBE_SMALL，alias/symbol 配置关闭；实现已切换为
完整 modprobe/depmod 配置，验收覆盖依赖、别名、blacklist、参数与内置识别。
不新造一套同名私有工具，也不假定现有命令“存在”就已经能加载模块。

目标目录为 ``/lib/modules/<KERNELRELEASE>/``，数据库由目标兼容 depmod
离线生成；宿主 depmod 可读相同产物作为交叉检查，不假定宿主版本相同。
宿主工具必须指定私有根，例如 ``depmod -b <staging> <KERNELRELEASE>``，
不得重写宿主的模块目录。还须检查 APK/BusyBox 的命令归属，避免分发时被
另一个同名程序覆盖；最终镜像中的实际工具也要完成同一套读取和依赖测试。
静态配置遵循 ``/etc/modules-load.d/*.conf``、``/etc/modprobe.d/*.conf``；
设备 coldplug 根据 sysfs modalias 加载。内核别名/依赖来自编译元数据，不在
OS 脚本里维护第二份 PCI ID 或依赖表。

早期依赖在 /init 中加载；普通设备模块服务必须早于音频、网络和 Xorg。
不能靠固定 sleep 等待模块完成，完成条件应是 modprobe 返回与对应设备出现。
用户手动操作 root 磁盘、挂载中的 FS、活跃 DRM/PCM 等模块时，安全拒绝卸载
属于预期行为；不能为“所有命令成功”而让系统继续使用被释放的代码。

构建、安装和更新事务
====================

内核 install 扩展 manifest，至少公布 KERNELRELEASE、模块 ABI 身份、
模块路径/依赖/校验和、内置元数据、配置与 symvers。父仓构建图增加模块树
和 initramfs 的完整前置依赖，m-y-n 切换、模块删除或配置变化均触发重建。
清理严格受产物 manifest 约束，不删除用户手工安装的模块和其他内核版本。

内核 release 使用唯一 ABI 后缀，OS VERSION_ID 保持发行数字版本。
模块安装在版本目录，更新时可同时保留当前和新内核模块。
GRUB 仍提供既有公开入口；其版本入口精确绑定 kernel/initramfs 成对文件。
旧版本入口引用自己的内核、initramfs 和模块树。

父仓预生成可移植 initramfs，与相应内核和所选模块共同装入内核 APK/安装
载荷。Installer、普通系统更新和 RPR 使用同一套载荷校验/发布接口：先完整
写入新版本模块、kernel、initramfs，校验并同步，再发布版本选择记录。
不能先删当前内核模块目录，也不能让旧内核指向新模块。
更换 BIOS/UEFI loader 选项不影响模块载荷配对与回滚。

明确的提交点与故障模型
----------------------

不能把 ESP/FAT 上的临时文件加 rename 当作整套更新的断电原子保证。
日常内核更新不重写稳定的 GRUB 入口，采用预分配的 A/B 选择槽：每槽包含
固定尺寸的版本描述文件与独立 SHA-256 校验文件，各自引用不可变版本载荷。
更新只写非当前槽；先同步载荷，再同步完整描述，最后写入并同步该槽校验。
槽从校验失败转为校验成功就是新版本的发布条件，不另写单点 active 指针。

稳定 GRUB 脚本先校验槽，再读取其受限版本/递增序号；选择校验有效且序号
最高的完整版本，同时保留旧版本菜单。解析有界且只允许预定字段/路径，
不能在校验前 source 描述文件。内核与 initramfs 的哈希也要校验。
两平台 GRUB 模块清单补齐原版 hashsum/SHA-256 所需组件，不写自定义引导协议。
首次安装预分配两个槽；已有系统迁移保留旧入口，确认新入口后才启用槽选择。

断电验收模型是任一步写入/同步失败、未同步写入丢失，以及正在写的非当前槽
记录撕裂：校验失败应回退到最后完整槽。设备丢失已同步数据、任意 FAT 元数据
损坏、更新固件/MBR/EFI 本体不属于“内核包原子更新”的可证明范围，必须另行
报告；不承诺跨文件系统/固件的绝对断电原子性。

阶段与执行检查
==============

O1：双仓配置委派
----------------

依赖 K1。修改 ``scripts/reliefos/Makefile.config``、``Makefile.kernel`` 与
帮助文本；新增 ``tests/build/test-kernel-config-delegation.sh``。

* [x] 先以可记录参数的 fake 子 make 测试同一 ARCH/PROFILE/绝对 O= 的委派，
  覆盖新输出、defconfig 重置、普通构建保留 m、用户覆盖 RELIEFNT_O。
* [x] 接入三个 kernel 配置目标；将 defconfig 的 OS/内核操作有序执行，
  不并行竞争同一配置输出，也不引入 parent/child KCONFIG_CONFIG 环境串用。
* [x] 使用真实两仓 conf/mconf 再验证默认全内置与配置持久化。

O2：模块产物、用户工具与 initramfs
----------------------------------

依赖 K2-K4、K6a/K6b、K7。修改 ``Makefile.kernel``、``Makefile.rootfs``、BusyBox
配置；新增 ``tools/build/modules-stage.sh``、``initramfs-stage.sh``、
``system/initramfs/init`` 和相应构建入口。

* [x] 增加 ``test-kernel-modules-stage.sh`` 与 ``test-initramfs-stage.sh``：
  空模块集、依赖闭包、缺文件、错 ABI、档案可复现、受控清理和 m-y-n 切换。
* [x] 构建原生 BusyBox，读取真实 .modinfo/dep 数据库，测试别名、built-in
  no-op、blacklist、参数及多层依赖；不能只用假的 modprobe 输出断言成功。
* [x] /init 挂载 proc/sys/dev、加载启动模块、解析根身份、等待有界设备发现、
  挂载真实根并 exec 新 init。支持 rdinit= 诊断；失败提供明确的早期诊断入口。
* [x] 引入 early/normal 模块加载顺序；不让普通服务在模块未准备好时启动。

O3：ISO、VMDK 与安装器载荷
--------------------------

依赖 K5-K7、O2。修改 ``boot/grub/grub.cfg``、
``scripts/reliefos/Makefile.images``、``tools/build/{boot-stage,iso,images,installer-stage}.sh``、
``userland/apps/installer/install_ops.c`` 和载荷检查。

* [x] 为 BIOS/UEFI、磁盘/普通 ISO/Installer ISO 添加一致的 Multiboot2
  initramfs 项，正确区分安装根载荷与初始根。检查安装器 Xorg/控制台两种载荷。
* [x] 打包前验证必需功能 n 的非法 profile、根驱动/FS 为 m 的依赖闭包、
  模块版本、kernel/initramfs 哈希与解包内容。禁止静默改成 y。
* [x] 分别测试全内置、混合及 allmodconfig 产物，既有输出文件地址不改变。

O4：更新、RPR 与恢复
--------------------

依赖 O3。修改 ``userland/runtime/src/bootloader{,_publish}.c``、
``scripts/reliefos/Makefile.rpr``、``tools/build/rpr-pages.sh`` 及 APK 内核载荷。

* [x] 新增 ``tools/test_kernel_bundle.py``，验证完整发布、缺模块拒绝、短写、
  同步/校验失败、各发布边界中断、双槽记录撕裂、启动前校验回退，以及旧
  入口和旧模块树保留；以私有镜像和明确故障模型注入，不改宿主磁盘。
* [x] 用同一发布接口绑定新版本三件套；只写非当前槽，成功前不改旧选择记录。
* [x] RPR 元数据同时记录所需内核 ABI 和模块清单；发布 guard 继续要求
  子仓 clean 且 gitlink 一致。不得因模块产物增加而跳过该规则。

O5：全链运行矩阵与文档
----------------------

新增 ``tools/test_kernel_modules_qemu.py`` 编排器，消费内核拥有的模块和
原生探针；只操作私有 VM/镜像与短路径 QMP socket。

* [x] defconfig 全内置、部分模块、allmodconfig，以及 MODULES=n 全内置。
* [x] BIOS 与 UEFI；1 和 4 vCPU；分别以 IDE/ATAPI、AHCI、NVMe 为根，
  至少覆盖 ext4 与可支持的其他根 FS；Installer/live 的内存根另外验证。
* [x] 最关键门槛：根控制器=m、根 FS=m，最终出现真实桌面并可读写文件。
  不是只看到 GRUB、启动日志或 /init 探针就算成功。
* [x] e1000 联网和数据校验；声卡 PCM 的 open/mmap/close；Xorg/IceWM 的
  DRM/fbdev 路径；内置/模块行为一致。QEMU 无 SVGA3D 不能冒充 VMware 通过。
* [x] 多层依赖装卸、内置 modprobe、重复加载、故意缺依赖/错版本文件的
  错误诊断、busy 卸载拒绝、空闲装卸循环、进程/映射/IRQ 并发持有。
* [x] 以相同 release profile 记录 yes/dd、启动耗时与桌面操作延迟对比；
  出现稳定明显退化先定位，不能把性能损失归因于“模块本来就慢”。
  同一 BIOS/1 vCPU/AHCI/4 GiB 的 QEMU/KVM 平台上，预热后各五次
  2 GiB yes/dd，中位数约为 default 2.755 GB/s、全模块 2.749 GB/s；
  比值为 0.9979，差异在本次样本波动范围内。普通启动及 VT 切换记录
  只是单次观察，不能当作统计延迟结论；缺少同环境改造前基线，不声称
  相对旧内核全面无性能退化。PIIX/IDE 无 initramfs 的直接根
  已通过桌面与声卡验证，但冷启动仍慢。实际 VMware 的 HDA、DRM 各八轮
  生命周期及普通桌面十轮 Fileman/VT 验证通过；真实物理硬件尚未验证。
* [x] 更新两仓职责对应的构建/安装/ABI/驱动文档与 RPR 说明，清除失效静态
  假设；保留有明确用途的全内置独立启动路径与历史发布事实。

安装验收的实际范围
------------------

全模块媒体的控制台、中文桌面两套实际安装盘均已分别从 BIOS、UEFI 启动；
BIOS 完成普通用户登录，桌面版实际显示 IceWM 与 Fileman，UEFI 到达对应
登录界面。离线检查两盘文件系统、UID 1000 账户、全部 41 项内核载荷及其中
24 个模块的路径、大小与哈希；模块包括 18 个生产模块和 6 个验收模块。
默认全内置、混合与 MODULES=n 的普通运行由各自配置的运行矩阵验证，
不能把这两次全模块新安装扩大表述为四种配置均完成了全新安装。

原 2400 秒安装速度门禁仍失败。独立功能长测实际完成桌面安装后，因全量
约 6 MB 日志经串口读取超时而失败，原结果未改判通过。取证工具已改为读取
有界末尾、使用本次请求标志并检查读取状态，宿主回归与真实来宾正负例通过；
已有安装盘再经独立双固件启动和完整离线检查验收。工具修正后没有重跑完整
安装复制，不作为安装提速证据。功能长测的 7200 秒设置只是等待循环截止，
单次取帧或完成确认可以越界，并非绝对时间墙。

新版 Installer 的 VMware 验证未完成，真实物理硬件未验证；上述内核 VMware
运行结果与 Installer 安装验证分别记录。

交付证据分源码检查、宿主测试、编译打包、QEMU、VMware与真实硬件记录，
每个运行结果绑定内核、模块与 initramfs 的精确哈希。
既有宿主失败、ABI golden 决策与音频 manifest 决策独立记录，不静默改写。
VMware 验证未完成时必须明示；实现与运行结果按实际完成的阶段分别记录。

参考
====

* `Linux Kbuild 模块安装 <https://docs.kernel.org/kbuild/modules.html>`_
* `Linux 早期用户态与 initramfs <https://docs.kernel.org/filesystems/ramfs-rootfs-initramfs.html>`_
