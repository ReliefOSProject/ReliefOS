# ReliefNT 模块化重构验证记录

本次按职责重组内核并拆分混合职责实现，保留既有算法、锁和中断约束、初始化
调用顺序、Linux syscall/UAPI 与输出路径。不引入可加载模块。
本记录对应隔离工作树中的提交前验证，最终提交与远端状态以 Git 历史为准。

父仓基线为 `02926e5aa3531cfc8285c80b264b49b7dffe9f8b`，
ReliefNT 基线为 `dd0d60f776cec88e84ec62f323963e080fba4483`。
实际执行范围与保留的限制如下；未执行的项目不算通过。

## 模块和构建

- 核心按 init、scheduler、syscall、device、signal、time、IPC、sync、random、exec
  划分；syscall、调度器、网络、存储、console、framebuffer、HDA、PCM 和 vmwgfx
  的混合职责文件继续拆分。目录和私有状态归属见
  [内核模块说明](../../kernel/reliefnt/docs/KERNEL_MODULES.md)。
- 当前共 90 个本地生产 Kbuild，281 个编译对象；其中独立 debug 和 vDSO clock
  对象不进入主内核归档，实际内核归档有 279 个唯一成员。
- 根 Kbuild 仅遍历子系统并集成产物，各目录显式登记对象和直接子目录。
  生产源码不通过包含实现 `.c`/`.inc` 组装。宿主白盒测试的组合头仅在测试目录。
- 私有头移到 `include/reliefnt/`；85 个公开 UAPI 头逐字节保持不变。
  Linux 6.12 Kbuild/Kconfig 引擎的 137 个受校验文件未改。
- [历史源码映射](../../kernel/reliefnt/docs/kernel-source-map.json) 只用于审计，
  不参与生产构建。当前工具、测试、构建和 CI 消费者已检查映射中的旧路径。
  带日期的历史记录保留当时的路径和行号，不能当作当前构建输入。

## 编译、增量和打包

| 层次 | 实际验证 | 结果 |
| --- | --- | --- |
| Release | 独立内核、父仓 `make kernel`、clean 后重建 | 通过 |
| Debug | `PROFILE=debug clean` 后 `-j8 all` | 通过 |
| 增量依赖 | `tools/test_build_incremental.py` 的完整生产构建测试 | 通过 |
| 镜像 | `make O=out/kbuild-verified -j8 image-vmdk` | 通过 |
| 产物身份 | 从实际 VMDK/ESP 和根分区读回 kernel、Fileman、Taskmgr、runtime | 与当前构建 SHA-256 一致 |

增量测试覆盖源码、私有头、UAPI、C 参数、链接脚本、同路径替换归档工具、缺失
对象和产物；还验证了 fresh `build-info` 的宿主工具前置依赖、clean 清理 lib/sound
并保留未知文件和配置、clean 后重建字节一致，以及无改动时不编译、不重新链接。
测试在隔离源码和输出中执行，不改主工作区。

运行测试使用的内核 SHA-256：

```text
b2a5266d7f11744dce3ae722078fd75caf1f02946d66435ab504eec72ad56bfc
```

对应 VMDK SHA-256：

```text
7864cf3d16a96c209d8062f6b8387ac963167839b3dde7232ba1d082c2e0cf34
```

## 源码和宿主回归

- 包含原 EEVDF `.inc` 的函数体审计：3,548 个函数体 token 一致，没有遗漏。
  唯一改写是 `syscall_dispatch_regs_legacy` 按职责转发；只读审查核对了条件和
  顺序，IOCTL 前缀及最终 `ENOTTY` 路径保持原顺序，未知 syscall 仍返回 `ENOSYS`。
  token 对比不替代运行验证，也不声称证明全部语义。
- 基线 1,637 个外部符号全部保留，符号类型未变化。新增的内核内部跨文件符号
  不属于用户 ABI；原 `struct task` 字段声明也做了 token 一致检查。
- Release/Debug 的宿主工具、ASan/UBSan、85 头导出、自包含 C/C++、vDSO ELF/符号
  版本/运行时、Logo（9,696,607 checks）、panic（124,345 checks）、模块布局检查通过。
