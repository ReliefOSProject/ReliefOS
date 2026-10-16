# ReliefOS EEVDF 调度

本实现参考本地 Linux 6.12 的 `kernel/sched/fair.c` 中 EEVDF 的调度规则，
以 ReliefOS 的任务、锁和时钟接口独立实现，替换原来按严格优先级轮转的任务选择。
所有现有可调度用户任务，包括桌面和服务，使用相同的公平调度策略。
进程生命周期、信号、地址空间、上下文切换及 idle 机制仍由原有内核接口负责。

## 已实现的规则

- 根据实际运行时间和 Linux nice 权重累计虚拟运行时间；保留整数除法余数，
  避免频繁短系统调用在记账时丢失 CPU 用量。
- 计算运行队列加权平均虚拟时间，只有 `vruntime <= V` 的实体有资格运行，
  从中选择最早虚拟截止时间；时间片用完后续订请求。
- 系统调用、缺页和时钟调度入口都进入同一选择策略，不在每次系统调用时重置时间片。
- 初次加入使用半个请求长度；唤醒时保留有界 lag，并补偿新实体对平均值的影响。
- 睡眠时负 lag 实体延迟出队，待债务衰减后再移除，防止短暂睡眠重置欠账。
- `nice` 改变时按旧权重结算，再调整 lag 与剩余虚拟请求；`sched_yield` 放弃当前截止时间。
- `fork` 子进程重新初始化调度实体，不继承父实体的队列链接和运行记账。
- 每 CPU 公平域、亲和性约束、空闲 CPU 拉取工作，以及迁移时按 lag 放置。
  任务帧发布和运行 CPU 的所有权仍由 `scheduler_lock` 原子维护。
- 每 CPU 使用增强红黑树，按虚拟截止时间排序，以子树最小虚拟运行时间筛选
  有资格的实体；相同截止时间仍优先选择最后入队的实体。

## 与 Linux 6.12 的差异

ReliefOS 当前时钟抢占为 100 Hz，所以请求长度选用 10 ms；没有宣称实现 Linux
亚毫秒 hrtick 精度。增强红黑树独立实现，保持原有 ReliefOS EEVDF 策略，
未移植 Linux 的 PELT、调度组、CPU capacity/NUMA、CFS bandwidth、实时调度类或
`RUN_TO_PARITY` 特性。现有 Linux ABI 对不支持的调度策略继续拒绝。

树的平均虚拟时间查询为 O(1)，通常的入队、出队、重设截止时间和资格搜索为
O(log n)。一次选择若清理 k 个债务已衰减的休眠实体，复杂度为
O((k + 1) log n)。`fair_sync()` 和空闲 CPU 的工作窃取仍扫描任务表，
整个调度轮次没有变成 O(log n)。

当前生产代码在 `arch/x86_64/smp.c` 已启用 AP 用户态调度：`smp_release_aps()` 在 BSP
完成首轮用户态 timer tick 之后置位 `smp_scheduler_started`，此后每个 AP 在
`smp_ap_entry` 的永久循环里调用 `userland_schedule_from_frame(NULL)` 并 `arch_enter_user_frame`
进入 Ring 3。配置 N 个 vCPU 时，N 个 CPU 都可以选取和运行用户任务。

**限制仍然存在**：设备 IRQ 目前全部路由到 BSP，AP 只接收 LAPIC 定时器 vector `0x40`
用于抢占（见 `smp_ap_entry` 里的注释"No device IRQ is routed to an AP"）；系统调用的
全局执行锁（`lock.c` 中的 `kernel_execution_lock_irqsave`）串行化几乎所有内核态服务，
所以多核有效并行度仍受这把 ticket 锁限制。拆分锁粒度或至少把设备 IRQ 分派到 AP 是
后续独立工作。每 CPU 选择和迁移有确定性的主机测试；启用 AP 前上下文切换/回收同步
审计已在 `smp_release_aps` 的"等待 BSP 完成首轮用户 tick"机制下补做。

