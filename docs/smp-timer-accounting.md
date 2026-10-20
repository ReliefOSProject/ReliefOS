# SMP 定时器与 CPU 时间统计

## 修复内容

副核原来用固定 LAPIC 初值 `1000000` 进行周期抢占。该数值不是频率：同一初值在本次 VMware 环境约为 4.1 Hz，在 QEMU/KVM 中约为 62.5 Hz。CPU 时间又直接按各核收到的定时中断次数累加，因而副核的统计结果随硬件计数频率改变。

现在每个副核在 BSP 启动屏障之后，用 ACPI PM 计数器校准本地定时器，目标为 100 Hz。BSP 的 PIT、设备 IRQ 路由和 EEVDF 策略保持原有实现。启动阶段的 PIT 中断可能集中补发，因此不能把一组中断的到达间隔当成校准参考。

ACPI PM 计数器按 [ACPI 6.5 §4.8.2.1、§4.8.3.3](https://uefi.org/specs/ACPI/6.5/04_ACPI_Hardware_Specification.html#power-management-timer-pm-tmr) 的 3.579545 MHz 自由运行计数器处理：使用 32 位寄存器读取，按 FADT `TMR_VAL_EXT` 屏蔽为 24 或 32 位。本实现支持已验证的 legacy I/O 端口或 32 位 System-I/O GAS；不支持的非零扩展 GAS 不会悄悄改用 legacy 地址。

校准时暂时屏蔽本地定时中断，以除数 16 启动一次性倒计时。每次 LAPIC 采样由前后两次 PM 读取夹住，拒绝超过 100 µs 的读取跨度。连续测量两个约 50 ms 的窗口，各窗口最多允许 60 ms；两次周期估值须相差不超过 5% 加一个计数的舍入容差，再用合计的实际 PM 间隔换算最终周期。等待过程不持共享锁，持续处理 membarrier 请求，并共享一百万次轮询预算。只有结果日志使用执行事务串行输出。

无参考时钟、参考停止、计数异常、窗口超时或两次估值不一致时，明确记录原因并恢复调用方提供的固定初值。此回退可继续抢占，但不能保证 100 Hz。轮询预算限制执行次数，不限制宿主机停止调度 vCPU 的墙钟时间。双窗口能够拒绝一次完整 PM 回绕造成的错误估率；多个暂停若恰好使两个窗口产生近似相同的别名偏差，仍可能通过。这需要另一独立可信时钟才能进一步消除。

## CPU 时间的含义

所有 CPU 的 busy、idle 和任务运行时间统一由 BSP 的 100 Hz 采样时钟结算，副核定时中断只请求抢占。精度仍为 10 ms，不使用未经跨核验证的 TSC 插值。这样即使定时器进入回退，CPU 统计也不会按错误的中断频率缩放。

调度器锁保护采样和 CPU 所有权切换；仍保留 CPU 的任务在交接前继续计时，包括已标记 BLOCKED 或 EXITED 但还未离开 CPU 的任务。pid 0 视为 idle，离线 CPU 不补算离线时段。统计读取不推进 EEVDF 的运行量和截止时间。

`/proc/stat` 的汇总与各核行来自同一份快照，并采用 [Linux proc 文档](https://docs.kernel.org/filesystems/proc.html#miscellaneous-kernel-statistics-in-proc-stat) 规定的 USER_HZ 单位，本架构为每秒 100 tick。`/proc/<pid>/stat` 保持已有的线程组汇总。`times()` 与 `getrusage()` 使用已结算的任务计数，rusage 的秒/微秒换算避免先乘造成溢出。

这次没有改变任务结构或 UAPI 布局，也没有重生成 ABI golden。现有实现尚未完整区分用户/内核/子进程 CPU 时间，`getrusage(RUSAGE_SELF)` 和 `times()` 仍保持原有的当前任务语义；本次修复不能视为完整 Linux/POSIX CPU 统计兼容性的认证。

## 验证方法

在仓库根目录运行主机回归：

```sh
mkdir -p out/tmp
TMPDIR="$PWD/out/tmp" make O=out/host-smp-time PROFILE=debug test-smp-time
TMPDIR="$PWD/out/tmp" make O=out/debug PROFILE=debug -j8 kernel
```

三个主机 fixture 直接编译产品源码，启用 ASan、UBSan 和 `-Werror`，覆盖 ACPI 描述校验、24/32 位回绕、完整 PM 周期暂停、校准故障与回退、不同 AP 中断频率、CPU 迁移/保留/离线/槽位复用和快照一致性。新增边界测试已先在修复前失败，再在修复后通过。

`tools/test_smp_time_qemu.py` 接受 `--image`、`--kernel`、`--sysroot`、`--out`，向私有镜像注入静态 musl probe 并更新两个 ESP 内核路径。它核对源镜像、回读内核与 probe、检查根文件系统，再以 1/2/4 vCPU 启动。guest probe 每核固定两个不主动让出的计算进程，检查抢占进展、USER_HZ、各核总时间、汇总一致性、`times()` 与 `getrusage()` 换算。

原始中断频率另用临时诊断内核独立核对，避免 CPU 统计本身掩盖定时器错误。诊断计数器和额外 proc 行已从产品源码恢复移除，并核对最终 ELF 没有相关文本和符号。

## 2026-10-20 验证记录

正式内核 SHA-256：`fb019ea8056b026af9ba3a66e6504b75f57e4071a919bbc532d54070342947fe`。

编译通过；三个新增 host fixture、nanosleep、timekeeper/vvar 和两个 procfs 功能 fixture 通过。相关调度恢复、CPU ID、执行锁、IRQ 输入、EEVDF 策略及红黑树独立测试也已通过。

| 环境 | vCPU | 内核 | 约 2 秒的每核原始中断 | 每核 CPU 统计增长 | 结果 |
| --- | --- | --- | --- | --- | --- |
| QEMU/KVM | 1、2、4 | 正式 | 未加诊断计数器 | 均为 200 tick | 通过 |
| QEMU/KVM | 2、4 | 临时诊断 | 均为 200 次 | 均为 200 tick | 通过 |
| VMware Workstation | 2、4 | 临时诊断 | 均为 200 次 | 均为 200 tick | 通过 |

VMware 的双核和四核正式内核也通过开机计算 probe。随后分别保持 2 个和 4 个 `yes` 负载进程，完整冷启动 GLX/X11 测量阶段的 aggregate busy 均为 100%；每个负载进程在图形测试和交互检查之后仍存活且 CPU 时间继续增长，随后逐个终止并等待回收。

两种配置各完成 18 组 X11 测量，均无 X 错误，像素回读通过；Xorg glamor 和 GLX 均确认 SVGA3D。实际帧缓冲截图经人工查看，确认基准色块完整、Fileman 中文界面与 12 项目录列表正常、窗口向左 25/向上 40 像素移动成功、VT2 显示文本控制台、VT1 返回原窗口且重绘完整。本轮不据此量化输入延迟或推断不同 vCPU 配置之间的性能提升百分比。所有测试虚拟机都已关闭。

本次日志、镜像来源与源码哈希、内核回读证明和失败的中间方案保存在排除提交的 `out/ap-timer-20261020/`。最终候选为 `candidate-e-*`、`e-clean`、`e-diag`、`e-native-diag*` 和 `e-vmware-load*`；总核验记录为 `candidate-e-validation.json`，截图与人工核验记录位于两个 `e-vmware-load*` 目录。更早命名含 `final` 的文件属于已淘汰中间方案，不作为最终结果。

## 已有失败与验证范围

没有把整套回归标记为通过。以下已有问题保留：Linux threads 的 membarrier fixture 引用已移除的 `smp_rdtscp_supported`；EEVDF 集成 fixture 的任务布局断言过期；SysV SHM fixture 缺少此前 DRM continuation API 的链接 stub；Taskmgr 两处 UI 文本断言过期。原始 HEAD 对照确认 EEVDF/SysV SHM 的失败可复现，相关 UI/测试文件也与父仓 HEAD 相同。音频产物缺失及既有 ABI golden 决策同样没有在本次任务中改动。

**VMware 验证未完成**：本次覆盖指定 vCPU 配置的计时、计算任务和桌面负载；完整发布镜像、安装/升级全流程、休眠恢复和物理硬件尚未验证。
