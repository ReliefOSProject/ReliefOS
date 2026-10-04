# Intel HDA / Linux 音频 / 声音控制 / Doom 项目进度

更新：2026-10-03。唯一实施目录：
`/home/xiaobai/.codex/worktrees/intel-hda-audio-plan/ReliefOS`。

最新状态：H1–H6、A1–A7 的**本地实施门槛已闭合**，U1 上游运行库/工具/SDK 和 U2 控制库/CLI
已闭合。按任务等权计数，当前为 **15/19 项，约 79%**；该比例不是最终可用程度或工时比例。
接替时 H1/H2 已完成；接替后收尾 H3/H4/H5/A1/A2/A3/A4/A5/A7/A6，共 10 项。
此前正式计数为 7/19，本次补齐 A3-A5 的本地合同并更新账本；批准计划明确将
真实 VM 验证置于 H6/U6、上游 ALSA 应用运行置于 U1/U6；H6 已取得实际播放和录音证据，已通过 H6 最终内联审查。

| 状态 | 任务 | 已有结果或剩余范围 |
| --- | --- | --- |
| 本地门槛已闭合，15 项 | H1–H6、A1–A7、U1–U2 | HDA/AC97/ES1371 VM 与波形、host fixture、上游 ALSA/OPL3、目标 SDK 链接 |
| 后续未完成，4 项 | U3–U6 | 设置页、Doom 音效、Doom 音乐、最终软件/真机矩阵 |

后续按批准依赖顺序执行 U3–U6。
目前已有 HDA 模块实际加载、客户机播放/录音、生命周期、控制恢复及原生 SHM 验证；
真机声音、ALSA 用户应用、设置页和 Doom 有声运行尚未通过。

最新进展与证据见 SDD progress 第 37–38 节；下方内容保留各次继续工作的历史状态。

## 本次继续工作的证据

A5 的控制事件阻塞读取现在使用实际等待队列与调度器；事件 epoch 在检查空队列前
获取，发布睡眠后再次检查，修正通知先于睡眠而丢失唤醒的时序。两个读者、通知
在睡眠前后到达、卡断开、待处理信号、队列满时保留可运行状态均有主机 fixture。
这不构成客户机 syscall、信号 handler 或完整 OFD 生命周期的验收。

Linux v6.14 控制读取参考同时确认未订阅返回 `-EBADFD`；对应回归先失败再通过。
原始参考来源与 SHA 记录在本次交接追加节中。

- 竞态有效失败：`task-10-control-blocking-races-red-2.log`。
- 竞态修复通过：`task-10-control-blocking-races-green.log`。
- ABI、PCM、device、params、ioctl、mmap、control、OSS、mixer 九组回归：
  `task-10-*-regression-blocking.log`，均 exit 0。
- 未订阅读取：`task-10-control-unsubscribed-red.log`、
  `task-10-control-unsubscribed-green.log`，分别失败与 exit 0。
- 根入口最终构建：`task-10-root-build-blocking-final.log`，普通
  `make O=out/audio-hda -j8 kernel drivers`，exit 0；保留已有编译警告。

仍保持内联实现与审查；没有新增子代理，没有提交、推送或归档。

本次改动与文档的空白检查通过。子仓全量检查仍报告 Linux `soundcard.h` 导出头的
既有空白；导出适配版本与上游原始 fixture 均保持 A1 已验收的 SHA256 不变。
完整检查记录保存在 `task-10-blocking-final-audit.json`；修正基线比较假设后的
定向检查保存在 `task-10-blocking-final-audit-3.json`。第二次审计的错误比较假设
及失败结果也已保留并在交接文档中说明。

## 最近的 A5 收尾进展

控制事件改为每个 OFD 预分配、每个固定控件一个 pending mask，并按首次通知顺序
读取。修复了读取旧事件、查询 ID 期间新通知被误标为已读的竞态，以及旧的 256 条
历史窗口让慢读者溢出的问题。订阅 ioctl 现在支持负数查询、任意正数启用，重复
启用保留积压，关闭订阅清空积压，符合已保存的 Linux v6.14 参考。

- 有效失败证据：`task-10-pending-race-red.log`、
  `task-10-pending-burst-red.log`、`task-10-subscription-query-red.log`。
- 最新控制测试：`task-10-subscription-queue-final-green.log`，exit 0。覆盖读取中
  新通知、1024 轮慢读者积压、订阅查询与顺序、关闭复用、64 个 OFD 各自覆盖
  64 个控件，以及先前的真实等待队列/调度器竞态。
- 其余八组主机回归：`task-10-*-regression-pending-queue.log`，全部 exit 0。
- 普通根入口构建：`task-10-root-build-pending-queue.log`，exit 0；本次构建
  kernel.sys，drivers 无需重建，仍不构成新 HDA 模块链接或客户机加载证明。

上述局部修复没有新增正式完成项。接下来仍须收尾真实 descriptor 的 dup/fork/
SCM_RIGHTS/最后关闭、阻塞与信号、VMA 生命周期及 ioctl/errno 的合同审查。
A5 简报明确固定控件集；无需新增用户定义控件或动态注册控件 API。

## 27. A3-A5 内联收尾（2026-10-02）

本次用户要求优先完成 A3-A5，所有实现均在唯一 worktree 内联完成，没有使用子代理。
正式等权计数仍为 7/19；A3-A5 仍标记为部分实现，因为客户机 syscall、真实 `/dev/snd`
节点枚举/权限、HDA 模块加载以及物理声音/Doom 证据尚未完成。

已落地的生产修复包括：

- A3 标准 PCM ioctl 增加已实现命令门控；未知命令在检查用户指针前直接返回 `ENOTTY`。
  设备节点的 PCM 传输方向继续按 OFD 权限拒绝错误方向访问。
- A4 `snd_pcm_status` 现在返回实际 START 时钟的 `trigger_tstamp`；软件
  `silence_threshold/silence_size` 保存并在播放事件低水位时向 DMA 环填充零样本，
  不再只接受字段而不执行语义。
