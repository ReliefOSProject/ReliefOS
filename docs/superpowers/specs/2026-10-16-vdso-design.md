# x86-64 vDSO 时间接口设计

日期：2026-10-16。状态：用户已批准；实现、独立审查、宿主测试及 QEMU 验证完成。
最终结果见 [VDSO.md](../../VDSO.md)。验证完成后，用户明确授权提交并推送。

## 目标与边界

给 ReliefNT 增加 Linux x86-64 vDSO，使现有 musl 程序能够通过标准
`clock_gettime()` 自动使用用户态快速路径。Linux vDSO ELF ABI 与 POSIX
时钟语义分别约束发现/调用机制和时间结果；提供 vDSO 不代表整个系统已经
获得 Linux 或 POSIX 兼容性认证。

首版提供 `__vdso_clock_gettime` 和 `__vdso_clock_getres`，版本均为
`LINUX_2.6`，并提供对应的无前缀别名。重点加速 REALTIME、MONOTONIC 和
两种 COARSE 时钟。其他时钟和不满足快速路径条件的环境，在 vDSO 内直接
执行原始 syscall，保留内核的支持范围和错误结果。不会把 CPU 时钟伪装成
系统单调时钟。

这次不实现 SYSRET、全局执行锁拆分、32 位兼容 vDSO、getcpu/getrandom，
也不同时建设新的半虚拟化 clocksource 或完整的跨 CPU TSC 同步框架。
保留现有工作分支和未跟踪文件；实现阶段未授权提交、推送或建分支；验证完成后用户明确授权提交并推送，仍不新增分支。
不得重生成 ABI golden，不修改 audio manifest 的待决策内容。

## 实现前代码事实

- 内核子模块基线是 `f485823e2360e075b6de19ef115c93dd1eed16cc`。
- `kernel/exec/userland.c:prepare_user_exec_stack()` 分配 16 对 auxv，尚无
  `AT_SYSINFO_EHDR`。exec 在新地址空间装载成功后才替换旧 mm，应保留此事务边界。
- musl 1.2.6 的 x86-64 实现已经解析
  `__vdso_clock_gettime@LINUX_2.6`，无需修改第三方 libc。
- `kernel/reliefnt/time.c:time_clock_get_locked()` 是当前系统时钟结果的来源，
  通过 IRQ-safe `clock_lock` 保护。支持时钟 ID 0、1、4、5、6、7、11。
- 当前高精度读取使用 PIT tick 基准与 TSC 插值，量化到微秒、封顶一个
  10 ms tick。启动核用 7 个 tick 间隔校准；目前没有 invariant-TSC 或
  跨 CPU 同步证明，现有序列号也没有覆盖全部时钟状态。
- `struct task_vma` 已有 `max_prot`，可限制 vvar 权限而无需改变结构体布局。
  现有页表克隆、物理页引用计数和 exec 回滚机制可以复用。

## 可选方案与选择理由

1. **推荐：标准 ELF + 共享快照 + 条件启用。** COARSE 读取不需要硬件计数器；
   高精度读取只在已证明安全的时间源上启用，否则直接 syscall。
   首版不以 invariant CPUID 位单独充当 SMP 同步证明。
2. 同时建设连续高精度 clocksource、SMP TSC 同步/监控及虚拟化时钟支持。
   能扩大高精度快速路径的覆盖率，但会把工作扩展到独立的时间源子系统。
3. 只发布调用 syscall 的 ELF 包装。能验证 ABI 接入，但没有读取加速，
   不作为本任务完成目标。

## ELF、装载和地址空间

构建独立的 freestanding PIC ELF64 `ET_DYN`，机器类型 `EM_X86_64`，
SONAME 为 `linux-vdso.so.1`。包括 PT_LOAD、PT_DYNAMIC、动态符号/字符串表、
SysV 或 GNU hash、GNU 符号版本定义；没有解释器、DT_NEEDED、运行时重定位或
可写可执行段。构建规则记录编译与链接参数变化并正确追踪依赖。

内核嵌入该 ELF，在 exec 新 mm 中选择经过碰撞检查的随机页对齐位置。
代码映射 RX，独立 vvar 数据映射 R/NX；数据页的其他字节清零，不包含
内核地址或进程私有信息。auxv 增加 `AT_SYSINFO_EHDR`，同时调整空间计数；
只有映射完整且可读时才公布 ELF 基址。

代码和数据的共享物理页保留内核引用，每个地址空间持有自己的引用；fork
继承、exec 替换和装载失败必须准确回收。vvar 不允许 mprotect 写入或执行，
也不能通过 COW 得到一份停止更新的私有副本。VMA 使用现有 max_prot 机制，
不增加影响 ABI golden 的任务结构字段。正常 mmap 选址不能覆盖这些 VMA；
显式 munmap/MAP_FIXED 的行为由既有内存 API 处理，调用者破坏自身 vDSO 后
不再保证 libc 缓存函数指针可用。首版不支持单独移动 vDSO 或 vvar。

## 时间数据与一致性

vvar 内部布局仅是内核与它构建的 vDSO 之间的私有协议，不作为新的公开 UAPI。
快照包括序列号、可用状态、REALTIME/MONOTONIC/COARSE 基准、时钟分辨率，
以及可选计数器的锚点、换算参数和安全状态。