## 验证

以下保留原链表策略的历史验证记录；红黑树重构的验证见末节。

```sh
python3 tools/test_eevdf.py
python3 tools/test_linux_threads.py
python3 tools/test_linux_memory.py
python3 tools/test_linux_rootfs.py
python3 build.py run kernel
python3 tools/test_execution_lock_qemu.py --scheduler --out build/eevdf-scheduler-verified
python3 tools/test_chroot_qemu.py
python3 tools/test_uinxed_guest.py --source /path/to/clean/Uinxed-Kernel --out build/uinxed-eevdf
```

确定性测试运行真实 EEVDF 策略与 `sched.c` 的选择/帧发布路径，覆盖公平性、
nice 比例、资格与截止时间、睡眠欠账、唤醒、yield、动态改权重、多 CPU 所有权、
亲和性迁移、阻塞、工作窃取和退出回收。使用 ASan/UBSan 检查内存及整数错误。

来宾 CPU 密集测试把两个进程固定到同一实际 CPU，单轮竞争 4 秒：
同 nice 工作量比例为 0.953，nice 0:5 为 2.941（理论权重比约 3.057）；
二者都持续取得进展。连续 16 轮 fork/亲和性/yield/wait 检查通过。
短测量还包含桌面和系统服务负载，不把单次比例当作精确性能保证。

审查补充了已出队休眠任务修改 nice 的测试：保存的 lag 按旧/新权重换算，
修改优先级不会唤醒任务或凭空增加其服务补偿。策略与实际调度入口测试均通过。
最终内核 SHA-256 为 `d3e1fae6326a04763dc1479d7d0ffd770ee6e361f949d6d9fc3a9a94d3b137ae`，
与上述来宾复测加载的内核一致。`build/images/leonos4-installer.iso` 已重新生成。
此机器的默认 PATH 优先找到缺少 compiler-rt 的自定义 Clang，构建使用
`PATH=/usr/bin:/bin python3 build.py run installer-image`，结果为 0 errors。

Uinxed 提交 `a9786eb8e147389ba960810537a4e35c579dcc18` 在 LeonOS 来宾中执行
`make -j2 UxImage` 成功，耗时 1056.981 秒，产物为 `build/uinxed-eevdf/UxImage`
（9,807,888 字节），SHA-256 为
`ae044ccde9d4bec71292cded9e7d27409a0059b76e42a760cb796aca8c69e42e`。
构建使用的 EEVDF 内核哈希为
`cdd3bee2899ee6b9cea0a71036d168c3f650563e0c70b76404a0ea0e7ab6b663`，
早于审查补充的休眠改权重修正；未在最终内核上重复完整 Uinxed 构建。
最终内核另外通过公平性、内存/线程和 chroot/exec 来宾回归。
此前运行耗时 1335.853 秒，本次约减少 20.9%，但仅为单次观测，
两次均只有一个活跃调度 CPU，宿主负载及构建工具环境也未严格控制，不能归因为稳定调度提速。

## 2026-10-16：增强红黑树重构

`eevdf.h` 负责策略、加权平均缓存和队列成员关系，私有 `eevdf_tree.h`
负责红黑树旋转、插入、删除及资格查找。树以虚拟截止时间、逆入队序号排序，
维护子树最小 vruntime 和休眠实体的最小 vruntime；先清理符合条件的延迟出队
实体，再选择最早截止时间。普通运行记账若未改变截止时间，只向祖先更新增强值。

队列保存相对 `clock` 的加权总和，沿用原策略的负数向下取整规则。
改变基准时同步换算缓存；使用 128 位中间值完成减项、加项和基准转换，
然后保存为原来的 64 位单位，不引入 128 位除法。整数边界回归覆盖了
最终结果可表示、但分步相乘或相减会溢出的情况。