- A5 OSS 路径补齐方向权限、真实 PCM 能力格式查询（含 U8）、硬件 period/buffer
  边界校验、RESET 的 DROP/释放、SETTRIGGER 的暂停/恢复或重新准备、DRAIN 的
  epoch 等待、非阻塞 `EAGAIN`、以及基于硬件帧指针的 `GETOPTR/GETIPTR`。
  OSS fixture 增加调度/等待链接桩，仅用于让生产等待分支保持链接检查；fixture 对
  PCM 指针的断言已改为硬件位置语义。

最新证据均为独立创建文件：

- `task-10-inline-a3-a5-final-audit.json`：A3-A5 十组主机 fixture 与根构建，全部
  exit 0。
- `task-10-inline-a3-a5-final-audit-2.json`：警告修正后的 OSS、ALSA ioctl、device
  回归及 `make O=out/audio-hda -j8 kernel runtime sdk`，四项全部 exit 0。
- 对应输出分别保存在 `task-10-inline-a3-a5-*-final*.log`；六个生产/fixture 文件的
  SHA256 同时写入两个审计 JSON。
- 随后加入未知 `A/0x7f` ioctl 的坏指针回归，`task-10-inline-a3-a5-device-final-2.log`
  与 `task-10-inline-a3-a5-root-build-final-3.log` 均 exit 0；最终源文件哈希保存在
  `task-10-inline-a3-a5-final-audit-3.json`。

主机 fixture 通过不等于 A3-A5 正式验收：仍缺标准应用/guest 探针、真实 devfs
`dents/stat` 与权限验证、信号重启和完整跨进程 OFD 生命周期合同审查，以及 HDA
模块加载、客户机播放/录音和物理/Doom 声音结果。后续依旧按批准顺序进入 A7、A6、
H6、U1-U6，且不改写既有失败证据。

## 28. A3-A5 本地实施门槛收尾（2026-10-02）

本次按用户“尽快完成 A3-A5”要求内联实现与审查，没有新增子代理。
A3-A5 的源码/host/build 门槛闭合，允许按批准顺序进入 A7；上游应用兼容与实际出声尚未验收。

- A3：真实 devfs helper 按 caps 枚举稀疏 device/direction，修正 OSS stat minor；
  syscall ioctl 持有原 OFD 跨阻塞/close/fd reuse。真实 descriptor/SCM_RIGHTS
  fixture 覆盖 dup/fork/进程退出、丢弃/回滚 ancillary 引用与最终释放；录音 DAC
  与 read/write/ioctl 的 SA_RESTART 递送有生产代码 host 回归。
- A4：LINK 使用直接 fd 的实际 descriptor 查找；trigger_tstamp 返回实际 START 时钟。
  silence 不再伪增加 appl_ptr，连续模式清理已播放区域；IRQ 先导入 mmap control
  再静音，避免覆盖新提交样本。实际 VMA 分裂、PROT_NONE fork、close/disconnect/last unmap 通过。
- A5：OSS open 持有首个支持 PCM 的硬件租约；格式/速率/声道协商回写，U8 与 D2 稀疏
  设备真实配置；RESET/trigger 使用 DROP，SYNC 补齐 partial/full tail 并实际 drain；
  读写等待防 lost wakeup，XRUN 返回 EPIPE。三声道 fragment 按完整 frame 协商，buffer
  保持整数个 period；queue/blocks 采用真实 boundary。control unknown 在坏指针检查前返回 ENOTTY。

最终选择 **17 个 host 验证命令及 1 个普通根构建，全部 exit 0**：
音频 abi/pcm/device/params/ioctl/mmap/vma/control/oss/mixer，descriptors、ioctl_cloexec、
permissions、memory、threads、HDA controls、header_export，以及
`make O=out/audio-hda -j8 kernel runtime sdk`。OSS 含 13 个真实 sched/wait/PCM 边界模式。
CLOEXEC 对照为宿主 Linux 的 38 checks、0 failures、0 skips，不是声卡或 guest 探针。

- 审查矩阵：`.superpowers/sdd/2026-09-30-intel-hda-audio/task-8-9-10-inline-closeout-review.md`。
- 最终审计：同目录 `task-10-a3-a5-closeout-audit.json`，逐项日志、源码与实际产物 SHA256。
- 最新受影响日志：`task-10-closeout-oss-final-6.log`、
  `task-10-closeout-control-final-2.log`、`task-10-closeout-descriptors-final-7.log`、
  `task-10-closeout-root-build-final-6.log`；其余选择在 audit 中逐项列出。
- 通用 ABI 与 syscall 支持清单同步更新至 `docs/ABI.md`、`docs/SYSCALLS.md`。

旧聚合 JSON 的 CLOEXEC 失败、SDK/PAM 缺失、错误 fixture 设置及所有语义 RED 均保留并区分。
旧 A7 草稿已隔离生产 syscall/VMA 接口与导出名单，不能算原生 SysV SHM 实现完成。
下一步 A7 必须复用当前 MM/VMA 持有关系并验证实际共享页释放，然后进行 A6/H6/U1-U6。
没有提交、推送或归档。

## 29. A6 本地实施收尾（2026-10-02）

同源标准 libc 探针已实现 `abi/play/capture/lifetime/controls`，固定 10 秒截止，
保留 byte/frame 短传输，处理 `EINTR/EAGAIN` 与零进度。控制测试在全部值快照后
才修改，并在正常、错误、捕获信号返回路径恢复与读回核验；某个恢复失败不跳过
其他值，并记录失败。SIGKILL 无法执行用户态恢复，不作为已保证的退出路径。

A6 同时发现并修复 A5 syscall control 1024 字节上限拒绝实际 1224 字节
`snd_ctl_elem_value` 的问题；新增生产适配层 RED/GREEN、完整传输/坏指针/边界回归。
根导出白名单补齐 sound/asound、sound/tlv、linux/patchkey，time UAPI 同时尊重
glibc/musl guard。实际 SDK 编译与两种 include 顺序证明安装闭包。