- 50 个既有父仓宿主 wrapper 与基线一致：38 通过，12 失败，没有新增失败。
  EEVDF 红黑树不变量和差分策略 oracle 单独通过。ext4 VFS 按 required 参数分别
  指定 ext2/ext4 后通过，包含 Linux `e2fsck` 校验。
- 内核版本/procfs、公开头边界、桌面绘制、镜像/GRUB 适配和内核适配器验证通过。
  `tests/long/test-jobs.sh` 最终 19 项检查通过，覆盖 `-j1/-j8` 干净构建、三轮
  增量选择、可比产物字节一致、构建中断及恢复。

既有 12 个 wrapper 失败：`eevdf`、`poll_wakeup`、`linux_memory`、`linux_threads`、
`linux_process_vm`、`linux_fifo`、`linux_sysv_shm`、`linux_ioctl_cloexec`、
`ext2_write_batch`、`ext4_vfs`、`storage_metadata`、`drm`。
其中有旧 task 大小/偏移断言、fixture 缺符号/旧签名、缺临时目录和 runner 缺参数等
问题；本次没有修改 golden 或生产实现来掩盖这些失败。`ext4_vfs` 的无参数调用
失败仍保留在 50 wrapper 比较中，正确参数的通过结果单独列出。

父仓默认 `make test` 仍因 `soundctl` 的 APK 组件所有权配置不一致而失败；相关
策略、组件和测试输入与上述验证基线字节一致。定向品牌测试仍有主仓 20 条既有未
归类旧名称记录，已与验证基线核对相同。本次只迁移了内核既有兼容名称白名单的
精确文件位置，没有扩大豁免范围。ABI golden 的既有差异和音频 manifest 策略未改。
子仓最终 `make -k -j8 all test` 返回 2，唯一失败目标是这个既有 `test-abi`：
`reliefos_driver_kernel_api` 的旧 golden 大小为 168，当前头定义为 296；原基线已有
相同差异。其余目标继续运行并通过，没有重生成 golden。

迁移适配中的 fixture 修正均有实际失败证据：HDA 流测试的条件定义恢复；DRM
测试增加仅在意外进入音频分支时 abort 的链接 stub；头边界改查真实导出字节；
内核适配器改识别 Kbuild 的实际对象名；源码副本不再错误排除生产 `mm/cache`。
长测试的 APIC/boot `.cmd` 路径也同步到新目录；失败记录保留，修正后完整重跑通过。

## QEMU 运行

运行于 QEMU/KVM q35、SVGA II、4 GiB，每次仅一台本任务虚拟机。测试只写私有磁盘
副本，源镜像的哈希在结束后核对不变。

| 场景 | 证据和结果 |
| --- | --- |
| 2 CPU 启动、静态/动态 POSIX I/O | 各 52 项检查通过 |
| XDM → IceWM | 登录成功；Fileman、Taskmgr 实际绘制、中文控件可见且正常关闭 |
| TTY panic | 当前 tty2 输出；寄存器、故障地址、停止另一个 CPU、稳定显示检查通过 |
| GUI panic | 模拟显示锁被占用时通过；876,288 个文字区域外像素不变，保留 227,846 个非黑桌面像素 |
| 1 CPU 与 2 CPU 编译负载 | 未修改的 GCC 15.2/assembler/make 私有 fixture，`make -j4` 各完成 12 个对象；两次来宾检查均返回 0 |
| `yes \| dd` | 两种 CPU 配置各传输 536,870,912 字节；本次约 1.3/1.5 GB/s，不是跨平台性能保证 |
| e1000 DHCP/TCP | 获得 `10.0.2.15`，连接主机 fixture 成功，主机收到 HTTP 请求 |
| HTTP 完整读取 | 未通过：wget 报 Malformed status line 并返回 4；同镜像换用重构前内核出现相同错误，未将其计为通过或新增回归 |

编译器 fixture 从已有 Alpine 工具链产物复制到私有来宾 `/opt/km`，没有把编译器
写入正式镜像，也没有替换系统 libc。完整网络失败证据保留在 `out/kmw1d/` 和
重构前内核对照 `out/kmw1e/`；比较记录为
`out/kernel-modular/network-baseline-comparison.json`。