任务分配使用私有包装对象，在 `struct task` 后附带一个 112 字节树节点。
原实体的链表指针位置改为指向该节点；实体仍为 64 字节。节点随任务槽位保留，
入队时无需分配内存。清零、fork 复制和回收重新绑定各任务自己的节点。
LP64 主机验证中 `struct task` 仍为 104176 字节，实体、priority、frame、
running_cpu 等关键偏移与改动前一致。ABI golden 和音频支持清单没有修改。

成员顺序链仅用于 O(1) 摘除，以及 64 位入队序号回绕前的极罕见 O(n)
重编号；平均值、活跃任务计数和普通资格搜索不再扫描该链。

验证命令（从父仓库运行）：

```sh
mkdir -p out/tmp
export TMPDIR="$PWD/out/tmp"
python3 tools/test_eevdf.py
python3 tools/test_linux_sysv_shm.py
make -C kernel/reliefnt O="$PWD/out/vdso-time-kernel" -j8 all
```

测试保留原链表实现作为独立的测试侧对照，执行 7000 次确定性随机操作，
核对选择结果、虚拟时间、截止时间、lag、权重、运行量和成员关系。
另对 2048 个节点验证父子关系、颜色、黑高、增强值、树高与删除后的状态。
ASan/UBSan 遇到错误会使测试失败。原有公平性、nice 比例、睡眠债务、yield、
亲和性迁移、多 CPU 预留和退出回收断言保留。
共享内存夹具改用同样的私有任务包装，继续验证真实 fork 失败回滚和回收路径。

最终测试内核 SHA-256：
`69f5ffce4079b26149ae2db2a5bef380b51962f49727ddec893278eefd7278a7`。
内核构建、EEVDF 测试、SysV SHM 测试和 15 组相关主机回归通过。

QEMU/KVM 双核客体中，同 nice 竞争工作量比为 0.995，nice 0:5 为 3.032；
CPU0 忙循环时，CPU1 上两个竞争者分别完成约 196 万、195 万次工作。
这是正确性和持续进展检查，不作为调度器整体提速比例。
QEMU 约 200 Hz 输入的标题栏/Alt 拖动、缩放、最大化、最小化、还原和最终
终端输入全部通过；私有快照基底和输入镜像哈希均未改变。

本机热缓存合成测试只重复查询静态队列，不计入运行记账、入队和完整调度轮次：
16 个实体约为链表 37.7 ns、树 11.7 ns；8192 个实体约为链表 23.0 µs、
树 55.8 ns。测试受宿主负载和缓存影响，不能推导应用或整个内核的加速倍数。
记录位于 `out/tmp/rbtree-pick-bench.log`。

客体证据位于 `build/scheduler-rbtree-guest/`、
`build/scheduler-rbtree-icewm-200hz/`；构建和测试日志位于 `out/tmp/rbtree-*`。
本机独立 Workstation 双核、关闭 3D 的完整交互回归也通过，证据位于
`build/scheduler-rbtree-vmware-verified-200hz/`，测试结束后虚拟机已停止。
干净的测试镜像为 `out/scheduler-rbtree/images/reliefos.vmdk`，内核读回校验
和测试索引分别保存在同目录上级的 `kernel-manifest.json`、`verification.json`。
提交前使用同一内核再次通过 QEMU 双核调度及约 200 Hz 输入的完整交互测试，
证据位于 `build/scheduler-rbtree-prepush-qemu/` 和
`build/scheduler-rbtree-prepush-icewm/`。同 nice 比为 0.997，nice 0:5 为
3.021；CPU1 两个竞争者均持续运行。两个 EFI 内核路径读回哈希一致，
源镜像和测试快照基底均未改变。
原先确认的跨 CPU 唤醒 tick 延迟和全局执行锁限制仍然存在。
**VMware 验证未完成**：用户原实例及 3D 配置未复测；本机独立 VMware
交互回归单独记录。构建和运行产物不纳入源码提交。