最终 host 聚合、UAPI、98 个导出头、Linux ABI contract、musl+mimalloc、客体构建
及普通根 kernel/runtime/SDK 构建全部 exit 0。97 项逐 ioctl 条件和第 6 节合同映射
在 `docs/audio-abi-support.json`。审查与源/产物/log SHA 位于
`.superpowers/sdd/2026-09-30-intel-hda-audio/task-11-report.md` 和
`task-11-closeout-audit-2.json`。初始审计与旧失败文件名带 green 的两次设置失败保留并排除验收。

尚未运行客户机音频 I/O、上游 ALSA、物理设备或 Doom。接续 H6，所有任务继续内联。

## 30. H6 正在进行（2026-10-02）

HDA 模块已实现所有 HDA BDF 与每个 codec 的发现/卡注册、真实方向 SD 租约、
能力交集、route/stream 接线，以及失败 DMA teardown 的 manager 保留与重试。
实际 ELF init rollback/unload、20 个模块分配失败点及既有五模块并存都有 host 证据。
普通根入口完整构建 VMDK、Live ISO、Installer ISO 成功；安装、发布、rootfs 与
manifest 产物检查通过。构建缓存仅从原工作区读取，按 lock SHA 校验后复制到本 worktree。

`out/audio-hda/qa/hda` 已证明 HDA 客户机加载；`qa/hw1` 经正常 OpenRC 启动标准
libc 探针，生命周期和声音控制通过，播放与录音 PREPARE 返回 EIO，波形未通过。
已新增真实 QEMU 固定 pin 行为的有效 RED 和 GREEN，严格限定六个已知 QEMU codec
ID；其他 codec 仍执行关闭/读回验证。正在重建并重跑客户机。
`task-12-host-final-1.log` 的 phase 链接失败已补齐无 sched waiter 的 fixture 边界，
`task-12-fixed-pin-green-1.log` 的 codec 与 phase 均 exit 0，旧失败证据全部保留。

H6 仍欠：有效播放波形、有限 capture syscall 与实际输入数据证据、完整 VM 拓扑矩阵、
多 codec 任务上下文的故障/控制事件处理、旧音频驱动 v1 adapter 和最终审查。
此后依序 U1–U6；A3–A5 不需要重新规划或实现。继续内联，未提交/推送/归档。


## 31. H6 真实播放与录音检查点（2026-10-02）

A3–A5 本地实现继续保持闭合，整体仍为 12/19。H6 尚未整体接受。

- `qa/hw5`：PC、1 CPU、MSI off、hda-output；实际双声道 1000/2000 Hz、约 2 秒 WAV，标准 libc 播放/生命周期/控制及 SHM 通过。
- `qa/hc2`：q35、4 CPU、MSI on、hda-duplex；实际输入后端录得 4096 帧/16384 字节，4095 帧与注入 400/800 Hz 序列匹配（首帧静音，样本容差 2）。
- `qa/hc3`：ICH9、q35、4 CPU、MSI off、双 codec 的 card1；上述播放/录音及所有 guest 模式通过。
- `qa/hc4`：PC、1 CPU、MSI on、hda-duplex；上述播放/录音及所有 guest 模式通过。
- `qa/nd1`：无音频设备、4 CPU；正常启动，HDA 不注册卡。仅 boot/module-state 验证。

各有音频运行的原生 SHM 为 83 项成功，退出前清除自建 segment。hw5/hc2 的旧 result.json 错误硬编码 82 导致 SHM boolean false；串口原证据为 83 PASS，保留旧 JSON，不覆盖历史。修正后的 hc3/hc4 正确接受 83。汇总与证据 SHA256 为 `task-12-vm-audio-checkpoint-1.json`。

生产修复均有有效 RED：固定 QEMU pin 不可变 quirk（仅六个明确 vendor ID/方向）、MSI off RIRB RESPONSE 状态/确认、SD 正常环回和累计完成帧、DRAIN partial-period 尾静音与 STOP 成功后才转 SETUP。录音 probe 改为 DROP、fsync、精确 fstat、close 成功后报告 PASS；runner 只修改私有完整副本并核对原 VMDK SHA 不变。QEMU 音频容器长度未收尾时另写 normalized WAV，只改 RIFF/data 长度，不改样本，原始文件保留。

仍欠 H6 重复模块加载前排他、旧 MSI 延迟投递安全、控制器错误对全部 codec 的 task-context 处理、jack/auto-mute 接入、legacy v1 OSS 边界、最终完整产品构建与审查。之后按 U1–U6 执行。没有子代理、提交、推送或归档；不声称物理声卡或 Doom 可用。


## 32. H6 生命周期与 legacy OSS 运行检查点（2026-10-03）

A3–A5 已完成本地源码/host/build 合同；整体仍为 H1–H5、A1–A7 共 12/19 项。
H6 尚未整体接受，后续 U1–U6、上游 ALSA、Doom 与真机验收未完成。

最新修复包括实际 ELF init 前同名模块排他、旧 MSI vector 至 reboot 不复用、
v1 OSS backend-generation 独占绑定与真实能力限制、有界完整 frame 批次、满环
DRAIN 尾静音不覆盖队首，以及 AC97 将 idle silence 排除应用待 drain 字节。
有效 RED 与全部失败/设置错误日志保留；最新 host 验证和普通根完整三种镜像构建通过。

最新 `qa/hc5` 通过 ICH9/q35/4 CPU/MSI off 双 codec card1 的实际播放、录音、
生命周期、控制恢复与 83 项 SHM。生产 image/probe/runner SHA 与运行记录一致；
原 VMDK 未改。AC97 `qa/ac-full2/3` 通过 OSS 提交和 SYNC，但波形存在重复/间隙，
连续播放质量仍未接受；`qa/es1` 为 ES1370/ES1371 硬件身份不匹配的失败记录。

本轮内联审查为 `task-12-inline-runtime-checkpoint-review.md`，精确 source/product/
log/runtime SHA 为 `task-12-runtime-checkpoint-2.json`。全部位于既定审计目录。
剩余 H6：所有 codec 的 task-context fatal-fault 处理、按 CAD 分发 jack/auto-mute、
tiny-buffer/tick-gap 回归、legacy 波形质量与 ES1371 实际后端、最终 H6 审查。
此前第 31 节的模块排他/MSI/v1 适配待办已有上述局部闭合；历史内容不删除。

