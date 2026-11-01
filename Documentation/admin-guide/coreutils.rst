========================
GNU coreutils 与 BusyBox
========================

命令与文件归属
==============

系统安装锁定的 GNU coreutils 9.12 全部 109 个命令入口，包括 ``[``、
``test``、``arch``、``hostname``、``kill``、``uptime`` 与 ``coreutils``
分发器。完整清单位于源码 ``userland/coreutils/programs.list``，构建时与
锁定上游归档的程序清单比较，缺失任意入口或辅助库都会使构建失败。

``/usr/bin/coreutils`` 是实际 ELF，其余 GNU 入口使用上游 single-binary
symlink 模式指向它。``stdbuf`` 的预加载库安装在
``/usr/libexec/coreutils/libstdbuf.so``；程序、链接、文档和许可证由
``reliefos-coreutils`` APK 管理。

根文件系统没有合并 ``/bin`` 与 ``/usr/bin``。原先由 BusyBox 提供的
``/bin/cat``、``/bin/cp``、``/usr/sbin/chroot`` 等旧路径保留为指向 GNU
入口的相对链接，并转交同一个 coreutils 包。链接生成阶段不会再为同名 GNU
命令生成 BusyBox 入口。其他命令与 BusyBox 的 ``sh``、启动及救援功能保持可用。

在终端中检查外部入口和 APK 归属::

    type -P ls
    /usr/bin/ls --version
    readlink -f /bin/cat
    apk info --who-owns /usr/bin/ls /bin/cat

``echo``、``printf``、``test``、``pwd`` 等名称也可能是 Bash/ash 内建命令。
需要明确调用 GNU 版本时使用 ``/usr/bin/printf`` 等绝对路径。显式
``/bin/busybox NAME`` 仍调用 BusyBox applet；更新和救援脚本需要此入口。
不要用 ``busybox --install`` 覆盖系统已经登记的文件。

升级与兼容性
============

新的 BusyBox 包依赖 ``reliefos-coreutils``，GNU 包声明接管旧 BusyBox 包的
重叠文件，使 APK 在正常事务中转移所有权，无需强制覆盖。不要手工删除
BusyBox ELF 或剥离它的内置 applet；显式救援调用仍依赖它们。
发行包与 Alpine 的 ``coreutils``、``coreutils-single`` 声明冲突，避免两个
包同时提供 GNU 入口。使用系统的 ``reliefos-coreutils`` 包进行更新。

GNU 与 BusyBox 的选项集合不完全相同。例如 GNU ``hostname`` 接受主机名参数，
不支持 BusyBox 的 ``-F``。系统初始化对现有主机名文件的读取显式使用
``/bin/busybox hostname -F /etc/hostname``；自定义脚本也应选择正确的实现。

功能边界
========

启用所有命令入口不代表内核实现所有扩展。``chcon`` 和 ``runcon`` 使用
上游没有 libselinux 时的实现，对实际 SELinux 操作返回失败；ReliefNT 当前
不提供 SELinux。不能把这两个命令能够显示版本解释为安全上下文操作可用。

本构建保持禁用 NLS、ACL 和扩展属性集成。普通权限、用户/组、文件操作、文本、
编码、校验和等使用真实 GNU 实现；依赖具体文件系统、设备或内核功能的选项，
仍以实际系统支持及命令退出码为准。

目标代码使用 x86-64 的 x87/SSE 基线。可选的 CPU 分发 CRC 后端会引入宿主
compiler-rt 的预编译 CPU 检测构造函数，该函数的 ISA 不受目标 ``-mno-avx``
约束，因此交叉构建使用上游通用 CRC 路径。此设置保留 CRC 计算功能，
也避免在尚未保存 AVX 状态的内核中执行 AVX 指令。

验收与测速
==========

``tools/test_coreutils_qemu.py`` 使用完整生产 VMDK 的私有副本启动 ReliefOS，
仅注入用户态测试程序、数据和 OpenRC 测试服务，不替换生产内核、libc 或 GNU
程序。它核对镜像内 ELF 与构建产物的哈希，并在结束后核对原始 VMDK 未修改。

来宾测试逐个验证 109 个命令入口和实际行为，包括退出码、文件内容、权限、
链接、文本转换、编码、散列、终端与进程工具。SELinux 两项单独记录为预期
不支持；同时查询真实 APK 数据库确认 GNU 路径归属，检查显式 BusyBox 救援调用。
这不是全部 GNU 上游测试或全部选项组合的替代品。

使用同一优化级别比较两种实现，例如使用 release 构建::

    mkdir -p out/tmp
    TMPDIR="$PWD/out/tmp" make O=out/coreutils-release PROFILE=release defconfig
    TMPDIR="$PWD/out/tmp" make O=out/coreutils-release PROFILE=release -j6 image-vmdk
    TMPDIR="$PWD/out/tmp" python3 tools/test_coreutils_qemu.py \
        --build-dir out/coreutils-release \
        --busybox-benchmark out/coreutils-release/userland/busybox.elf \
        --output out/coreutils-qa-new --accel kvm

验收虚拟机使用 1 个 vCPU、4 GiB 内存，并显式提供 invariant TSC。
当前内核仅在单 CPU 条件下启用可信 TSC 高精度计时，多核回退的 PIT 计时
可能漏算长时间同步 I/O 的墙上时间。测试消费内核仓自己的 vDSO 探针，要求
高精度模式验证成功后才测速；不能把不满足此门槛的数据作为吞吐结果。

每项性能测试先预热一次，再测三次，输出原始纳秒样本、中位耗时与 MiB/s：

* ``cat``：读取 32 MiB，写入 ``/dev/null``。
* ``wc -l -w -c``：完整扫描 32 MiB，核对行数、单词数和字节数。
* ``sha256sum``：对 32 MiB 文件计算并核对已知摘要。
* ``tr a-z A-Z``：转换 32 MiB，写入 ``/dev/null``。
* ``sort``：排序 262144 行八位数字（2.25 MiB），写入 ``/dev/null``，
  另对完整排序输出核对独立生成的有序数据摘要。
* ``cp``：实际向 ext4 复制 1 MiB 文件，并逐字节比较副本。

``cat``、``tr``、``sort`` 另通过管道对完整输出做不计时的摘要校验，避免将文本
处理测试变成额外的磁盘写入测试。复制使用较小的数据集，以适应当前文件系统同步刷新
成本；不为测速绕过系统持久化语义。测量包含进程启动与退出，输入经历
预热，不清空系统缓存，也不要求每次复制强制落盘。因此它们是指定工作负载的
耗时与吞吐，不能当作冷盘速度、持久化写入速度或跨机器性能保证。报告应包含
虚拟机平台、vCPU/内存、两种实现的编译优化级别及校验结果。

真实签名 APK 的旧路径升级可单独验证::

    TMPDIR="$PWD/out/tmp" python3 tools/test_coreutils_apk_upgrade.py \
        --build-dir out/coreutils-release --output out/coreutils-upgrade-new

此测试在宿主 Linux 用户命名空间的私有根目录内执行 APK 事务，仅证明包升级
和文件归属；系统内功能与测速必须以上述 ReliefOS 来宾运行结果为依据。