所有发布在 clock_lock 下串行进行：写入者先发布奇数序列，更新完整快照，
再以 release 顺序发布偶数序列。读者使用匹配的读取与屏障协议，将结果
暂存，确认序列未变化后才写入 timespec；重试有界，不能无限自旋。
tick、设置墙钟和成功的 adjtimex 修改均必须更新相关数据。

vDSO 和 syscall 复用同一时间换算规则。高精度路径启用前必须覆盖并处理
tick 边界、负向频率/相位调整以及插值上限的连续性；不能把旧的 tick
基准与新的 TSC 锚点组合，也不能让 MONOTONIC 在边界倒退。
分辨率必须与实际量化能力一致，不能把当前微秒插值宣称为 1 ns 分辨率。

COARSE 快速路径只读取一致快照，不执行 RDTSC，因此可以用于 SMP。
首版高精度 TSC 路径限于：单个在线 CPU、支持用户态 TSC 读取、具有
invariant-TSC、校准结果有效且连续性测试通过。缺少任何条件均走 syscall。
多 CPU 环境的高精度调用保持 syscall，直到独立工作提供可信同步时间源。
安全状态变化也必须发布，不能由用户态缓存成永久可用。

## Linux/POSIX 行为约束

- 使用 SysV AMD64 C 调用约定，x86-64 timespec 是两个 64 位字段。
  vDSO 返回 0 或原始负 errno；libc 继续负责 POSIX 的 -1/errno 转换。
- 输出满足 `0 <= tv_nsec < 1000000000`，MONOTONIC 不受墙钟设置影响。
  REALTIME 允许设置/校时，调时后必须读取到新状态。
- 未加速时钟、动态/负时钟 ID、无效 ID，以及尚未初始化的快照统一走
  对应 syscall。不得把合法但未加速的时钟直接返回 EINVAL。
- `clock_getres(clock, NULL)` 仍验证 clock，并在合法时成功。
  直接 syscall 的指针校验及 errno 顺序保持原样。
- NULL 输出指针可以主动转入 syscall。Linux vDSO 对其他无效用户指针可能
  产生 SIGSEGV；不宣称用户态存储可以完全模拟内核的 EFAULT 探测。
- RTC 无效时，现有 REALTIME 使用 monotonic 的行为是已有的 Epoch 语义缺口。
  该状态不启用 REALTIME 快速路径，本任务不借 vDSO 掩盖或宣称修复此缺口。

## 实现分解

1. 用失败测试约束生成 ELF 的结构、符号版本、无运行时依赖及真实函数调用。
2. 增加共享时钟协议和发布机制，以实际时间代码测试序列一致性、归一化、
   调时传播、负向纪律调整及 syscall/vDSO 结果一致性。
3. 增加 vDSO 专用构建、嵌入与 exec 映射，覆盖引用计数、失败回滚、碰撞、
   权限及 auxv；沿用现有 VMA 和页表机制。
4. 增加静态/动态 musl 客体探针，核对发现、符号解析、直接调用、libc 调用、
   syscall 回退、fork/exec，以及同一线程在各 CPU 迁移后的结果。
5. 运行内核编译、相关宿主测试及 QEMU 1/2 vCPU 验证；在配对基准中分别
   报告 raw syscall、libc/vDSO 和 COARSE。计数器不可信时明确报告 fallback，
   不把它算作快速路径性能结果。

## 验证与已有失败

2026-10-16、实现修改前已运行：

- `TMPDIR="$PWD/out/tmp" PYTHONDONTWRITEBYTECODE=1 python3 tools/test_linux_time.py`
  通过（nanosleep 状态测试，不是 vDSO 证据）。
- 同环境 `python3 tools/test_linux_memory.py` 失败，4 个子用例为既有测试夹具
  与现有内核不匹配：physical_pages 的 mm_init 参数数目、elf_file_page 缺
  console_log、mprotect_vma_growth 缺 SHM/audio 生命周期桩、brk_collision 的
  task_shm_map 签名。其余已执行子用例的结果与失败分开报告。
- 同环境 `python3 tools/test_openrc_network.py` 中 interface、packet 和 NTP
  discipline 用例通过；timekeeper_discipline 因夹具缺 audio_service_tick 和
  console_display_tick 而链接失败，后续 driver_control 未执行。
- `tools/test_secure_loader.py` 首次缺少必需的 --output 参数，未执行其验证；
  它属于 Linux 参考机/客体端到端流程，不计为已通过的装载器基线。

不修改 ABI golden，也不为显示全绿而顺带改变既有测试的期望。
最终报告必须分开列出编译、宿主测试、QEMU 运行和硬件验证。
**VMware 验证未完成。**

## 标准与源码参考

- [Linux vDSO ABI 文档](https://docs.kernel.org/admin-guide/abi-stable.html#vdso)
- [Linux vDSO ELF、auxv 与符号版本说明](https://man7.org/linux/man-pages/man7/vdso.7.html)
- [Linux 6.12 x86 vDSO 符号版本表](https://github.com/torvalds/linux/blob/v6.12/arch/x86/entry/vdso/vdso.lds.S)
- [Linux 6.12 x86 vDSO 映射实现](https://github.com/torvalds/linux/blob/v6.12/arch/x86/entry/vdso/vma.c)
- [POSIX.1-2024 时钟定义和 timespec 约束](https://pubs.opengroup.org/onlinepubs/9799919799/functions/V2_chap02.html)
- 本仓库 musl：arch/x86_64/syscall_arch.h、src/internal/vdso.c、
  src/time/clock_gettime.c、ldso/dynlink.c。