根空白检查通过；子仓保留 canonical soundcard/ipc UAPI 空白，排除这两处后本轮
定向检查通过。所有写入继续限定 worktree；无子代理、提交、推送或归档。


## 33. H6 CAD 路由与小缓冲区检查点（2026-10-03）

正式进度仍为 12/19；A3–A5 已闭合，H6 尚未整体接受。全程内联，无子代理或 Git 写操作。

真实 controller/controls fixture 已修复按 CAD 提取 unsolicited 事件，保留其他 codec 的 FIFO 顺序及 wrap；控制服务共享 transport/event/sense 总预算，不再重复消费完整预算。真实 stream fixture 使用 24 MHz WALLCLK 与 100 Hz tick 下界检测小缓冲区整圈漏判，并覆盖计数回绕、暂停恢复和长时间间隔。各修复均先出现有效语义 RED；最新受影响 sanitizer host 回归为 `task-12-cad-wallclock-final-green-3.log`，exit 0。

普通根入口 `make O=out/audio-hda -j8 kernel drivers image-vmdk iso installer` 的 build-9 成功，六模块产物检查与 hda.drv 无未解析符号通过。最新 `qa/hc6` 在 ICH9/q35/4 CPU/MSI off/双 codec/card1 上通过标准 libc 播放、4096 帧实际录音、生命周期、控制恢复和 83 项 SHM；双声道 1000/2000 Hz 波形约 2 秒，录音 4095 帧与 400/800 Hz 注入匹配（首帧静音、容差 2）。原 VMDK SHA 不变。

精确 source/product/log/run/result SHA 保存于 `task-12-runtime-checkpoint-3.json`，前次检查点及所有失败日志均保留。CAD 路由和 tiny-buffer 回归已局部闭合；production jack/auto-mute 接入仍未完成。下一断点为所有 codec 的 task-context fatal-fault 处理，随后 legacy 连续波形与兼容后端、H6 最终审查、U1–U6。


## 34. H6 全 codec fatal 与 PCM 永久断开检查点（2026-10-03）

A3–A5 本地合同已闭合，整体仍为 12/19；H6 尚未整体接受。所有工作继续内联执行。

新增 append-only audio_request_disconnect：IRQ/tick 只请求，核心按同 owner 与精确 BDF 关闭全部 codec admission，发布 DISCONNECTED/ENODEV；task wait phase 处理 STOP/disconnect 与退休。失败 STOP 保留 module/provider/PCM DMA，最终 close/munmap 不提前回收。PCM 所有状态转换、linked START 回滚及晚到通知均保持永久断开；18 条真实故障回调测试覆盖成功/EIO、8 条操作与自动启动写入，已有 short progress 保留。

全部有效 RED/设置失败/旧聚合日志保留。最终回归 task-12-fatal-callback-error-green-1.log 全部 exit 0；普通根全镜像 build-13 exit 0，六模块/stage 检查及无未解析符号通过。最新 hc9 使用该 VMDK，ICH9/q35/4 CPU/MSI off/双 codec/card1：48 kHz 双声道 1000/2000 Hz 约 2 秒播放、4096 帧实际录音（4095 帧匹配 400/800 Hz、首帧静音、容差 2）、生命周期、控件恢复和 83 项 SHM 全通过；QEMU exit 0，原 VMDK 未改。

精确身份与运行记录为 task-12-runtime-checkpoint-4.json，内联审查为 task-12-inline-fatal-checkpoint-review.md。fatal 故障注入是 host fixture 证据，尚无 VM fatal 注入。下一断点：production jack/auto-mute 按真实能力发布与 task-context 服务，legacy 波形质量/ES1371 实际兼容后端、H6 最终审查，再 U1–U6。所有未提交源码和旧审计证据保留，无子代理或 Git 写操作。


## 35. H6 耳机控件发布与任务服务检查点（2026-10-03）

A3–A5 已完成本地源码/host/build 合同，最新全部受影响回归通过；整体仍为 12/19。H6 尚未整体接受，继续内联，无子代理、提交、推送或归档。

私有 card ops 追加尺寸保护的可选 task_service，tick 仅合并任务，manager wait phase 释放 execution ownership/开启 IF 后执行共享预算的 pinned callbacks；零预算、新 tick、注销等待及轮转有实际 core/phase/lifetime 回归。控制状态增加非等待 gate，避免 waitable read/write 与 jack service 并发更改。模块按实际 amp/presence 能力发布耳机控件，无 mute 或无 amp 仍可报告 jack，内部 ID 正确映射冻结 public index；多个 CAD 的事件互不串扰。unsupported unsolicited 改轮询，新 flight 期间的事件及失败重试均保留。Auto-Mute 仍未发布，等待真正 live group/stream 事务。

最新主机聚合 task-12-jack-integrated-final-host-1.log 包含全部 10 组 HDA、10 组 Linux audio、descriptor/OFD/SCM_RIGHTS 与定向空白检查，全部 exit 0。有效语义 RED、缺 API 编译 RED 与中途编译设置失败分开保留；task-12-jack-module-green-1.log 的失败不用于验收。普通根全镜像 build-16 exit 0，六模块/stage/manifest 及 hda.drv 无未解析符号通过。

hc10 使用本次 VMDK，经标准 OpenRC/标准 libc，ICH9/q35/4 CPU/MSI off/双 codec/card1：48 kHz 1000/2000 Hz 双声道约 2 秒实际播放、4096 帧录音（4095 帧匹配 400/800 Hz、首帧静音、容差 2）、生命周期、控制恢复和 83 项原生 SHM 全通过。QEMU exit 0，原 VMDK SHA 不变。此次未注入 jack/fatal 故障，不声明真机插拔或自动静音已验证。

审查为 task-12-inline-jack-checkpoint-review.md；精确 source/product/log/run/result SHA 保存于 task-12-runtime-checkpoint-5.json，前四次检查点和所有源码快照保留。下一断点：Auto-Mute 生产 group/stream 事务；随后 legacy 波形质量/ES1371 实际兼容后端、H6 最终审查、U1–U6。


