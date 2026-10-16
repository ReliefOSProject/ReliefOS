# x86-64 vDSO 时间接口

ReliefNT 通过初始栈的 `AT_SYSINFO_EHDR` 发布 `linux-vdso.so.1`。现有
musl 1.2.6 可以直接发现并调用它，无需修改 libc 或重新链接应用。

## ABI 与调用行为

ELF64 `ET_DYN` 导出 `__vdso_clock_gettime`、`__vdso_clock_getres` 及对应的
无前缀别名，符号版本都是 `LINUX_2.6`。它使用 SysV AMD64 调用约定和两个
64 位字段的 timespec，直接返回 0 或负 errno；POSIX 的 -1/errno 转换由
libc 完成。ELF 没有解释器、动态依赖、GOT 或运行时重定位。

| 时钟 | 用户态快速路径 | 其他情况 |
| --- | --- | --- |
| REALTIME / MONOTONIC | 可信单 CPU TSC 插值 | syscall 228 |
| REALTIME_COARSE / MONOTONIC_COARSE | 共享快照读取，无 RDTSC | 快照不可用时 syscall 228 |
| MONOTONIC_RAW / BOOTTIME / TAI | 尚未加速 | syscall 228 |
| 动态、负数、无效 ID | 交给内核判断 | syscall 228/229 的原始结果 |

REALTIME 的快速路径还要求有效的墙钟。`clock_getres` 对已启用的高精度时钟
返回共享的实际计数器分辨率，其余情况走 syscall 229；合法时钟的 NULL
输出指针仍成功，无效时钟仍报错。NULL `clock_gettime` 指针转入 syscall。
其他无效用户指针可能像 Linux vDSO 一样产生 SIGSEGV。

## 时间源和一致性

高精度路径要求恰好一个已发现且在线的 CPU、invariant TSC、用户态 RDTSC
权限、有效校准和受限的调时范围。校准逐个检查 PIT 间隔，要求连续两个
七间隔窗口各自的最大/最小间隔差不超过最小值的 1%，且两次估计频率相差
不超过 1%。拒绝异常窗口后从最新采样重试，未确认前保持回退。
仅 BSP 在线但已发现 AP 时也拒绝启用。
多 CPU 和不可信 TSC 环境继续使用内核 syscall，COARSE 仍可加速。

vvar 是内核与所构建 vDSO 之间的私有协议。写入者持有 `clock_lock`，以
奇偶序列和屏障发布完整快照；读者最多尝试八次，失败后执行 syscall。
RDTSC 采样位于序列验证之前，防止旧换算参数配上新时刻的计数器。

连续高精度基准独立于 PIT 的 COARSE 基准。tick、设置墙钟和成功调时先按
旧比例结算已经经过的时间，再发布新比例，避免负向 ADJ_TICK 导致边界倒退。
Q32 换算比例向上取整，覆盖 2.4 GHz 下逐 tick 向下截断丢失 1 ns 的问题；
它仍是有限精度换算，绝对准确度依赖硬件和校准。闰秒修正同步更新 REALTIME
与 TAI 关系。旧 syscall MONOTONIC 路径增加返回下限，防止 SMP/不可信时间源
在负向调时或跨 CPU 采样时倒退。

## 映射与生命周期

整个 ELF 使用一个 RX `PT_LOAD`；紧邻其前一页的 vvar 为 R/NX。VMA 的
`max_prot` 禁止 vvar 获得写入或执行权限，也禁止代码获得写权限。位置随机、
页对齐，并检查现有 VMA、页表、内核保留洞和最大用户栈窗口。

内核为物理页保留永久引用，各 mm 单独持有映射引用。fork 继承共享物理页，
exec 在新 mm 成功装载后才替换旧 mm，失败映射逐页回滚。首版不支持单独
mremap vDSO/vvar。主动 munmap 或 MAP_FIXED 覆盖自己的 vDSO 会使 libc
缓存的函数指针失效。

CPU 迁移探针还发现原有 syscall 快速返回路径在亲和性修改后仍可能恢复到
被禁止的 CPU。快速返回现在额外检查当前 CPU 是否在任务允许掩码中，否则
进入已有的完整调度流程；没有新增任务结构字段。

## 构建和验证

从父仓库运行，产物与临时文件留在 out/build 内：

```sh
mkdir -p out/tmp
TMPDIR="$PWD/out/tmp" make O=out/vdso-host-tests PROFILE=debug test-vdso
TMPDIR="$PWD/out/tmp" make -C kernel/reliefnt \
  O="$PWD/out/vdso-time-kernel" ARCH=x86_64 PROFILE=release -j8 all
```

`test-vdso` 检查真实 ELF 的段权限、符号版本、嵌入字节及实际函数调用；
并以 ASan/UBSan 检查真实 timekeeper、页表映射、引用计数、碰撞、失败回滚、
VMA 扩容和初始 auxv。时间测试覆盖负向调时、2.4 GHz 下 1,000 tick 的
9/10/11 ms 累计值、闰秒、墙钟修改、不安全时间源和并发快照。

客体探针使用已构建的 musl+mimalloc sysroot，测试静态和动态链接程序：

```sh
TMPDIR="$PWD/out/tmp" python3 -B tools/test_vdso_qemu.py \
  --image out/xorg-vmware-debug/images/reliefos.vmdk \
  --sysroot out/xorg-vmware-debug/sysroot/musl \
  --kernel out/vdso-time-kernel/generated/system/kernel.sys \
  --output build/vdso-example-kvm-1 --smp 1 --accel kvm --invtsc --require-hres
```