双核负载来宾的串口完成标记、退出码 0 和源镜像不变检查均已保存；该宿主 runner
在输出汇总及保存结果后，工具会话记录退出码 143。因此这里仅确认已观察到的
来宾编译/管道结果，不把宿主清理流程计为成功。原汇总误带的 HTTP 字样已在日志
追加更正，双核负载测试本身没有执行 HTTP。

本次 QEMU 的 SVGA II 不提供完整 VMware SVGA3D，不能据此证明 VMware 3D 加速或
物理设备上的性能。**VMware 验证未完成**，物理硬件未验证。

## 本地证据

详细证据位于本工作树 `out/kernel-modular/`，不提交构建产物：
`final-body-audit.json`、`final-symbol-audit.json`、`incremental-final.log`、
`final-debug.log`、`image-build.log`、`final-host.json`、`brand-baseline.json`、
`final-review.md` 及各定向测试日志。
桌面截图/结果在 `out/kmq/`，panic 在 `out/kmt/` 和 `out/kmg/`。
单核编译/管道记录在 `out/kmw1b/`；双核负载结果在 `out/kmw2f/`。

只读最终审查发现的 LTP 上游路径误替换和驱动目录文档遗漏均已修正并复核。
此记录说明实际覆盖范围，不宣称不存在任何未发现的缺陷。

## 提交集成说明

用户随后授权提交和推送。提交时接入 ReliefNT 的 `2d64ed5` 发布文档更新，以及
ReliefOS 分支截至 `ce246574` 的发布文档、署名和 ReliefMail/CA 更新，保留这些
远端内容。本次重构的内核源码没有因这些更新改变。上面的镜像和 QEMU 记录仍
对应明确列出的原验证基线，不作为新增远端用户态代码的运行验收。

## 发布后 CI 漏文件修复

ReliefNT Actions run `39622766793` 在 `7b502f5` 的干净检出中失败：
`mm/Kbuild` 引用 `mm/cache/`，但子仓的 `cache/` 忽略规则把生产目录中的
`Kbuild` 和 `page_cache.c` 一同排除，导致两个文件未进入提交。本地源码副本
包含它们，因此先前的本地构建通过并不能证明发布提交完整；这是该检查的遗漏。

修复把下载缓存规则收窄为根目录 `/cache/`，补入原有两个生产文件，并让模块
布局检查调用 Git 实际忽略规则，拒绝被忽略的生产源码和 Kbuild。无 Git 元数据的
源码归档继续执行结构、源码覆盖和上游引擎校验。CI 在下载依赖前执行这项静态检查。
恢复的 `page_cache.c` 与重构基线的 `mm/page_cache.c` 字节一致。

本地以原提交的 Git archive 复现 `missing Kbuild: mm/cache` 和 `make all`
退出 2；从临时 Git index 导出的完整修复候选在独立输出目录执行 `make -j4 all`
退出 0，生成 `kernel.sys`、`kernel.debug` 和 `kerneldebug.sys`，包括实际编译的
`mm/cache/page_cache.o`。测试先在旧规则下报出两个 ignored production input，
收窄规则后通过。证据保存在 `out/dualboot/logs/ci-{before,fixed}-*.log`。

同一干净源码候选的 `make -k -j4 test` 退出 2，唯一失败仍是上文列出的
`test-abi`（168 → 296）；其余宿主、ASan/UBSan、头导出、vDSO、Logo、panic
和模块布局目标通过。未重生成 golden。本修复未改变内核执行逻辑；未重新运行
QEMU 或 VMware。**VMware 验证未完成**。

用户于 2026-10-22 授权先提交推送本修复。ReliefNT 修复提交为
`dbf3b294470c78f7b13e0653f6f35eb75e8b10de`，已先推送至 `main`，触发
[新的 Build ReliefNT run](https://github.com/ReliefOSProject/ReliefNT/actions/runs/39690836331)。
记录此时远端构建仍在运行，本地干净源码编译通过不替代其最终结果。
父仓接入同分支 `2799815e` 的计算器更新后，只提交本修复的子模块引用和此记录，
未将双启动设计、计划或实现纳入本次修复提交；不把旧 QEMU 证据用作计算器更新验收。