## 36. H6 生产 Auto-Mute 与 prepare 音量持久性检查点（2026-10-03）

A3–A5 及 H1–H5/A1–A7 本地门槛继续闭合，整体保持 12/19。按用户要求全部内联，无子代理、提交、推送或归档。

Auto-Mute 仅为能力相容的独立单成员 stereo speaker/headphone group 发布。RUNNING 切换先停止 SD RUN，再事务切换完整 group、真实左右 gain/mute 和 converter binding，保留 DMA/BDL/generation/tag/format；共享 DAC 通过旧 pin 路径关闭隔离。PREPARED/PAUSED 状态保留；独立 headphone lease 与 gate 竞争返回 EBUSY/保留任务。目标激活、部分 restore、绑定或右声道 gain 失败均回滚；无法证明恢复时停止并隔离同 BDF，保留资源供清理重试。真实 controls/codec/module/stream 的语义 RED/GREEN 覆盖以上路径。

新增普通 codec 无 Auto-Mute 的 prepare 音量回归先在 task-12-prepare-volume-red-1.log 失败，最小修复后 task-12-prepare-volume-green-1.log 的 module/controls/codec/stream 全通过。安全路由激活后按真实 group 的已发布 amplifier 恢复用户音量与显式 mute。

task-12-auto-mute-final-host-1.log 的 10 组 HDA、10 组 Linux audio、descriptor/OFD/SCM_RIGHTS 和定向空白检查均 exit 0；其中额外 cloexec runner 的默认 SDK 路径错误 exit 1 保留，聚合日志不算全部成功。实际 SDK 的 task-12-auto-cloexec-sdk-green-1.log 重新通过 descriptor fixture 与 host Linux 38 checks，未作 cloexec guest 验收。四个严格 freestanding TU 全通过。普通根 build-17 的 kernel/drivers/VMDK/Live ISO/Installer ISO exit 0，六模块 staging/manifest 和 hda.drv 无未解析符号通过。

hc11 用最新 VMDK、标准 OpenRC/libc、ICH9/q35/4 CPU/MSI off/双 codec/card1，通过 48 kHz stereo 1000/2000 Hz 1.999979 秒播放、4096 帧 capture（4095 帧匹配 400/800 Hz、首帧静音、容差 2）、lifetime、controls restore 和 83 项原生 SHM。QEMU exit 0，原 VMDK SHA 不变。Auto-Mute/jack/fatal 没有 VM 注入，不声明物理插拔验收。

审查 task-12-inline-auto-mute-checkpoint-review.md 与 source/product/log/runtime SHA 记录在 task-12-runtime-checkpoint-6.json。全部此前证据保留；task-12-sparse-evidence-only-1.json 记录旧 QA raw/ext4 的 SHA 不变稀疏化。下一断点为 AC97 连续波形质量、ES1371 实际兼容后端、H6 最终审查，然后 U1–U6；H6 尚未整体接受。


## 37. U3 设置页与持久化接受（2026-10-03）

当前为 **16/19 项，约 84%**；已闭合 H1–H6、A1–A7、U1–U3。设置应用使用独立声音 worker 和受 mutex 保护的模型快照，安装器隐藏声音页且不链接 ALSA/helper。OpenRC 音量状态恢复只读取已有文件，停止时写入临时文件并在成功后原子替换，失败保留旧状态。

U3 证据：`task-15-build-1.log`、`task-15-packages-green-1.log`、`task-15-settings-run-1.log`、`task-15-inline-review-1.md`、`task-15-closeout-audit-1.json`。剩余 U4–U6：Doom 音效、Doom MIDI/OPL3 音乐、客体/真实 ALSA 全链路与最终审计。


## 2026-10-03 U4/U5 closeout

U4 已在当前 worktree 内联完成并接受：`audio_output` 的有界 pending byte queue 覆盖短写、EAGAIN、EINTR、EPIPE/ESTRPIPE 恢复和容量上限；`audio_mixer` 使用 32.32 phase、线性插值、声道分离、int64 累加和饱和裁剪。ASan/UBSan host suite 与 `make O=out/audio-hda -j8 app-doom` 均 exit 0。

U5 已在当前 worktree 内联完成并接受：MIDI/MUS 解析和 frame scheduler、running status/tempo/140 Hz、GENMIDI signature/size/index 检查、运行时 OPL 寄存器桥接、pause/loop/stop 生命周期和分块一致性均有测试；目标 app-doom 链接固定 Nuked OPL3，构建 exit 0。证据为 `task-16-inline-review-1.md`、`task-16-closeout-audit-1.json`、`task-17-inline-review-1.md`、`task-17-closeout-audit-1.json` 及对应日志。

当前为 18/19（约 95%）。仅剩 U6：必须对新镜像执行上游 ALSA 多客户端/录音、soundctl/设置页、Doom WAD 和长时负载，并记录 QEMU 矩阵；物理声卡若无设备仍须明确标为未运行。

## 2026-10-03 U6 preflight continuation

U6 的新入口 `tools/test_audio_e2e_qemu.py` 已在最新 `image-vmdk` 上运行并
保留完整结果。q35 Intel HDA duplex/4CPU/MSI-on 与 q35
ich9-intel-hda duplex/4CPU/MSI-on 的启动、codec、ALSA 播放、1/2 kHz 波形、
已知输入录音、controls/lifetime 和 SysV SHM 预检通过；pc/intel-hda/
output/1CPU/MSI-off 的严格波形窗口失败证据也已保留。统一入口在
`--duration 600` 下明确返回未闭合，因为可见 GUI、Freedoom WAD 长时压力和
physical codec 仍未运行。U6 不接受，整体仍 18/19（约 95%）。详见
`.superpowers/sdd/2026-09-30-intel-hda-audio/task-18-inline-review-1.md`、
`task-18-closeout-audit-1.json` 与 `docs/superpowers/audio-validation/2026-09-30.md`。

## 40. A3–A5 final rerun confirmation (2026-10-03)