每次使用新的 `--output` 目录。`--require-hres` 等待启动校准并要求实际高精度
窗口验证，不能用回退冒充这项覆盖。双核用 `--smp 2`；软件模拟用 `--accel tcg`，
这两种回退测试移除 `--invtsc --require-hres`。该工具复制磁盘，只修改私有镜像；替换两个 EFI 内核路径后
读回校验 SHA-256，并核对源 VMDK 前后哈希。

探针核对 ELF/auxv、直接调用/libc/raw syscall 区间与分辨率、非法 ID、NULL
getres、权限、fork/exec、逐 CPU 迁移和负向 ADJ_TICK 后的单调性，并恢复调时
和亲和性。权限反向测试会触发预期的用户态页故障，不代表内核崩溃。
高精度探针还要求调用前后处于同一个偶数序列、TSC 均在有效窗口内，并将
结果与该快照的 Q32 换算区间比较。报告区分高精度能力已公布与有效窗口验证；
计时批次仍可能遇到过期窗口并回退，不能仅凭 mode 标志归类为全程加速。
性能数字为配对批次的序列化 RDTSC 中位数，只表示该次客体环境。

## 2026-10-16 验证记录

最终测试内核 SHA-256：
`df1cc964f42a915bcbe2a894fc5509143e8ae89148bc0fedd7c936e4fc5b0e92`。

- **编译：** 内核 `all` 通过，生成 kernel.sys / kernel.debug / kerneldebug.sys。
- **宿主测试：** `make test-vdso` 通过（ELF 实际调用，以及 timekeeper、映射、
  引用计数和 auxv 的 ASan/UBSan 检查）。原有 `test_openrc_network.py` 五项、
  `test_linux_time.py`、`test_eevdf.py` 和 `test_musl_abi.py` 均通过。
- **独立审查：** ELF/映射、时间代码、整体实现及后续校准/探针修改均已复核，
  已发现的问题完成修正和定向复审。

| QEMU 环境 | 静态 / 动态 musl | 高精度行为 | 证据目录（build/） |
| --- | --- | --- | --- |
| KVM，1 vCPU，启动负载 | 均通过 | 校准尚未确认，正确回退 | vdso-final4-kvm-1 |
| KVM，2 vCPU | 均通过 | SMP 回退 | vdso-final4-kvm-2 |
| TCG，1 vCPU | 均通过 | 不可信时间源回退 | vdso-final4-tcg-1 |
| TCG，2 vCPU | 均通过 | SMP / 不可信时间源回退 | vdso-final4-tcg-2 |
| KVM，1 vCPU，invtsc，require-hres | 均通过 | 校准稳定后，直接调用和 libc 均通过 Q32 窗口验证 | vdso-final5-kvm-1-hres |

所有运行使用同一最终内核，两个 EFI 路径均读回匹配哈希；源 VMDK 的前后
SHA-256 均为 `093bb45a2076e76315091e4a37eda3d185173369976ef693acfbf8fea2ca1163`。
双核探针确认在 CPU 0/1 上实际运行，所有配置通过负向 ADJ_TICK、恢复原值、
权限及 fork/exec 检查。

严格单核运行的校准为 3,493 cycles/µs；独立宿主测量约为 3,493.425 cycles/µs。
该次静态/动态 musl MONOTONIC 调用中位数为 94/95 cycles，raw syscall 为
1,729/1,727 cycles，约减少 94.5%；MONOTONIC_COARSE 为 32/33 cycles，raw 为
1,682/1,680 cycles，约减少 98%。高精度批次允许过期窗口回退，单独的稳定
窗口检查才用于证明实际快速调用。

启动验证中曾遇到 `reliefos-runtime` 的 `checkpath: chmod: Read-only file system`，
阻止依赖它的探针启动。探针只依赖注入的程序、库和串口，现在在 localmount
之后运行；该应用状态目录启动问题保留为单独问题，不能将探针通过当作完整
桌面启动通过。实现前的 `test_linux_memory.py` 四个夹具编译/链接失败也未修复；
具体基线见[设计记录](superpowers/specs/2026-10-16-vdso-design.md)。原有
timekeeper 夹具只补齐缺失的设备 tick 与 CPU 拓扑桩，测试断言保持原样。

## 验证边界

本实现遵循 Linux x86-64 vDSO 的发现/符号 ABI，并测试相应 POSIX 时间结果
约束；这不等于整个系统获得 Linux/POSIX 兼容性认证。RTC 无效时旧 REALTIME
使用 monotonic 的 Epoch 语义缺口仍存在，该状态不启用 REALTIME 快速路径。
PIT 窗口一致性检查用于拒绝受扰动的启动采样，尚无独立 HPET/PMTIMER 交叉
校准或运行时 clocksource watchdog。SMP 高精度加速、跨 CPU TSC 同步、
半虚拟化时钟、32 位 vDSO 和休眠恢复尚未
覆盖。**VMware 验证未完成；真实硬件验证未完成。**

设计依据：[Linux vDSO ABI](https://man7.org/linux/man-pages/man7/vdso.7.html)、
[Linux x86 ELF 布局](https://github.com/torvalds/linux/blob/v6.12/arch/x86/entry/vdso/vdso-layout.lds.S)、
[Linux 映射实现](https://github.com/torvalds/linux/blob/v6.12/arch/x86/entry/vdso/vma.c)、
[Linux TSC 校准中的受扰动采样处理](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/tsc.c)、
[POSIX.1-2024 时钟定义](https://pubs.opengroup.org/onlinepubs/9799919799/functions/V2_chap02.html)。
