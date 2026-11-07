============================
内存、swap 与 OOM 的目标验证
============================

这些入口用于独占、可丢弃的 ReliefOS 来宾，不是生产启动路径。内核探针源码位于
``kernel/reliefnt/tools/tests/``；使用正式 SDK 的 ``bin/reliefos-musl-cc`` 构建，
不要在宿主上执行来宾压力探针。将构建、宿主测试和内存测量分时进行，并记录
配置、磁盘、内核、模块、initramfs、工作负载和实际平台。单次成功只覆盖实际运行的
配置和用例。

来宾探针
========

``memory_lifecycle_guest_probe.c`` 要求 ``uname -s`` 为 ``ReliefNT``。示例在
来宾中运行，文件参数必须是 root ext 文件系统中不存在的专用路径：

.. code-block:: sh

   /usr/lib/reliefos/tests/memory/lifecycle --guest-lifecycle \
       /root/memory-lifecycle-example 180 --eviction-bytes 402653184

探针在任何锁策略测试之前，用独立 peer 栈执行原始隐式栈的 ``CLONE_VM`` 丢弃与
重访；随后检查私有 fork/COW、共享匿名 fork、普通锁失败后的既有锁、``PROT_NONE``
固定移动和权限恢复。共享文件首次写测试仅在可将两个 peer 固定到两个实际 CPU 时
运行，否则记录 ``LIFECYCLE_SKIP``。两个 peer 的就绪与指定 CPU 不证明首次 fault
精确重叠，也不证明远端 TLB 故障或硬件特性。探针用锁页拒绝丢弃和未锁页可丢弃
进行有限检查，不校验 ``VmLck`` 数值。

``poll`` 和 ``pselect`` 用已同步的延迟写入检查空管道不就绪、用户可见的等待与
唤醒、返回的字节和描述符复用；等待时间不证明内核内部的瞬态任务状态。

可选字节数须为 4096 的倍数、不超过 2 GiB；应根据来宾 ``MemTotal``、已有可用
swap 和宿主内存预算选取。探针写入整个私有映射后，结合标准 ``mincore``、系统级
``pswpout`` 快照及活动 swap，只有在对应操作前观测到非驻留页才检查 fork/COW 与
受保护页的移动、恢复和字节指纹。没有足够非驻留页或相关观察不可用时输出 SKIP，
不能当成换页通过；计数器是系统快照而非某个进程的归属证明。

现有 ``memory_pressure_guest_probe.c`` 可用 ``--expect-nonresident`` 检查更大
工作集的实际非驻留与完整读回；``memory_oom_guest_probe.c`` 的
``--guest-oom MAX_BYTES TIMEOUT_SECONDS`` 对所拥有子进程制造破坏性压力，验证
OOM 信号、计数和后续分配。应按目标环境设置上限并在私有 VM 内显式运行。探针的
编译成功不等于它们已在来宾运行。

QEMU 编排
=========

``tools/test_memory_qemu.py`` 消费已有标准磁盘、内核 payload/ESP cohort、所选
``kernel.sys`` 以及由 SDK 编译好的 lifecycle、pressure 和 OOM 可执行文件。
它在新输出目录内制作私有磁盘并逐项读回载荷；不改写源盘，不编译、不下载、不修改
用户磁盘。每次准备生成随机 nonce。来宾 workload 由调用方提供，它应使用
``MEMORY_QA_NONCE`` 生成自己的文件名，检查每个命令的返回值并在完成或错误时处理
自己的资源。预置可执行文件的路径为：

* ``/usr/lib/reliefos/tests/memory/lifecycle``
* ``/usr/lib/reliefos/tests/memory/pressure``
* ``/usr/lib/reliefos/tests/memory/oom``

例如，准备并启动一个 BIOS、256 MiB、双 CPU 的 AHCI 来宾：

.. code-block:: sh

   TMPDIR="$PWD/out/tmp" PYTHONDONTWRITEBYTECODE=1 \
   python3 tools/test_memory_qemu.py \
       --source "$SOURCE_IMAGE" --source-sha256 "$SOURCE_SHA256" \
       --kernel "$KERNEL_SYS" --root-payload "$ROOT_PAYLOAD" \
       --boot-payload "$ESP_PAYLOAD" --lifecycle "$LIFECYCLE_BIN" \
       --pressure "$PRESSURE_BIN" --oom "$OOM_BIN" --workload "$GUEST_SCRIPT" \
       --output "$NEW_OUTPUT_DIR" --qmp-dir "$NON_TMPFS_DATA_DIR" \
       --firmware bios --memory-mib 256 --cpus 2 \
       --rss-limit-mib 3072 --timeout 900

输入磁盘必须有唯一的 ``RELIEFOS_ROOT`` 和 ``RELIEFOS_ESP`` GPT 分区，且尚未
包含待注入的同版本内核树。``--prepare-only`` 只生成私有镜像和 ``prepared.json``；
此模式无需 QMP 目录。运行模式仅启动一个工作磁盘，拒绝已存在的 QEMU/VMware
进程，在调用者指定的非 tmpfs 目录创建短 QMP socket，对自己的 VM 设置超时和
RSS 上限，并记录清理状态。请为高 RAM 测试另行选取宿主能够承受的 RSS 限额；
配置的来宾 RAM 容量不等于允许它全部触碰宿主内存。工作镜像和日志保留在新输出
目录，目录中的固定锁文件留在 QMP 目录中供后续运行串行化。

``result.json`` 的 ``script_completed`` 只说明本次非重复 nonce 的工作负载以零
状态完成、启动标记匹配且没有报告 fatal；``observations`` 列出实际打印的探针
标记与 SKIP。具体 swap 激活、换出、回读、OOM、swapoff 及工作负载正确性仍须由
workload 的断言和串口日志证明。此编排不自动设置 swap，也不自动证明高 PFN、
跨 CPU fault 同时发生、VMware、物理硬件或完整 Linux/POSIX 兼容。标准模块高 PFN
探针说明见 ReliefNT 的 ``Documentation/core-api/resource-limits.rst``；应记录其
物理地址、实际读写、平台和限制。