按用户要求重新执行 A3–A5 最终回归集合，未改动既有源码。PCM/device/params/ioctl/
mmap/VMA/control/OSS/mixer、HDA controls、descriptor/OFD、permissions、memory、threads
以及定向 `git diff --check` 全部 exit 0。日志为
`task-10-a3-a5-final-rerun-20261003.log`，SHA256 为
`967dcdd57964609cf4e15919fdcbaa7bf74a705cc45a372b986436af76e6549f`，结构化记录为
`task-10-a3-a5-final-rerun-20261003.json`。A3–A5 没有剩余本地合同任务；客体/物理设备仍归 U6，不能由这次 host 回归代替。

## 41. U6 inline continuation and GUI/Xorg boundary (2026-10-03)

按用户最新决定，可见 GUI 交互不作为当前 U6 门禁；该部分转入后续 Xorg 工作，
不以当前 ReliefOS framebuffer/Desktop 路径伪造完成。当前 worktree 仍只由本对话
内联操作，未使用子代理、未提交、未推送、未归档。

U6 独立镜像已重新构建，当前产物 `out/audio-hda-u6/images/reliefos.vmdk` 的
SHA256 为 `768f6c7077ecb2f37bed7d4fb6fac54313c98e75ee5e730968a0aea0d8b801c5`。
基础 `out/audio-hda/images/reliefos.vmdk` SHA256 仍为
`611818cbec99cb1a1a998e6c68f0d49e77ddfbb223fb3bfbe9c7b633fba1b6c6`。
U6 rootfs 实际包含 `doom.elf`、`freedoom1.wad` 和 `FREEDOOM-COPYING.txt`。

最新完整 QEMU 核心证据为 `out/audio-hda-u6/qa/pf/result.json`、`serial.log` 与
`tone.wav`：q35/intel-hda/hda-duplex/4CPU/MSI-on 启动并注册 HDA codec；标准
libc probe 的 abi/play/capture/lifetime/controls 全部通过；播放波形为 48 kHz
S16 stereo、左右 1000/2000 Hz、active 1.999979 s、最大周期误差 1；录音为
4096 帧/16384 字节，4095 帧匹配 400/800 Hz（首帧静音、样本容差 2）；native
SysV SHM 83 项通过；原生产镜像 SHA 未改变。

`task-18-u6-matrix-20261003.log` 及 `qa/m1..m9` 保存 boot-only 矩阵：q35/pc、
Intel HDA/ICH9、1/4 CPU、MSI on/off、双 codec、output-only、无音频设备均有
启动/模块状态记录；ES1371 组合保留硬件身份不匹配红证据，未误报通过。

`system/rootfs/etc/asound.conf` 保留计划规定的 dmix/dsnoop 默认别名；它们需要的
ALSA timer 节点尚未实现，因此单客户端 default 及同时多客户端 default 播放/录音
仍保留真实失败证据，未以禁用共享插件的方式伪造通过。
`u6-standard-probe-20261003.sh` 的外部工具记录显示 `speaker-test` 微秒几何和
`soundctl test` 的通用 latency 协商仍返回 `EINVAL`；这些日志保留为兼容性缺口，
核心标准 libc probe 的波形/录音证据不作替代或篡改。

当前正式账本仍为 **18/19（约 95%）**：A3–A5 本地源码/host/build 门槛已闭合；
U6 的 HDA guest 核心 I/O、控制、Doom payload/staging、host Doom 音效/音乐回归
和 QEMU 矩阵已有证据。未完成项为 dmix/dsnoop timer 与真实多客户端语义、
`speaker-test`/`soundctl test` 通用协商、Freedoom 菜单/关卡/连续开火及 600 秒
压力、Xorg 可见 GUI、physical codec。没有物理声卡时保持未运行记录，不将 QEMU
模拟器结果外推为真机验收。

## 42. U6 ALSA/Doom software gate closeout (2026-10-03)

本节覆盖第 41 节之后的实际修复和复跑。工作仍限定在
`/home/xiaobai/Projects/Projects/ReliefOS-intel-hda-audio-plan`，全部内联执行，
未使用子代理、未提交、未推送、未归档；旧失败日志和未提交源码均保留。

ALSA 默认配置继续使用 `dmix`/`dsnoop`。由于 ReliefOS 核心每个物理端点只允许一
个硬件 lease，ALSAlib 的 direct plugin 对 `SNDRV_PCM_VERSION 2.0.18` 默认不会启用
共享 server；新增 `patches/alsa-lib/0001-reliefos-dmix-server.patch`，并由
`mk/upstream.mk`/`tools/build/upstream.sh` 作为可追踪上游 patch 应用。标准 probe
的默认播放、默认录音、两路并发播放/录音和 `soundctl test` 均已通过；
`speaker-test` 的微秒几何仍只记录 warning，不把它伪报为 native geometry 通过。

Doom `/dev/dsp` 先以阻塞 OSS 描述符提交完整 tick PCM；初始化期间发生的首个
`-EPIPE` 由 `SNDCTL_DSP_RESET` recovery callback 重新准备 stream 后重试。真实
Freedoom WAD、MIDI 注册、非零混音块和 headless 5 秒关卡运行均通过，最终串口不含
`PCM submission failed` 或 `no PCM audio device`。

最新生产构建命令
`TMPDIR=out/audio-hda-u6/tmp-build make O=out/audio-hda-u6 -j8 app-doom image-vmdk`
exit 0。当前镜像 `out/audio-hda-u6/images/reliefos.vmdk` SHA256 为
`58b536c9edcc2ef9adc8c8ddb9d451ea531af7cc5a38e6e4673c73cc19ebb8af`，Doom ELF
`out/audio-hda-u6/userland/doom.elf` SHA256 为
`8d837f0bc9bede370385c92c793078507ee0c911ba0f7ef0196ccbf11cb6a70d`。

最终 QEMU 运行 `out/audio-hda-u6/qa/std17-final` 使用标准 OpenRC、Intel HDA
duplex、4 CPU、ALSA file backend，`tools/test_hda_qemu.py` exit 0：

- guest core ABI/playback/timer/lifetime/controls/capture/SysV SHM 全部 PASS；
- 波形 48 kHz S16 stereo，1000/2000 Hz，active 1.999979 s，连续性最大周期误差 1；
- capture 4096 帧/16384 字节，4095 帧匹配 400/800 Hz，首帧静音、容差 2；
- standard probe 的 abi、headless Doom、hw/default playback、default capture、
  lifetime、dmix/dsnoop concurrent、amixer/soundctl controls 全部 PASS；
- `std17-final/serial.log` SHA256 为
  `24360f18c8513823ce4592e33eebfe6b611cda1313dd943a87a15ab5bb6c91a5`，
  `result.json` SHA256 为
  `d4340c75c6038fdab23f0820294d0bdd41a37b3646bd9bd975b1bf2e3bd3007c`。

主机 U4/U5/控制回归重新在工作区 `TMPDIR` 下运行，`controls settings doom-output
doom-mixer doom-music` 全部 exit 0；直接使用系统 `/tmp` 的一次失败仅为磁盘满，
没有进入编译验证，不覆盖该失败证据。

因此 U6 的可执行软件客体门（HDA/ALSA/控制/采集/Doom headless）已闭合；正式账本
仍保留为 **18/19（约 95%）**，因为用户已明确把可见 GUI 转入后续 Xorg，且当前环境
没有 physical codec，600 秒压力和物理门也未运行。完整证据索引为
`docs/superpowers/progress/2026-10-03-u6-closeout-audit.json`。

## 43. 2026-10-04 post-restart U6 continuation

重启后继续在数据盘 worktree 内联工作；`out/audio-hda-u6/tmp-backup-20261003`
约 9.6G，按备份目录中的 `SHA256SUMS` 逐项校验全部通过。未提交、推送或归档。

修正了 QEMU ALSA file backend 的录音素材长度：输入时长为
`max(60, Doom 请求时长 + 60)`，并写入每次 `run.json`。600 秒 Doom 使用 660
秒输入（126,720,000 字节）。同时修正 `tools/test_audio_e2e_qemu.py`，仅在 Doom
模式区间统计 Doom XRUN，并另记整次串口 XRUN。合成边界和真实日志分类均通过。

镜像 `out/audio-hda-u6/images/reliefos.vmdk` SHA256 为
`fe47bff3c733f1283ae8f6334dfecb3c1e26c3d0205704761e1e9620701966d0`。短跑
`qa/lts28` 在同一镜像上通过 core audio-qa、48kHz 左右声道波形、已知输入采集及
标准 probe 七个模式；该次使用 8192 帧 buffer 的独立 probe，仍在核心 PCM 64KiB
缓冲上限内。证据见其 `result.json`、`serial.log` 和 `tone.wav`。

600 秒 run `qa/e2e660d` 中，Freedoom WAD headless 音频通过，CPU/IO worker 活跃
60 秒，标准服务 `DONE failures=0`，Doom 模式区间未发现 XRUN。两条 0ms overrun
记录都在 Doom 结束后的独立 lifetime capture 模式。该次默认 2048 帧 core capture
预检仍失败：`READI_FRAMES EPIPE`，提取录音为 4096 而非 16384 字节；因此 Doom
长跑可单独记为通过，但 U6 仍保持未接受，直到默认核心采集稳定复现通过。报告器原先
把所有串口 XRUN 错算为 Doom XRUN，修复后的分类仅用于 Doom 区间，不豁免其他失败。

宿主实际有 AMD HDA 和 Realtek ALC897：只读枚举显示 PCI `1002:ab28`、`1022:1487`
及 `HD-Audio Generic / ALC897 Analog`。这不是 ReliefOS 驱动的真机证据；尚未将镜像
原生启动或独占直通。因此物理 ReliefOS codec 门仍未运行。可见 GUI 依用户决定延至
Xorg；正式进度仍为 18/19（约 95%）。完整新证据索引为
`docs/superpowers/progress/2026-10-04-u6-resume-audit.json`。


## 44. 2026-10-04 U6 lifecycle 默认软件验证

默认捕获生命周期的软件范围已验证通过，并接受 dmix/dsnoop 8192/65536。host 定向 strict、9组合QEMU矩阵、重复 null/file 生命周期、known waveform/stereo、600秒Doom与60秒压力均通过。U6 的原始小 buffer/无尾静音 EOF 问题未闭合，物理 gate 未执行，不能宣称整体完成。

最小修复：QA null sink正确处理短写；PCM XRUN先STOP、丢弃旧run通知后PREPARE；HDA真实WALLCLK与粗ticks分离；CHANNEL_INFO输入channel usercopy；HWSYNC真实pointer查询与累计帧高水位。所有有效RED/GREEN保留，HWSYNC初版半圈比较的OSS boundary回归也有实际失败与最终strict/OSS通过。

生产 dmix/dsnoop 均为 `period_size=8192`、`buffer_size=65536`：32KiB周期、256KiB buffer、8个HDA period。周期约170.7ms、buffer约1.365s；稳定性与延迟代价明确记录，不宣称最低延迟或实时保证。普通 arecord 不指定参数时，client 协商8192/24576，底层dsnoop为8192/65536。

最终长测 `out/ds87-r5-long600` 的 wrapper 与两个独立数据/计数验证退出0：600秒Doom写入164,659,280字节，startup/XRUN/suspend恢复均0；60秒CPU/I/O完整重叠，I/O完成7,839,744字节/29轮。整个serial XRUN-clean，core/standard均failures=0；最后60秒stereo为2883584帧/11534336字节，全部已知样本连续匹配。GUI和physical没有参与软件退出码，分别记录deferred与not_run。

9组合详见r5-matrix-results.json（ds78–ds86）：7个双工完整suite/连续录音，output-only适用core/wave/controls，no-audio仅boot/module状态。新波形probe为512/8192加8192帧显式静音尾部，完整tone checker不放宽；canonical512/2048源码未改。ds69原小buffer EPIPE和ds73无尾静音EOF仍未闭合，不以新probe伪装生产DAC drain修复。

正式账本仍18/19，GUI按用户要求延期Xorg；物理codec ReliefOS原生启动/独占PCI透传未执行。当前handoff已更新，完整source/image/evidence SHA见2026-10-04-u6-lifecycle-audit.json，内联审查见2026-10-04-u6-lifecycle-repair-review.md。未启动子代理、提交、推送、归档、reset或清理dirty/untracked证据。

## 45. U6 closeout continuation：新反例与当前R8（2026-10-04）

当前仍18/19，U6未闭合，GUI deferred to Xorg，physical not_run。完整当前事实、SHA与继续断点见 `2026-10-04-intel-hda-audio-current-handoff.md`；前轮handoff/status全文保存于 `out/audio-hda-u6/qa/u6-closeout-20261004/`，旧审计不改。

新增PCM DRAIN全部未提交ring清零（不推进appl_ptr）与canonical非阻塞DRAIN STATUS/SETUP完成识别，真实RED/GREEN保留，原始512/2048、threshold512、无尾静音仍测试。R6标准六组合通过、双codec ds100 long-file EPIPE与数据断点，output/none未运行。R7 ds103双codec10段capture/全部60秒已知stereo通过不能推翻ds100；50秒标题→demo包含音乐转换/44次攻击/8并发SFX，新的600秒场景负载仍待验收。

R7 controls完整wave发现amp读回正确但mute/half无效，另有原始native第4流EPIPE且queued仍1515帧、用户调用间隔约24µs。HDA prepare现将mixer恢复放在formatbind后、START前，新增独立effective gain硬件模型RED/GREEN与失败STOP/重试验证；HDA position仅加错误诊断，没有放宽full-ring判定。

当前R8 VMDK SHA01c20f005100685671f8838a9a30f634881b04331af2bf1cd50d11dd3118bce6，HDA1d63652929176de9655d51058d8e204fd64fed5f6193eeae4da2d2dad0ba4e25；实际镜像ESP/root组件逐项SHA匹配构建产物。module/stream/controls/codec sanitizer与stream strict通过；整份module strict继承警告失败保留。R8权限专用guest通过5节点owner/group/supplementary/other/root、EACCES/EPERM及原metadata恢复；其waveform不适用，不替代音频gate。SDK/packages通过，make test与位置诊断继续中。尚未接受新的完整矩阵/控制波形/无尾静音有限流/600秒长测。

## 46. R9 STATUS 修复、完整构建与当前严格反例（2026-10-04）

R8 ds108 native 16流与timer API全部成功，但完整17流g0少349帧、g8少16帧；runner/完整waveform均失败。ds109五个controls API成功，实际mute、半幅5976/5976、balance12000/5976及全幅恢复正确，但严格checker因静音95875/96000帧失败。ds110 g24真实欠载hw73823/appl73774（硬件超过49帧），没有hda-pos拒绝日志；timer DRAIN EPIPE。这些压力诊断与host make test并发，不能作为接受结果。

STATUS错误将XRUN delay报告为boundary−49巨大的正数、avail截为0。真实ioctl fixture RED134；R9保留超过一个ring的avail，只有RUNNING/播放DRAINING报告delay，播放欠量为有符号，非运行delay0。DELAY在XRUN仍EPIPE。回绕、continuous dmix和capture overrun严格ASan/UBSan GREEN0，受影响PCM/device/params/ioctl/mmap strict与HDA相关sanitizer、stream strict均0。position失败诊断的last/WALLCLK改为锁内快照，检测规则不变。

make test退出2：test-pages继承branding-compatibility六个文档链接缺失；保留完整红日志，未改无关文档或跳过检查。R9完整root Make最终退出0，VMDK/liveISO/installer/SDK都实际生成；早先错误目标名、缺依赖cache的失败日志保留，依赖通过工作树内只读cache并集解决。

R8 ds112无声卡实际open测试：/dev/dsp ENODEV，control/playback/capture ENOENT；Doom 20秒静默进入引擎并正常退出、SHM83。最初ds111以dsp节点不存在为前提的QA失败保留。R9 image SHA da90ca85baeb19a8c8e1f35aca9491e7827d016431f08a0d1db607a14321f38a；kernel cbfdbc8d23c7013fc2ee3ae4efabe30bb7036d31301f32087de93b8f9945e0f7；HDA e7b7de0d3185a5b7a8c4daf429bb3f72090d53c9a7796a1019cfb4f165e5887d。完整manifest C/r9-source-products.json。ds124新版标题→demo/600秒/60秒CPU+I/O运行中；最终完整矩阵、原始无尾静音EOF仍待完成，不关闭U6。GUI延后Xorg；physical not_run。

## 47. R10 DMA quiescence、实际镜像与当前矩阵（2026-10-04）

R9 ds124新版标题→demo/600秒与60秒压力完成，但Doom恢复XRUN两次；音乐4、SFX3869、attack1230、并发voices8。所有10capture/全部2883584 known samples通过，native前16流匹配而timer截386帧。原report漏掉正xrun_recoveries的whole-serial true保留原件，reporter真实RED1/GREEN0后重新解释false，不撤销失败。

R10最小驱动修复：位置拒绝IRQ清RUN/readback成功后才通知XRUN，STOP卡住仍保留RUNNING/backing供下一IRQ重试，tiny-ring/stuck-stop真实RED134/GREEN0。新增Doom真实fd EPIPE epoch提交量/hw/延迟/调用cycles诊断，样本/恢复行为不变。完整正常root Make退出0，raw/VMDK compare0，实际ESP两kernel路径及root HDA/asound/Doom五SHA全匹配。VMDK SHA6f446b63b460e3fbcf9dca5681cea4c713aa27ae89b003bcf98c5ae9bc5ad780；manifest C/r10-source-products.json。

C/r10-standard-matrix.py运行中，ds125/126/127标准及完整raw-null/raw-file/10capture/60秒已知输入通过，完整17原始native流仍截尾，严格结果false。WAV120诊断ds134 scene2/429/71/8、60秒负载、零恢复，capture明确未运行；完整native g0截547、timer截819，严格失败。首次WAV ds133 unsupportedADC capture阻塞/Doom未运行的自有QMP停止证据保留。不同120/600秒不能用于后端因果结论。U6仍18/19；8192/65536未接受；GUI延后Xorg、physical not_run。
