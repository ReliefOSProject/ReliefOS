# Intel HDA / Linux 音频 ABI / Doom 音频实施进度与交接

保存日期：2026-10-01，用户时区 Asia/Shanghai。

本文因用户要求“把目前进度非常详细地保存，然后给出提示词让另一个 Agent 接替”而创建。它是接替控制者的完整入口，描述交接时的事实和继续执行方法。用户已经批准实施，接替后应继续原项目；这次交接不代表项目完成。

## 1. 交接断点：先读这一节

**H1、H2 已完成各自源码、host 测试、适当编译和独立审查关卡。H3 Repair 2/3 已冻结，Repair 2 的独立 Sol/high 复审与 Repair 3 的定向 I1 复核均已完成，H3 已于 2026-10-01 验收。H4 尚未实施。**

H3 的 I2、I3、I4、I5、I8、M1 原始具体缺陷已被复审判为 ADDRESSED。其中 I8 的底层电源正确性仍依赖 I6，不能把它理解为整体电源生命周期已通过。

H3 Repair 2 关闭了 I1 的替代匹配/整路径容量、I6 的 SET 后 fresh readback、I7 的真实 pin gate；Repair 3 又在 discovery 阶段拒绝不可编码的 input-amp index。修复包、完整日志、报告和定向复核均已保存。没有执行 H4–H6、A1–A7、U1–U6 实现。

接替后第一件实际工作：

1. 确认本文、根 AGENT.md、相关技能和当前 worktree 状态。
2. 读取完整 [H3 Repair 1 复审报告](../../../.superpowers/sdd/2026-09-30-intel-hda-audio/task-3-fix-1-re-review.md)。
3. 读取已准备但尚未派发的 [H3 Repair 2 简报](../../../.superpowers/sdd/2026-09-30-intel-hda-audio/task-3-fix-2-brief.md)，不覆盖既有 task-3-fix-2-before.json。
4. H3 已完成上述 Repair 2/3；读取 `task-3-fix-2-*`、`task-3-fix-3-*` 的完整证据和复核。
5. 刷新 H4 的真实接口和正式合同，保存 `task-4-before.json`，再按当前用户授权使用 GPT-6.1-Sol / xhigh 实施。
6. H4 完成后由 GPT-6.1-Sol / high 独立只读审查；后续仍按既定 H1–H5 → A1–A5 → A7 → A6 → H6 → U1–U6 顺序推进。

**没有真实 HDA .drv 链接、加载、ReliefOS 来宾播放、录音、Doom 音频或真机验证。现有 host、TU 与普通 kernel target 的成功日志不能替代这些门槛。**

## 2. 用户目标、授权及固定偏好

原始需求：给 ReliefOS 增加 Intel HD Audio，尽可能完整支持声卡；doomgeneric 声音系统良好运行；音频接口完全遵循相应 Linux ABI 和 POSIX 行为；系统程序可以调音量、设置扬声器、耳机和录音源等。

已确认验收环境：QEMU Intel HDA，并以常见真机的扬声器、耳机和录音为目标。

用户已经说过“批准，开始实现”“继续”。不要重新询问是否实施，也不要把实现请求退回规划。用户最新模型要求：

- 所有实现及修复 Agent：**gpt-6-luna，reasoning_effort=xhigh**；因该服务当前返回 404，用户已明确授权本次实现改用 **gpt-6.1-sol，reasoning_effort=xhigh**。
- Review Agent：**禁止 GPT-6-Astra**。当前采用 gpt-6.1-sol；H3/full/directed complex review 使用 high，小修复可 medium。
- 原控制者只协调、核验、做文档/ignored QA/快照，不亲自修改生产 C、汇编、Make 或 Shell；H4 实现使用用户授权的 Sol/xhigh。
- 同一任务只有一个写源码的实现者，任务实现与后继任务串行；独立审查者不改源码。
- 不派遣实现者/审查者的下级 Agent。

已批准的范围为通用模拟 HDA 播放/录音、标准 PCM/control/OSS、程序控制、共享播放/录音、Doom 音效及 OPL 音乐。HDMI/DP ELD/GPU 联动、SOF/SoundWire 为独立扩展；S/PDIF/多声道必须按实际能力公布，不能从模拟 fixture 推定支持。真机未测时如实保留未验收记录。

“遵循 Linux ABI/POSIX”对应音频设备、native x86_64 LP64 ioctl、syscall、fd、mmap、poll、错误及上游程序行为；不据此宣称整个操作系统已经获得所有 Linux/POSIX 接口的完整认证。

## 3. 唯一实施目录与 Git 状态

| 项目 | 精确位置/状态 |
| --- | --- |
| 唯一写入与构建 worktree | /home/xiaobai/.codex/worktrees/intel-hda-audio-plan/ReliefOS |
| 原始主仓，仅只读参考 | /home/xiaobai/Projects/Projects/ReliefOS |
| 内核子仓 | 实施 worktree 内 kernel/reliefnt |
| 主仓 HEAD / 初始基线 | bafaa59081f8c998037524fd5aea8865dd0468bd |
| 子仓 HEAD / 初始基线 | f8f5a4908807e81cca7ed766fb6c60e7829767dc |
| 分支状态 | 主仓与子仓 detached HEAD，保留现状 |
| 构建输出 | out/audio-hda，实际位于实施 worktree 内 |
| 参考下载缓存 | /home/xiaobai/Projects/Projects/ReliefOS/cache/downloads，只读消费 |
| 审计、日志、快照目录 | .superpowers/sdd/2026-09-30-intel-hda-audio，位于实施 worktree |

主仓显示 kernel/reliefnt 为 dirty 子仓；计划、测试、新内核文件多数仍为 untracked。这是已实施成果，必须保留。主仓 git diff 本身不会包含 untracked 文件正文，也不会显示子仓全部修改。

**没有 stage、commit、创建 branch、push 或 PR 的用户授权。** 设计批准和实施批准不等于 Git 操作授权。根 AGENT.md 明确要求授权，计划里“授权后 commit”的步骤是条件句。不要创建用于审查的虚假空提交，不更新 gitlink，不复制覆盖原仓，不 archive worktree。

本次交接再次执行只读 git status：原始主仓及其子仓输出为空，均保持 clean。实施 worktree HEAD 与以上基线一致；改动尚未提交。

审计目录 ignored，里面的报告/日志/blob 不会随普通 Git 提交自动带走。接替 Agent 必须访问同一文件系统与完整 worktree；迁移机器时应保留此目录、blobstore 和输出证据。不能只拿一份 Git diff 丢掉新文件或审计材料。

## 4. 权威文件与阅读顺序

| 内容 | 文件 |
| --- | --- |
| 完整设计 | [2026-09-30-intel-hda-audio-design.md](../specs/2026-09-30-intel-hda-audio-design.md) |
| 项目计划索引 | [2026-09-30-intel-hda-audio.md](../plans/2026-09-30-intel-hda-audio.md) |
| H1–H6 硬件项目 | [2026-09-30-intel-hda-hardware.md](../plans/2026-09-30-intel-hda-hardware.md) |
| A1–A7 ABI 项目 | [2026-09-30-linux-audio-abi.md](../plans/2026-09-30-linux-audio-abi.md) |
| U1–U6 用户态与 Doom | [2026-09-30-audio-userland-doom.md](../plans/2026-09-30-audio-userland-doom.md) |
| 仓库约束 | [AGENT.md](../../../AGENT.md)，文件名为单数 |
| 持续审计账本 | [progress.md](../../../.superpowers/sdd/2026-09-30-intel-hda-audio/progress.md) |
| 编号执行副本 | [execution-plan.md](../../../.superpowers/sdd/2026-09-30-intel-hda-audio/execution-plan.md) |
| 编号映射 | [task-map.txt](../../../.superpowers/sdd/2026-09-30-intel-hda-audio/task-map.txt) |
| H3 原任务 | task-3-brief.md、task-3-context.md、task-3-implementation-instructions.md |
| H3 修复前全审 | task-3-review.md，原 I1–I8 Important + M1 Minor |
| H3 修复绑定裁决 | task-3-fix-1-brief.md |
| H3 当前权威复审 | task-3-fix-1-re-review.md、task-3-fix-2-re-review.md、task-3-fix-3-re-review.md |
| H3 完整实现报告 | task-3-report.md，Repair 1 附录覆盖早期状态 |
| H3 修复绑定简报 | task-3-fix-2-brief.md、task-3-fix-3-brief.md，均已执行并冻结 |
| 交接提示词文件 | [2026-10-01-intel-hda-audio-successor-prompt.md](2026-10-01-intel-hda-audio-successor-prompt.md) |

表中未带链接的 task 文件均在上述审计目录。不要一次让接替 Agent 重读所有旧日志。先取得设计/计划全貌，再读当前任务、上下文、绑定裁决和权威复审；旧关卡出现相关新变化时再定向查证。

设计和三个实施计划已经批准。任务编号执行副本只是适配 task brief 提取工具，不是另一个规格版本。19/A7 后加入，但执行顺序提前到 A6 前。

## 5. 技能和执行流程

用户显式要求 using-superpowers、writing-plans，已经使用其流程完成设计、计划、批准和 worktree。之前已使用 brainstorming、using-git-worktrees、subagent-driven-development、test-driven-development、systematic-debugging、requesting-code-review、receiving-code-review、verification-before-completion 等。

接替者按当前真实技能文件读取；根路径 /home/xiaobai/.agents/skills。关键文件为 using-superpowers/SKILL.md 及 references/codex-tools.md、subagent-driven-development/SKILL.md 和 implementer/task-reviewer/re-review 模板。SDD 明确要求委派，也有用户明确的实现/审查模型要求。工具和用户最新模型限制高于旧技能参考的路由建议。

流程要求：

1. 控制者读完整计划，将精确任务正文与上下文交给干净的实现 Agent。
2. 保存 task-N-before.json；实现者先测试，再最小实现，逐轮保存真实 RED/GREEN。
3. 实现者自审、验证、报告、冻结；控制者读取实际日志与完整报告。
4. 从前后工作树快照产生包含 untracked 的完整固定 diff 和 after SHA。
5. 新独立 reviewer 做规格与质量审查；不能只相信实现者状态。
6. 前三轮修复优先用 followup_task 唤回原实现者，修完重新冻结并做定向复审。不能每个 finding 新建一个实现者。
7. 本任务关卡通过才更新计划复选框、索引、执行副本及账本，再给下一个任务。
8. 项目最后需要完整横向审查和真实验收，不把单任务 GREEN 当最终交付。

新会话可能无法访问旧 collaboration Agent。先看当前工具是否能列出并唤回；可访问时继续原 /root/implement_h3_luna_xhigh。不可访问时，依据已保存完整简报创建一个新的 Luna/xhigh 接替该实现任务，明确记录跨会话接替，不声称调用成功或旧 Agent 正在工作。先确认没有旧源码写入者再派发。不要用跨线程聊天工具绕过用户授权。

隔离委派显式使用 model 与 reasoning_effort，并使用 fork_turns=none。review 只读源码、索引、HEAD，只允许写 ignored report；不得建自己的子 Agent、重建 Git diff 或重复已通过 broad suite。

进度沟通用中文，持续工作中不超过约 60 秒没有有效更新。等待不要单次阻塞超过 60 秒；以前使用 45 秒有界等待。只报告新事实、风险和下一步。

## 6. 所有任务状态与实际顺序

| 执行位置 | 编号 | 任务 | 交接状态 |
| --- | --- | --- | --- |
| 1 | 1 / H1 | 版本化卡接口、PCI MMIO/DMA/MSI、核心生命周期 | 已接受，含两轮修复复审 |
| 2 | 2 / H2 | controller、CORB/RIRB、命令传输/即时模式 | 已接受，含两轮修复复审 |
| 3 | 3 / H3 | codec 图、模拟路由、association group | Repair 2/3 已完成并验收，保留无 module/VM/physical 证据边界 |
| 4 | 4 / H4 | 播放/录音 DMA、BDL、位置、period/XRUN | 合同已正式化，实施尚未开始，已准备 before 快照步骤 |
| 5 | 5 / H5 | 真 mixer、耳机插拔、原子路由/音量策略 | 未开始，上下文草案已保存 |
| 6 | 6 / A1 | canonical native Linux UAPI 与导出 | 未开始 |
| 7 | 7 / A2 | PCM 对象、状态机、参数、核心引用 | 未开始 |
| 8 | 8 / A3 | 标准 fd/read/write/poll/nonblock 与错误语义 | 未开始 |
| 9 | 9 / A4 | hw_params refinement、mmap、指针、VMA | 未开始 |
| 10 | 10 / A5 | ALSA control、TLV/events/锁、OSS | 未开始 |
| 11 | 19 / A7 | native SysV SHM，dmix/dsnoop 必要前置 | 未开始，已补入批准目标的实现依赖 |
| 12 | 11 / A6 | 同源 Linux / ReliefOS 标准 ABI probes | 未开始 |
| 13 | 12 / H6 | 真实模块、构建/stage、manager、QEMU 波形 | 未开始，集成预研已保存 |
| 14 | 13 / U1 | 上游 ALSA/OPL 依赖、可复现构建、SDK | 未开始，版本/哈希预研已保存 |
| 15 | 14 / U2 | opt-in 音频库、soundctl 控制程序 | 未开始 |
| 16 | 15 / U3 | 独立声音设置页、worker、持久化服务 | 未开始 |
| 17 | 16 / U4 | Doom 非阻塞输出/短写/时钟/重采样 | 未开始，实际端口缺陷已查证 |
| 18 | 17 / U5 | MUS/MIDI/GENMIDI、Nuked OPL3 | 未开始 |
| 19 | 18 / U6 | 上游多应用、GUI、长时 Doom、完整验收 | 未开始 |

只接受了 19 个任务中的两个关卡；任务工作量不均，不据此计算整体完成百分比。剩余任务仍需持续实施。

## 7. H1：已经完成的实际内容和约束

主要实际代码：

- 内核子仓 arch/x86_64/pci_irq.c 及 PCI/paging/MM/IRQ/IDT/boot 配套。
- 内核子仓 include/reliefos/driver_audio.h，为模块私有音频合同；公开 driver.h 只包含 driver_abi，不泄漏音频私有类型。
- 内核核心 kernel/reliefnt/audio/core.c，核心头 kernel/reliefnt/include/reliefnt/audio.h。上述路径相对子仓；主仓完整路径有双层 kernel/reliefnt。
- driver_resources.c、driver_manager_phase.c、manager/lock/time 配套。
- 主仓 tools/test_hda.py、audio_fixture.h、audio_lifetime_test.c、pci_audio_resources_test.c、driver_execution_phase_test.c。

旧 v1 前缀实际布局保持：

| 字段/结构 | LP64 实际值 |
| --- | --- |
| legacy audio_ops | 32 bytes |
| kernel API register_audio offset | 160 |
| 原 kernel API prefix size | 168 |
| 新 enumerate / map / unmap | 168 / 176 / 184 |
| alloc_dma / free_dma | 192 / 200 |
| request_irq / free_irq | 208 / 216 |
| register_card / unregister_card | 224 / 232 |
| period_elapsed / control_changed | 240 / 248 |
| pci_write32 / audio_set_service | 256 / 264 |
| 追加后 kernel API 总尺寸 | 272 |
| 当前 audio_params / audio_caps | 24 / 40 bytes |
| 当前 audio_card_ops v1 | 88 bytes，control_write offset 80 |

后续不得改变旧 prefix、旧 audio_ops 或旧 params/caps 对象布局后仍盲读旧对象。

核心 card 上限 16、stream 上限 64；资源账本最多 256 项。card/stream/IRQ handle 使用 24-bit generation + 8-bit slot，耗尽拒绝而不回绕。注册复制 identity/ops，callback pin owner；核心得到 hardware stream lease，实际 PCM/VMA 页的所有权由后续 A 任务承接。

资源 API 依赖 loading_slot：alloc_dma、map_mmio、request_pci_irq 属于模块 init 阶段。H4 后续不能在普通 PCM prepare 中调用这些 init-only 分配器；应初始化时预分配持久 BDL/position DMA，PCM DMA 由核心分配并借给硬件。

MMIO：调用者拥有的 PCI BAR 页对齐 base，完整 4 KiB 范围必须在 BAR 内，32/64-bit BAR 边界及 16 GiB direct-map 范围验证；UC/PWT/PCD/NX、引用、原 PTE 恢复、SMP TLB 同步。DMA：最后一个字节符合 mask，溢出检查、原子 claim、清零，精确页 tuple 回收。

legacy alloc_pages(uint32_t) 返回 uint64 physical，0 表示失败；free_pages(uint64,uint32)。CPU metadata 页显式拥有/回收，可用已检查 direct-map alias；不能把 legacy page 当作未记账的设备 DMA。

MSI：cap walk 有界最多 48 项，vector 0x50..0x5f，64-bit 地址，BSP APIC 范围；不支持返回 ENOTSUP 让 controller polling，不伪造 INTx。释放依次 disable/readback、detach、drain 已进入 handler、回收。**pending-before-dispatch 的硬件 vector reuse 边界尚未覆盖，见 H6。**

audio service 每 10 ms tick 全局总预算 32，公平服务，callback 有 pin/unregister 同步；不得睡眠、同步 codec verbs 或等待 execution ownership。任务 callback 可等待，但处于短 transaction 外的 wait phase。

卸载：活动 stream 普通 unload EBUSY；forced disconnect 先 STOP、同步 callbacks；STOP 失败保留模块及资源。旧驱动 admission close/drain/reopen 保持。

H1 第一轮修复解决 execution ownership 阻塞等待、legacy admission 和相关注释；第二轮修复解决自动 MMIO cleanup 再消费 lease 导致 provider/PTE 引用不归零、phase fixture 标记发布竞争及注释。

权威报告链：task-1-report.md → task-1-review.md → task-1-fix-1-re-review.md → **task-1-fix-2-re-review.md**。最后复审所有 R1/R2/R3 addressed，无新 Critical/Important。

实际最后日志：task-1-fix-2-resource-red.log/green.log、phase-green.log、final-suites.log、build-final.log、diffcheck.log。resources/lifetime/phase 通过；正常增量 Make 重编真实 manager/resources 并链接、安装 kernel.sys/kernel.debug，已有驱动增量目标成功。旧 e1000、Linux descriptor、Linux ABI、UAPI/header export 基线也有保存证据。

历史 H1 曾有 make -B 引起 kconfig configure race；原顶层失败输出被覆盖，报告如实保留嵌套 build.log 和 provenance 缺口。不能重写历史或把恢复增量构建冒称完整强制重编。后续禁止 -B 与失败日志覆盖。

## 8. H2：已经完成的实际内容和约束

实际文件：子仓 drivers/hda/controller.c、hda.h；主仓 tools/tests/hda_controller_test.c；runner controller suite。

已实现 controller init/destroy、encode、submit/poll ticket、task-context exec、bounded service、unsolicited queue、stream acquire/release、recovery permission/explicit recover、representative card ID/deferred disconnect 等。

transport 模式 UNAVAILABLE/RINGS/IMMEDIATE。CORB/RIRB 2/16/256 size、正确宽度、W1C source、CAD solicited/unsolicited 分流；64-bit ticket 单飞，禁止回绕，不把 software generation 当硬件 flush。命令约 100 ms deadline，真实 stop/reset/readback 和低→高 CRST 握手，100 Hz 时钟下稳定等待约 21 ms。

只有 ring ready 才 GCTL.UNSOL；controller IRQ 源包括 CORB CMEIE/CMEI、RIRB RINTCTL/OIC 与 W1C 源。INTSTS 为 RO 汇总，WAKEEN=0；只有持有真实 IRQ 才打开 CIE/GIE。H4 handler 尚未实现，不能提前打开 stream SDIE。

即时模式只允许 ring 初始化硬件握手失败且真实 reset/quiet/stable 成功后降级。API/ENOMEM/ERANGE 等资源错误不被 fallback 吞掉；普通命令超时 poison，不自动切模式。ICVER=1 验证 CAD 和 solicited；ICVER=0 保留字段不读取，依靠单 controller producer/single-flight、ring stop、UNSOL off 和真实 reset 清旧响应。

active PCM 阻止透明全 controller reset。只有停止所有 stream/route 并释放 gate 后 task-context recover。H5 必须消费 submit/poll，以非阻塞状态机处理多 verb，不能在 IRQ/tick 执行 hda_exec_verb。

无法证明 ring DMA 停止时，init/destroy 返回负错误并保留 unsafe leases，initialized=0，service 不再运行，硬件恢复后可 retry。**void fini 与 manager 自动 cleanup 的桥接尚未实现，H6 必须解决。**

最后第二轮修复：occupied IMMEDIATE flight 在竞争 submit 时先拒绝，不能清 owner IRV，覆盖 pending 和 complete-but-unconsumed；ICVER=0/1 GREEN。Doxygen 审计修正 pointer-return 漏计，真实名单为 51 controller definitions + 15 当时 header declarations。H3 后追加 private sleep helper使这份数量成为历史，不据此硬编码现状。

权威链：task-2-report.md、task-2-review.md、task-2-fix-1-re-review.md、**task-2-fix-2-re-review.md**。所有剩余 finding 已关闭，无新 Critical/Important。

最后日志：task-2-fix-2-R1-owner-response-RED.log/GREEN.log、focused-final.log、strict-host-final.log、freestanding-final.log、root-make-final.log、doxygen-audit.log、diffcheck-final.log。controller/resources/lifetime/phase、严格 sanitizer host、freestanding TU、普通 kernel target 有真实通过日志；没有 HDA module link。

历史 I3 的 pre-implementation direct RED provenance 缺口仍在报告中；后来的 fallback mutation RED 不是实现前测试证据，不能回写成 test-first。

## 9. H3：当前实现、已关闭项与证据边界

### 9.1 已接受的实际接口

生产 codec.c 解析最多 256 NID / 256 connections、cap inheritance、完整 raw graph、range 展开、cycle/bounds、单路线、AFG power 与 signal ownership、快照/回滚/诊断。调用生产函数的 fixture 有真实 verb log，而不是复制 graph/route 算法。

当前 private group API：

- hda_find_route_group(codec,pin,group)。
- hda_apply_route_group(codec,group,enable)。
- hda_route_group_destroy(group)。

group.members 为 exact CPU-page-owned hda_route 数组；small handle 有 codec、members_phys/pages、member_count、association、total_channels、active/rollback。成员有 converter_nid、path/connection_index、channel_start/count。discovered group 即使 inactive 仍通过 route_groups pin codec metadata；active group destroy EBUSY；group destroy 必须先于 codec destroy。

route active/snapshots_valid/rollback_failed/rollback_error 和 lease 标记代表真实保留资源；失败 restore 不能释放存储。codec 拥有 route_node_owner[256] exclusive signal 标记及 AFG saved power/valid/count 数组，disjoint playback/capture 可共享 AFG，首 owner 保存原值、最后成功 restore 才释放。

逻辑 codec transaction gate 覆盖整个 apply/restore/group rollback，可 task wait，但没有 spinlock 跨等待。group preflight 在硬件写前检查所有成员，冲突零副作用。apply/restore 要求 PCM 已停止，不允许 IRQ/pointer/trigger 改路由或快照。

当前实现报告早期的 greedy solver、min(pin,DAC) capacity 限制已由 Repair 2 的替代匹配和整路径容量检查取代；Repair 3 在 discovery 阶段拒绝 input-amp connection index > 15，避免公布不可编码路径。

H3 还将 H2 已审查的 IF-preserving sleep wrapper 暴露为私有 hda_controller_sleep_ms，task-only、controller/API lifetime、无 execution ownership/spinlock、恢复入口 IF；真实 controller fixture 验证 IF0/IF1 和 IRQ 进展。没有新 kernel API。

### 9.2 第一轮原缺陷与复审结果

| Finding | 原问题 | Repair 1 最终复审 |
| --- | --- | --- |
| I1 | 无实际 association/sequence group/channel 映射 | 未闭合：有 group 但不重分配早选路径，且漏中间 widget capacity |
| I2 | AMP GET/SET 方向和右声道编码及 fake oracle 错 | ADDRESSED |
| I3 | 任一声道恢复成功清共享 flag，遗忘另一声道失败 | ADDRESSED |
| I4 | 多连接 Pin/ADC 没有真实 Connection Select | ADDRESSED |
| I5 | 在 root F00 parameter 1 读取虚构 subsystem ID | ADDRESSED，改实际 AFG F20 IID |
| I6 | PS_Error/异步 power readiness 处理错误 | 未闭合：SET 后仍按旧 Act 提前成功 |
| I7 | 未用 0dB offset、跳过 mute-incapable amp | gain 部分修正；真实 pin gating 未闭合 |
| I8 | 多 active route 共享 AFG/control 生命周期无保护 | 原 ownership 算法 ADDRESSED，整体依赖 I6 |
| M1 | cached connection count=256 显示 00 | ADDRESSED，真实 dump 显示 0x100 |

### 9.3 Repair 2 必须修复的精确反例

**I1(1)：普通可行组被 greedy 固定为无解。** codec.c:768–771 首个 BFS DAC 即终止；1095–1117 永久保留前一成员 used/blocked。同 AFG/association，唯一 sparse sequence 的 P(seq0) 直接连接 A/B，Q(seq2) 仅直接 A，A 先入队。当前 P→A 后 Q 被拒绝，-ENODEV；实际 P→B、Q→A 完全不共享、可行。需要有界替代匹配/回溯，不能以“共享 signal widget 保守拒绝”豁免。

**I1(2)：channel range 漏整条路径容量。** discovery 1102–1107、apply validation 2015–2021 只 min(pin,DAC)。stereo pin→mono selector→stereo DAC 被公布为 2 channels 且 preflight 通过。需要沿 actual path 检查中间 widget，收窄、拒绝或选择合适替代路线，range 与总 channels<=16 均真实。

复审另提醒 input amp selected index>15：single-route 可以发现但 apply 拒绝；group 不应声称此为可应用 feasible path，也不能截断。它没有另记一个 blocker，但须保持原 binding 限制。

**I6：SET 后使用旧 GET 的 Act==target 返回成功。** helper codec.c:1486 旧 GET，1489–1493 SET，1496 旧 current_act==target 立即 return0。apply 例：old SetD3/ActD0，请求 D0，codec 在 SET 后拒绝/新增 PS_Error；当前不读新状态即继续 enable。restore 例：old SetD0/ActD3，请求原 D3，错误清 last AFG lease/owners 或 power changed bit，未验证恢复。SET 后至少 fresh GET 验新 PS_Set/PS_Error，并依实际 Act 的有界等待合同；两方向错误和 retry 都要覆盖。

**I7：全部 amp 无 mute 且 prior pin 已 enabled 时没有真实 gate。** codec.c:1673–1685 mute 全 no-op，1688–1711 直接 power/selector；1718–1735 只开目标 pin，若原来已开甚至不发 pin SET。restore 1796–1838 同样没有先 gate 再改 topology。需要在 topology/gain 操作前关闭并 readback 实际 IN/OUT/HP gate；记录不确定写入，gate失败不能继续不安全变更，保留 retry metadata；最终还原原 pin/VREF/EAPD 和真实 channel amp。

原控制者已定向读实际 power early-return、group blocking 和 apply/restore 代码，技术上接受上述剩余 finding。完整权威复审原文已附在 Repair 2 简报，行号以冻结源码为准，修复后不得沿用旧行号当现状。

### 9.4 已固定的真实 HDA wire 合同

AMP GET：output bit15/input=0，LEFT bit13/RIGHT=0，bit12 reserved，input index3:0。AMP SET：output bit15/input bit14，L13/R12，input index11:8，mute7/gain6:0。

input index1 的 GET L/R 为 0x2001/0x0001，SET L/R prefix 为 0x6100/0x5100；output GET L/R 0xa000/0x8000，SET prefix 0xa000/0x9000。fake 独立按 literal/mask decode，拒绝 reserved/invalid direction；不能再用生产错误常量当 oracle。mono 只有 LEFT，RIGHT GET 不存在返回 0，SET channel flags 不制造 RIGHT；生产只验证真实 LEFT。

每 amp 的 dirty bit 必须等真实左右都成功还原才清；错误或不确定写入保留。多连接 Selector/Pin/AudioInput 配实际 selected edge，single hardwired，mixer input amp 独立。

IID 为实际枚举 AFG 的 F20，不是 root parameter1。每 AFG 独立，codec.subsystem_id 只是明确标注 first-AFG compatibility value。PCISubsystem 与 FG IID 不混用。

Power PS_Error bit8；Set3:0、Act7:4；D0 readiness 有真实 task wait/deadline，其他状态按 AFG 依赖语义。0dB Offset6:0、NumSteps14:8，Offset<=steps；gain/mute 能力分离，所有 present amp snapshot/0dB/restore；最低增益不是保证静音。

group association1..14、同AFG、eligible analog pins、unique sparse sequence 排序；0 无可用组，15 独立 endpoint。WCAP channel count 为 (((caps>>13)&7)<<1 | (caps&1))+1。distinct feasible converter/path；允许保守拒绝未支持的共享 signal layout，但不丢合法成员、不发明 sequence15 mirror、不伪造 channel mapping。

### 9.5 Repair 1 的实际测试证据与不足

最终源码有 codec/controller PASS、strict host sanitizer/Werror PASS、优化 freestanding TU PASS、Doxygen PASS、正常 root kernel target PASS、两仓 diffcheck PASS。复审没有重跑这些 suite，而是读取完整固定 diff、当前五文件 SHA 和实际完整日志，找到尚未覆盖的反例。

| task-3-fix-1- 前缀的日志 | 真实范围 |
| --- | --- |
| I2-red / I2-green | 原 production output SET 无方向被独立真 wire fake 拒绝 → 正确协议通过 |
| I3-attempt-1-failed / I3-red / I3-green | 首次 fake dimension setup 失败单独保留；RED 第一 LEFT case；GREEN 两侧非对称失败/成功/重试 |
| I4-red / I4-green | Pin 未选 index1 的 RED；GREEN 多 Pin/ADC、还原与读回失败 |
| I5-attempt-1-failed / I5-red / I5-green | 缺字段 compile 不冒充语义；随后真实 F20/root query RED→GREEN |
| I6-red / I6-codec-green / I6-controller-green | preexisting Error、普通 delay、timeout/retry、IF helper；缺新 SET 后 error 且旧 Act=target |
| I7-offset-red / I7-zero-db-red / I7-green | 不合法 Offset / 无 mute gain 未写 0dB 的 RED→GREEN；缺所有 amp 无 mute、prior pin 已开 |
| I8-red / I8-iteration-compile / I8-green | RED 第一 non-LIFO stop；字段适配 compile 单独保留；GREEN 两种 stop/零写冲突/last restore retry |
| I1-red-api / I1-green | missing group API compile RED，未执行 solver；GREEN 既有 group 测试，缺早成员重匹配和中间容量反例 |
| M1-red / M1-green | 生产 cached count256 dump 输出边界；不证明256不同有效节点枚举 |
| codec-controller-final-2.log | 实际 python3 tools/test_hda.py codec controller，二 suite PASS/exit0 |
| strict-host-final.log | 原 final 失败：2个 uint8>=256 type-limits + 1个 signedness Werror，实际诊断后来如实追加 |
| strict-host-final-2.log | 修复后严格 ASan/UBSan compile/run exit0 |
| freestanding-final.log / existing-object-nm.log | TU exit0；实际 undefined 只有 hda_controller_sleep_ms、hda_exec_verb、hda_phys_to_direct_map_page，无 memory helper |
| doxygen-audit-final.log | 40 codec definitions、26 header declarations 逐名 tags PASS；语义独立复审 |
| root-make-kernel-final.log | 普通现有 kernel target exit0，主要 headers/install，无 HDA link |
| diffcheck-final.log | 两仓/source 空白 exit0；旧 command quoting 展示不完整，不能冒称完全可复制 shell transcript |

表中省略 .log 的行实际都有 .log 后缀。所有失败与过程边界保留，不重写成理想 TDD 历史。Repair 2 使用新 task-3-fix-2-* 日志，完整记录命令/输出/exit。

### 9.6 冻结身份与修复快照

task-3-fix-1-review.diff 为完整五文件固定修复差异，3341 行。task-3-fix-1-before/after.json 与 blobstore 保存前后内容。task-3-fix-2-before.json 已在源码仍冻结时保存，交接时未覆盖。

交接再次核验以下当前 SHA，全部 MATCH Repair 1 after：

| 文件 | SHA-256 |
| --- | --- |
| kernel/reliefnt/drivers/hda/codec.c | 4974bbe4d5bf5c3040ee25d12920b1ddfb014aee7499951ad7bbfc7418c80e70 |
| kernel/reliefnt/drivers/hda/controller.c | 5668eb1989f9ca32bb61c6c4ab0a371e74cdced19e32c30a7b94e6220033fba1 |
| kernel/reliefnt/drivers/hda/hda.h | eadd80989459f90e4f7eaf13fae085c24c09470bbeedb8246af88f471c4b3976 |
| tools/tests/hda_codec_test.c | 17689a2427f438942760a7483abf2a91ad584e733a0ff8fb59972022879ee5a5 |
| tools/tests/hda_controller_test.c | f67bcd63c533a9ae5c1de6095b60adc394ef9cddc653d52cc66d2bd520c465dd |

交接新增 ignored handoff-production-freeze.json，记录两仓 35 个 modified/nonignored new 生产或测试文件的 hash 与 HEAD/status，排除 docs。用于确认此次交接没有生产源码变化，不替代任务前后完整 snapshot。

本次新增交接文档/提示词并更新计划索引在旧 Repair 2 before 之后发生；后续 package 可能包含这些 main 文档差异。它们是已知交接 metadata，不能覆盖旧 before 或谎称全包只有生产文件。审查说明应识别这些文档，仍保留完整差异。

## 10. H4 正式交接合同：实现尚未开始

已有 task-4-context.md、task-4-format-ruling.md、task-4-implementation-instructions.md、task-4-review-instructions.md。H3 已接受；这些文件已刷新为正式 dispatch contract。先保存 `task-4-before.json`，再以 GPT-6.1-Sol/xhigh 实施；H5/H6、模块注册和真实 guest 仍不在 H4 范围内。

### 10.1 native memory format 与旧表兼容

原 audio_params 24 bytes 只有 precision/frame bytes，没有 selected memory format/subformat；caps 40 bytes 没有每格式 msbits，也没有 BDL geometry；ops v1 88 bytes。h4-private-layout-preflight.c/log 实际编译 canonical 当前私有头，非手写镜像。

草案裁决：保持这些旧对象与 v1 prefix；向 ops 尾部追加两个 optional size-gated callback（selected format capability / explicit-format prepare），使用新的 fixed private metadata 与 extended params，内含旧 params by value 和明确 format/subformat/msbits。注册接受原 88 bytes，copy only fully covered fields；部分 function pointer 不可 raw min(size,newsizeof) 拷贝成非空截断地址。边界 size、guarded old allocation、full extension 都要测。核心 query/prepare 仍有 owner pin、waitable contract，旧 S16 行为保持。

HDA20/24-bit 数据在32-bit container 的 MSB 端，native Linux LSB S20_LE/S24_LE 不等价。保守 native S32_LE + truthful msbits：STD highest common precision；MSBITS20/24 需 matching raw cap；MSBITS_MAX 仅 raw32。S16需 raw16。native mmap 与 read/write 同布局；不靠 sample_bits 推 Linux memory format。

8-bit signedness/mono packing尚未由精确硬件源或目标样本证明，不直接公布 S8/U8；保留 raw8 metadata。raw384k 不代表 descriptor 编码有效。A2/A4 后续必须消费显式格式、memory SAMPLE_BITS 与 significant msbits，不能依 width 猜 format。

### 10.2 实际 stream/BDL 边界

Intel 规范：BDL 2..256 entries；每 buffer start128-byte aligned；length 是正整数 Words。若一个 period 一个 IOC BDLE、contiguous PCM ring，则 period_bytes 必须 multiple128，period count2..256；这是设计导致的 period step，不是所有 BDLE length 的通用128限制。新 bounded metadata应给 geometry，让 A2/A4 refinement 输出可 prepare 的参数，其他卡旧 fallback不强加HDA要求。

WALCLK offset0x30 为32bit24MHz，约179秒 wrap，controller reset重置；600秒 Doom 跨多次 wrap。SDSTS BCIS2/FIFOE3/DESE4 W1C，FIFORDY5 RO；LVI7:0>=1，高8reserved；LPIB可以等于CBL作为wrap边界；SRST须RUNclear再assert/deassert readback；bidirectional DIR先配置。

不能从单 modulo pointer 或 clock 猜多圈进度；证明不了应 XRUN。trigger/pointer/IRQ 有界、无 sleep/codec sync verbs/usercopy/alloc；prepare/close task-context。stop readback失败保留 uncertain DMA；真实 callback generation、pause、capture完整period、pos-buffer/LPIB fallback等按任务验收。

GCAP descriptor total须在 stream register/INTCTL容量、15个tag与核心/账本资源实际界限内验证，不能shift溢出/截断公布无backing流。BDL/position DMA初始化预分配、精确tuple保存；PCM DMA借核心。

### 10.3 group→converter 全流格式

Intel7.3.3.8：每 Converter Format 必须匹配完整 controller Stream Descriptor format，包括全流 CHAN，哪怕 DAC 只有 mono/stereo。group中每 DAC 用同一全流format，各选自己的首 channel/range；不能把每member channel_count写成Converter CHAN，不能以单DAC WCAP小于总channels拒绝有效group。

multi-channel DAC>2若真公布，另有 F2D/72D zero-indexed Converter Channel Count，须真实readback/rollback；与全stream CHAN区分。rate/precision相交每selectedmember，actual rendered range容量另检。

Table84：format可跨Dx保留，Stream/Channel ID会因Dx重置。H4须H3真实ready后绑定，tag/format release在last power restore前验证。缓存tag不等硬件绑定。

## 11. H5 与 H6 的既知集成问题

H5 的 task-5-context.md 是草案。H3当前activation初始化0dB、stop恢复snapshot；H5用户成功改变音量/mute不能被下一prepare或旧snapshot恢复抹掉。需明确 desired-control/baseline所有权、实际hardware readback、原子rollback，测试播放中改音量、STOP/reprepare、route switch与独立capture source。不能只显示cached百分比。

jack worker消费H2 submit/poll/take_unsolicited，单flight/全局有界budget；IMMEDIATE或无unsolicited pin真实100ms polling。queued事件不等当前pinsense，overflow重采样。quirk需要真实PCI/FGIID/codec/revision证据，不从宿主Linuxdump复制未经验证策略。

H6 的 h6-qa-preflight.md列出必须解决的五个集成 checkpoint：

1. **manager retention bridge**：controller destroy负错误保留 unsafe DMA；现有 manager void fini 后自动释放owner资源/模块，必须桥接 unload/init rollback/retry失败，保持旧v1/旧五驱动，实际manager/provider fixture验证 IRQ停、DMA/MMIO/image留存、恢复后retry才回收。H2独立destroy成功不覆盖此问题。
2. **pending MSI vector reuse**：现有free只drain已进入ISR，没有APIC pending-before-dispatch drain/quarantine。deferred旧vector释放后投递新owner风险；hardware interrupt没有software generation标签。H6需真实模型/机制与定向审查，不声称activeISR fixture已覆盖。
3. **BAR probe前BDF所有权**：H2 probe会临时改PCIcommand/BAR，要求callerowns/quiesced；当前排他MMIOlease在probe后才拿，manager没有明确pre-init重复模块排除。copied/renamed第二HDA模块必须在触碰首模块运行设备前拒绝。选择真实PCIclaim或有验证duplicate exclusion，旧模块回归。
4. **controller fatal→多codec card断开**：H2只有一个representativecard/deferredtoken；H6每controller多个card须断开所有受影响codec/stream，不能仅覆盖最后ID；另controller存活。IRQ只queue，task unregister/stop处理保留资源。
5. **真实guest probe launch**：旧 tools/test_execution_lock_qemu.py 使用旧Pythonimagebuilder、legacyGRUBautospawn，不能当canonicalMake/currentkernel启动证明。现OpenRC/inittab没有证明serial输入shell。H6需owned QA overlay/OpenRChook或真实可验证交互，并由A6实际probe输出marker证明启动。

H6还负责 hda.c、Make列表、driver install/publish/rootfs staging、SDK/运行选择、旧AC97/ES1371adapter诚实能力。当前 mk/run.mk默认AC97；HDA须参数化实际controller+codec，保留旧设备选择。**hda.drv尚不存在于真实构建/安装/镜像证据。**

## 12. ABI 与 SysV SHM 后续注意事项

A1采用锁定 Linux v6.14 native x86_64 LP64 UAPI原始头及依赖；canonical导出、runtime/SDK/docs闭环，不维护手写镜像，不将kernelprivateheaders公开。

已预研的重要布局：PCM hw608/sw136/status152/sync136/xferi24；ctl info272/value1224/list80/event72。HW ioctl0xc2604111、SW0xc0884113、CTLREAD0xc4c85512、WRITEI0x40184150；PCMprotocol0x020012、CTLprotocol0x020009。这些是参考对照，A1/A6仍要实际独立probe，不是目前guest支持。

A2/A3核心frames，read/write bytes，WRITEI/READI frames。真实PCM状态、64-bit引用、waitablecallbacks在execution外；POSIX fd/OFD由dup/fork/SCM_RIGHTS共享，短I/O/nonblock/EINTR/POLL、用户copy分段<=4096、close/unmap/disconnect生命周期要实测。

A4fixed-point参数相交与refinement；nativeold/newstatus/controlmmap；捕获data **允许PROT_READ|PROT_WRITE**且须READ，受fd权限约束，这是Linux/alsalib实际行为。早期“capture拒绝WRITE”已经纠正，不再恢复错误规则。status拒绝WRITE；VMA max_prot与backingrefs；data映射存续HW_FREE/HW_PARAMS返回EBADFD而非释放已映射页。

A5 ELELOCK owner是OFD，最后close释放；真实numid、events/TLV、control队列；OSS mixer、fragments、byte queue与ALSA保持一致，不用伪寄存器或固定成功值。

### A7：19号必须在11号A6之前

dmix/dsnoop需要nativeSysVSHM，当前内核private共享内存设备不等SysV segment。A7补入已授权共享音频需求，顺序A5→A7→A6。task-19-brief.md在执行副本、映射和ABI正式计划都有；task-19-context.md为草案，必须A4/A5接受后刷新真实VMA backinghooks。

实际当前分派源码：syscall.c、syscall_ipc.c、syscall_sysv_sem.c、syscall_internal.h；VMA syscall_mm.c、scheduler sched/sched.c和sched.h。现task_vma还没有A4计划的genericaudio/devicebacking字段，不推定它已经存在。附件identity不同于splitVMAcount，CLONE_VM不凭空增attachments；fork/exec/exit/partialunmap/read-onlymaxprot/RMID最后释放必须实测真实provider。

**已纠正的重要IPC_64错误**：旧草案曾用musl genericfallback0x100推断x86目标，实际src/internal/syscall.h先include arch/x86_64/syscall_arch.h，后者IPC_64=0。nativeLinuxv6.14 shmctl独立选64bit结构，不strip rawcmd0x100；IPC_STAT|0x100应EINVAL。现sem测试故意拒绝raw0x100是正确边界，不以旧草案擅自修sem。纠正后的task-19-context.md与账本为权威。

Linux linux/shm.h里的shmid_ds/shminfo标为obsolete；LP64 wire用asm-generic的shmid64_ds/shminfo64。libcmusl本地genericbits/shm.h，无x86_64 override；独立编译libc/canonicalview避免旧tag重定义。

native-shm-flag-preflight.c/log是strict host ownedIPC_PRIVATE4096 probe：libc/native正常STAT成功，rawOR0x100 EINVAL，RMID自身segment成功，无IPC遗留。它是hostglibc/Linux及本地muslsource宏检查，不是muslReliefOSguest证明。以后host测试只改ownedsegment、不碰globalIPC或其他用户segment。

## 13. Doom、库、CLI、GUI 与最终验收

U1锁定上游alsalib/utils及Nuked，rootMake+短Shellconfigure适配、canonicalcomponent/stage成员清单、sysroot/SDK实际链接、许可证归属。不要用宿主lib路径或新Python生产builder。

U2为opt-in libreliefos-audio.so.1及soundctl，cards/controls/route/capture/watch/test，用真实labels/errno/TLV；基础runtime不强制ALSA。

U3独立SoundPageworker，不阻塞Desktop/UI；OpenRC alsactl restore持久化；installer运行隐藏声音页且无无谓ALSA依赖。

U4已有task-16-context.md草案及实际端口检查：

- 跟踪的 userland/apps/doom/main.c、reliefos_audio.c；普通rg可能因ignore隐藏目录，显式路径或--no-ignore读取。
- OSS write_audio直接返回POSIX libc write；现pendingloop比较-EAGAIN错误，因为libc返回-1+errno。adapter只翻译一次；ALSA本身负errno不要再翻译。
- 现source.position/step为uint32 16.16，长音效会phasewrap；改64-bit有真实长样本回归。
- 非阻塞短写保留byte tail（包括两字节OSS shortwrite）、finitequeue、EAGAIN/EPIPE/ESTRPIPE恢复；仅整块被queue接受后推进mixer/musicclock，不能无声丢尾。
- mk/userland.mk使用wildcardappC/汇编与选doomgeneric源、FEATURE_SOUND，排除SDL/Allegro/mus2mid；新源要在真实Make/link/dependencyclosure内，保留DG公开模块入口。

U5与U4同acceptedoutputclock，MUS/MIDI/GENMIDI合法银行、175*36+8及#OPL_II#、Nuked18voices按frame事件；仅无效银行fallback，不以恒定蜂鸣代音乐。动态库许可证、stage与SDK闭环。

U6需实际上游多应用同时播放、dmix/dsnoop录音、amixer/alsactl、GUI外部events、Doom600秒；CPU+IOstress60秒，XRUN0，频率约1%/时长约2%容差，mute RMS<=1LSB，正确LR/knowninput/capture。两种physicalcodec门槛要真机证据；拿不到就如实记录，不能编造。

## 14. 依赖锁与主参考源

下列版本/哈希为此前已核验预研材料；U1/A1仍需进入真实依赖锁/显式fetch/构建stage验证，不代表相关软件已移植：

| 依赖 | 锁定身份 |
| --- | --- |
| alsa-lib | 1.2.14，SHA256 be9c88a0b3604367dd74167a2b754a35e142f670292ae47a2fdef27a2ee97a32 |
| alsa-utils | 1.2.14，SHA256 0794c74d33fed943e7c50609c13089e409312b6c403d6ae8984fc429c0960741 |
| Nuked OPL3 | 1.8 / commit765ec962e473aeb767e4cba74ffdc8f588ffbfe8，codeloadSHA256 2fad908c3904d3ef51b55e0d6a8980c7142942e2d0baa7e99bf038cf0a41199d，LGPL2.1+ |
| Linuxv6.14 asound.h | SHA256 ec647abccd554aab1ebbf77af42efac11748384f027870eb90cba974b6bcda0a |
| Linuxv6.14 tlv.h | SHA256 74b97d2cae70e67fd7bc5750dd46e0b9976d390fb817d98630df6ba9c2d33d61 |

主参考：[Intel HDA specification](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf)、[ALSA PCM documentation](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html)、[alsa-lib1.2.14 pcm.h](https://raw.githubusercontent.com/alsa-project/alsa-lib/v1.2.14/include/pcm.h)、[Linuxv6.14 hda_codec.c](https://raw.githubusercontent.com/torvalds/linux/v6.14/sound/pci/hda/hda_codec.c)、[Linuxv6.14 nativeSHM](https://raw.githubusercontent.com/torvalds/linux/v6.14/ipc/shm.c)。

Linux实现只做行为/规范参考，不复制GPL内核实现。不同版本QEMU源码不能代表本机QEMU11.1.1实际guest行为；所有能力claim仍需要目标验证。

## 15. 构建/测试/宿主预研的实际范围

根Make为唯一生产入口，O=out/audio-hda。15个锁定third_partygitlink及childkconfig已从本地现有源初始化；下载只允许显式fetch，新下载使用有所有权的worktreecache，原cache只读。

常规后续编译命令：

~~~sh
cd /home/xiaobai/.codex/worktrees/intel-hda-audio-plan/ReliefOS
make O=out/audio-hda RELIEFOS_CACHE=/home/xiaobai/Projects/Projects/ReliefOS/cache/downloads kernel
python3 tools/test_hda.py codec controller
git diff --check
git -C kernel/reliefnt diff --check
~~~

上面是相关任务稳定修改后的示例，不要求接替者无变化重复运行所有suite。H3Repair2 exactflags/strict/TU/nm/Dox应从现报告/简报取得，记录自己的完整新命令与输出。新源变化/失败/未答风险才重跑受影响检查，不make-B、不覆盖旧log、不重复宽泛suite耗时。

之前基线/任务已有e1000、Linuxdescriptor4场景、LinuxABI3、UAPI82C/C++、headerexport82 exact/private/residue等通过日志。它们属于当时源码证据，不等当前完整LinuxABI实施。19 unique warnings基线未掩盖，不凭增量少warnings声称全量无warning。

宿主只读声卡参考：AMD1022:1487HDA、RealtekALC897 CAD0、vendor0x10ec0897/rev0x100402，host-hda-readonly-reference.md及codec-reference.txt保存原始dump。没有改hostcontrol；这是宿主Linux当前配置，不是ReliefOS physical证据。

本机QEMU11.1.1 CLI帮助确认 intel-hda/ich9-intel-hda msi=on/off/auto、hda-duplex audiodev/cad/mixer。host-capture-preflight/result.md记录私有ALSAfileplugin known1秒48k stereo left1kHz/right500Hz实际read1024frames/2048samples匹配。

另有owned no-disk/nodefaults pausedq35：IntelHDA msi-off、hda-duplex CAD0/privateALSA输入/sink成功初始化，QMPquery-status prelaunch/runningfalse、QMPquit exit0。它只证明device/backendCLI可初始化，没有启动ReliefOS、DMA、QEMU真实inputread或guestcapture。none/WAV后端不能证明录音样本。

交接时没有本任务遗留exec/PTY、ownedQEMU或ownedIPCsegment；不全局kill用户进程。QMPsocket须在支持Unixsocket真实磁盘目录 /home/xiaobai/Projects/Projects 下，不用/tmp、tmpfs或DrvFs。H6使用owned隔离镜像/snapshot与输出，禁止修改用户VM磁盘。

## 16. 审查包、Agent 身份与证据保存

审计目录里的 review-worktree.py：

~~~sh
python3 .superpowers/sdd/2026-09-30-intel-hda-audio/review-worktree.py snapshot 4
python3 .superpowers/sdd/2026-09-30-intel-hda-audio/review-worktree.py package 3-fix-2
~~~

snapshot参数写 task-参数-before.json，package读同名before，写after/review.diff。脚本会遍历两仓tracked+nonignorednewfiles、SHA256/blob；工作目录必须worktree根。**Repair2before已经存在，不运行snapshot3-fix-2覆盖它。** 下一任务before需最终合同/文档同步后再存。

H1/H2各original、repair1、repair2review.diff与before/after完整保留。H3original五文件包、Repair1五文件包、Repair2before、报告、日志和blob全部保留；blobstore已有约93MB以上历史，勿clean。新文件不能遗漏。

交接可见旧Agent：

| Agent | 模型/作用 | 状态 |
| --- | --- | --- |
| /root/implement_h3_luna_xhigh | Luna/xhigh，H3原实现+Repair1 | completed/frozen，可访问时Repair2 followup |
| /root/review_h3_fix1_sol | Sol/high，Repair1 directed review | completed，结论未过 |
| /root/review_h3_sol | Sol/high，H3原fullreview | completed，原FAIL |
| /root/implement_h2_luna_xhigh | Luna/xhigh，H2及两轮修复 | completed/accepted |
| /root/fix_h1_luna_xhigh | Luna/xhigh，H1两轮修复 | completed/accepted |
| /root/implement_h1 | 早期Sol/high原H1，模型要求之前 | 历史已结束，不恢复为新实现 |

曾派Astra reviewer在开始有效工作前usage失败，无报告/接受；后续已改Sol。旧Luna/high H1repair在用户指定xhigh后中断，0源码变化；不要恢复该旧Agent。已completedAgent不需要为释放slot手工close，当前工具无close。

## 17. 接替后操作清单

- [ ] 读本文及AGENT/skills，确认canonicalworktree；原仓只读。
- [ ] 只读核验HEAD/status、五H3SHA、handoff-production-freeze；若不同，先查明外部变化，别覆盖用户工作。
- [ ] 读完整Repair1review与Repair2brief，保留before与旧logs。
- [ ] 可访问则followup原Luna/xhigh；跨会话不可访问则一个新Luna/xhigh接替，先确认无旧写入者。
- [ ] Repair2 I1替代分配+整path容量、I6新SETreadback、I7真实pin gate，各有实际RED/GREEN。
- [ ] 最终稳定source、严格host/TU/nm/Dox/正常Make/两仓whitespace；控制者读完整报告与实际log。
- [x] package3-fix-2，固定diff/after，新的 Sol/high 定向 review；Repair 3 再固定 package/diff 并完成 I1 定向复核。
- [x] 无 open Critical/Important 才接受 H3，更新 5 步 checkbox、索引、execution-plan、ledger。
- [x] H4 合同正式化：真实 H3 接口、selected format/msbits、geometry append-only、scope/test 完整。
- [ ] 保存 `task-4-before.json`，由 GPT-6.1-Sol/xhigh 实现 H4。
- [ ] 按表执行所有后继任务，尤其19/A7在11/A6前。
- [ ] H6真实module/manager/stage/QEMU，U6实际上游+GUI+Doom600秒证据；physical未测如实说明。
- [ ] 未获得Git授权前保留可review工作树，不提交、不推送、不归档。

当前项目仍需实质实现，交接之后的第一项是 H3 Repair 2。不要因为已有测试PASS、准备了H4草案或handoff文档而声称声卡/Doom已经可用。

## 18. Post-handoff H4 Repair 2 continuation (2026-10-01)

The historical H4 status above records the pre-implementation handoff. The
successor continued inline in the designated worktree after the user cancelled
subagent use. `task-4-before.json` was preserved; the new package is
`task-4-fix-2-after.json` plus `task-4-fix-2-review.diff`.

H4 Repair 2 now has production changes for complete group converter cleanup,
prepare-time route preflight before BDL/SD publication, retryable cleanup
metadata, and one bounded position helper shared by pointer and IRQ service.
The real targeted RED/GREEN logs, final host suites, strict production TU
checks, sanitized symbol audit, focused Doxygen audit, ordinary Make result,
and whitespace result are listed in
`.superpowers/sdd/2026-09-30-intel-hda-audio/task-4-report.md` and the
associated `task-4-fix-2-*` files. The inline self-review is
`task-4-fix-2-review.md`; no independent review agent was run after the user’s
explicit cancellation.

This is still source/host evidence only. HDA module link/load, guest playback
or capture, Doom audio, and physical-device validation remain unperformed.
No commit, push, or archive was performed.

## 19. Post-handoff H5 controls continuation (2026-10-01)

The successor continued H5 inline in the designated worktree after the user
cancelled subagent use. The H5 baseline is preserved as
`.superpowers/sdd/2026-09-30-intel-hda-audio/task-5-before.json`, copied from
the final H4 Repair 2 snapshot without overwriting it.

H5 now has production `kernel/reliefnt/drivers/hda/controls.c` and the host
fixture `tools/tests/hda_controls_test.c`. The controls expose truthful
registration metadata, capability-derived integer/dB volume information,
actual amplifier readback, mute capability gating, independent capture-source
state, deduplicated jack state changes, bounded unsolicited/polling service,
and task-context auto-mute. Route state and control policy are restored when a
later route operation fails. The focused fixture includes the required third
simulated verb route failure and verifies speaker/headphone active-state
rollback.

The controls suite and the complete affected host suite pass in
`task-5-green-controls-final-3.log` and `task-5-full-final.log`.
Strict freestanding HDA TU validation is in `task-5-tu-full-final.log`, the
ordinary Make result is in `task-5-make-final.log`, and the focused H5
Doxygen/whitespace scans are in the `task-5-*` logs under the SDD directory.
Earlier RED, failed-link, failed-fixture, and broad-Doxygen attempts remain
preserved as historical evidence.

The inline self-review is `.superpowers/sdd/2026-09-30-intel-hda-audio/task-5-review.md`;
there was no independent review agent after the user's explicit cancellation.
The review records one integration boundary: capture-source selection is a
validated control state but is not yet a claim of multiple physical selector
routes or selector readback. H5 remains host/source verified only. H6 still
owns module registration/staging and guest validation; no HDA module load,
guest playback/capture, Doom audio, or physical-device result is claimed. No
source was committed, pushed, or archived.

## 20. Inline continuation into A1 (2026-10-01)

The execution ledger now records H3, H4, and H5 as accepted at their documented
host/source evidence boundaries. The next approved task is A1, before A2-A5,
then A7, A6, H6, and U1-U6. A1 uses the locked Linux v6.14 native x86_64 LP64
UAPI contract; its before snapshot and RED evidence must be preserved before
any header or test implementation changes.

## 21. A1 completion and next boundary (2026-10-01)

A1 was completed inline in the designated worktree after the user cancelled
subagent use. The final package is `task-6-review.diff`; the preserved baseline
is `task-6-before.json` and the current package snapshot is
`task-6-after.json`. The production export contains the pinned Linux v6.14
ALSA PCM/control/TLV and OSS UAPI headers, with the canonical source URLs and
SHA-256 values recorded in the files and report. The raw reference copies are
under `tools/tests/fixtures/audio-uapi/linux-v6.14/` and are test-only.

The final ABI runner passes 128 enum values and 285 PCM/control/OSS constants
in native and time64 modes. The shared UAPI and exact export checks pass, and
the generated SDK include tree passes the installed layout probe. The supported
build target is `headers_install`; the literal top-level `headers` target from
the brief does not exist and its failure is preserved. `runtime sdk` succeeds
when pointed at the existing read-only reference cache; no cache files were
written. Full details and the inline review are in
`.superpowers/sdd/2026-09-30-intel-hda-audio/task-6-report.md` and
`task-6-review.md`.

A1 remains a host/source/export/build result. It does not claim a loaded HDA
module, guest ALSA behavior, physical capture/playback, or Doom audio. The
next ordered task is A2 (Task 7), beginning with its preserved before snapshot
and RED test. No source was committed, pushed, or archived.

## 22. A2 PCM state and buffering completion (2026-10-01)

A2 was completed inline in the designated worktree after the user cancelled
subagent use. The production PCM state machine is in
`kernel/reliefnt/kernel/reliefnt/audio/pcm.c`, with the shared period event
and detach handling in `audio/core.c` and the private contract in
`include/reliefnt/audio.h`. The host fake card drives the real card/stream
registration and `audio_period_elapsed` path; it does not replace the PCM
implementation.

The initial missing-translation-unit RED is preserved in
`task-7-red.log`. Final PCM state/transfer/XRUN/drain/LINK/lifetime evidence
is `task-7-green-final-2.log`; HDA stream/lifetime and Linux audio ABI
regressions are `task-7-hda-final.log` and `task-7-abi-regression.log`.
Strict ASan/UBSan `-Werror` compilation is `task-7-werror.log`, and the
ordinary `kernel drivers` build is `task-7-build-fixed.log`. The first build
failure caused by the canonical UAPI removing the old `DSP_CAP_OUTPUT` name,
and the scoped compatibility fix, remain in `task-7-build.log` and the report.
Final reruns after the last PCM source change are retained as
`task-7-green-final-3.log`, `task-7-abi-regression-final.log`,
`task-7-hda-final-2.log`, `task-7-werror-final.log`, and
`task-7-build-final.log`; all exit 0.

The inline review is `task-7-review.md`. A2 uses frame units, an explicit
Linux-like state enum, boundary checked ring arithmetic, error-to-XRUN
transition, stable two-PCM lock ordering, and release/unregister assertions.
Its evidence is limited to host/source/build behavior; no HDA module load,
guest ALSA, physical playback/capture, SSYNC hardware result, or Doom audio
is claimed. The A2 before baseline was reconstructed from the accepted
`task-6-after.json` plus the A1 handoff section after an accidental snapshot
overwrite; the overwritten file is retained as
`task-7-before-overwritten-after.json` and the provenance is recorded in the
report/review.

No source was committed, pushed, or archived. The next ordered task is A3
(Task 8), consuming the A2 PCM and lifetime contract.

## 23. Post-handoff A3/A4/A5 controlled continuation (2026-10-02)

The successor continued inline in the designated worktree after the user
cancelled subagent use. A3's dynamic `/dev/snd` control/PCM nodes, OFD routing,
read/write/poll/disconnect behavior are covered by the device fixture and the
current syscall dispatch. A4's parameter, ioctl/state, checked usercopy, data
ring mmap, and lifetime subset is covered by the PCM/device/mmap fixtures. These
are host/source contracts; they do not claim a loaded HDA module or guest I/O.

A5 now has the production control and OSS adapters in
`kernel/reliefnt/kernel/reliefnt/audio/alsa_control.c` and `audio/oss.c`, with
core per-control event sequences and task-file dispatch. Fresh evidence is in
`task-10-control-green-final.log`, `task-10-oss-green.log`,
`task-10-device-green.log`, `task-10-pcm-green.log`, `task-10-abi-green.log`,
`task-10-hda-controls.log`, and `task-10-build.log`; the detailed scope and
inline review are `task-10-report.md` and `task-10-review.md`.

A5 remains open. Shared ALSA/OSS real gain and mixer nodes, capture and
PCM-backed OSS queue semantics, hardware drain, complete control event/lock
semantics, standard application probes, module/guest/physical audio, and Doom
audio are still unperformed. The ordered next task A7 (Task 19) must wait for
A5 acceptance; A6, H6, and U1-U6 therefore remain pending. No source was
committed, pushed, or archived.

The OSS adapter then received a narrow usercopy hardening pass: integer ioctl
arguments now require `user_range_writable`, and the low-page `-EFAULT` case is
covered by `task-10-oss-usercopy-green.log`. The updated source build and
source-only whitespace checks are `task-10-build-usercopy.log` and
`task-10-kernel-source-diffcheck-usercopy.log`; this does not change the open
A5 acceptance boundary.

After that pass, the current individual regression logs
`task-10-control-regression-current.log`, `task-10-oss-regression-current.log`,
`task-10-device-regression-current.log`, `task-10-pcm-regression-current.log`,
`task-10-abi-regression-current.log`, and
`task-10-controls-regression-current.log` all exit 0. The current ordinary build
and root whitespace checks are `task-10-build-current.log` and
`task-10-root-diffcheck-current.log`, also exit 0. These are retained as the
latest source/host evidence only.

## 24. A5 OSS direction and current continuation evidence (2026-10-02)

The successor added the missing `O_ACCMODE` default-trigger behavior in
`audio/oss.c`: read-only opens enable input, write-only opens enable output, and
read/write opens remain duplex. The capture-only fixture now proves the initial
`-EAGAIN` boundary and a 2048-byte read after 512 fake-card frames. Existing
playback fixtures explicitly use `O_WRONLY` so the test flags match the Linux
access-mode contract.

The current OSS path remains PCM-backed and covers capture space/pointer,
finite playback queue space/delay/pointer, frame tails, RESET, SYNC drain,
SETTRIGGER, SETDUPLEX, and low-page integer-ioctl `-EFAULT`. Mixer masks,
volume, mute, and record-source values route through shared card controls. The
fresh regression logs are `task-10-abi-regression-current-2.log`,
`task-10-pcm-regression-current-2.log`, `task-10-device-regression-current-2.log`,
`task-10-params-regression-current-2.log`, `task-10-ioctl-regression-current-2.log`,
`task-10-mmap-regression-current-2.log`, `task-10-control-regression-current-2.log`,
`task-10-oss-regression-current-2.log`, `task-10-mixer-regression-current-2.log`,
and `task-10-controls-regression-current-2.log`; all exit 0. Kernel and driver
build evidence is `task-10-build-current-2.log` and
`task-10-drivers-current-2.log`; both exit 0.

The inline review is preserved in
`.superpowers/sdd/2026-09-30-intel-hda-audio/task-10-review-current.md`.
A5 is still open for INFO/ADD/REMOVE event masks, owner PID, blocking control
read/wakeup, dup/fork OFD lifetime, standard application probes, and reference
errno comparisons. A7 native SysV SHM, A6 diff/probes, H6 module/guest work, and
U1-U6 including Doom and physical evidence remain pending. The required order
is A5 → A7 → A6 → H6 → U1-U6. No source was committed, pushed, or archived.

The event-mask portion now has a narrow production extension: core exposes
`audio_control_changed_mask` and `audio_card_control_event_mask`, while the
ALSA control read path returns the recorded INFO/VALUE mask. The TDD red/green
pair is `task-10-control-event-mask-red.log` and
`task-10-control-event-mask-green.log`; the affected kernel/drivers builds are
`task-10-build-event-mask.log` and `task-10-drivers-event-mask.log`. This does
not close dynamic ADD/REMOVE generation, owner PID, blocking wakeup, or OFD
lifetime gates.

The next narrow A5 fix records the real caller task PID in the control-file
lock. `ELEM_INFO` now reports that PID while the lock is held, and the fixture
verifies the syscall control path with task PID 4242. The red/green evidence is
`task-10-control-owner-pid-red.log` and `task-10-control-owner-pid-green.log`;
the kernel build is `task-10-build-owner-pid.log`, with all green/build commands
exiting 0. This still does not close dup/fork OFD lifetime, blocking wakeup,
dynamic ADD/REMOVE generation, or standard probe/reference-errno gates.

After the owner-PID change, all nine current host regressions (ABI, PCM,
device, params, ioctl, mmap, control, OSS, and mixer) were rerun and exited 0;
the logs are the matching `task-10-*-regression-current-4.log` files. The
drivers and kernel builds also exited 0 in `task-10-drivers-owner-pid.log` and
`task-10-kernel-owner-pid-final.log`, and the focused source diff check is
`task-10-owner-pid-diffcheck.log`.

The following A5 slice adds bounded per-control event history in core. A
control OFD now merges all INFO/VALUE masks from its last sequence through the
current sequence, so a VALUE followed by INFO is read as one combined event.
The red/green pair is `task-10-control-event-merge-red.log` and
`task-10-control-event-merge-green.log`. Nine current-5 host regressions are
green; kernel/drivers builds are `task-10-kernel-event-merge.log` and
`task-10-drivers-event-merge.log`, with the focused check in
`task-10-event-merge-diffcheck.log`. Dynamic ADD/REMOVE, blocking wakeup, and
OFD lifetime gates remain open.

## 25. Inline control read races and corrected project count (2026-10-02)

The acceptance ledger has seven completed tasks: H1-H5 and A1-A2. A3-A5
have partial source/host results and remain unchecked. A7, A6, H6 and U1-U6
are the nine later tasks. Thus the equal-task acceptance count is 7/19 (37%),
not 9/19. The successor closed five accepted tasks after inheriting H1/H2.
`2026-10-02-intel-hda-audio-status.md` records the current Chinese status and
acceptance boundary; implementation still follows the approved dependencies.

The earlier unverified blocking-control change had two races: the epoch was
captured after the empty read, and a wake before sleep publication could leave
the task BLOCKED despite having consumed its event. The current wrapper
captures the epoch first, publishes interruptible sleep using the real wait
queue/scheduler, rechecks the epoch, and restores readiness before a raced
successful read. Syscall dispatch keeps O_NONBLOCK on the non-sleeping path.
The control fixture includes actual `wait.c` and `sched/sched.c`; test wrappers
only inject real core notifications around actual scheduler transitions.

The initial `task-10-control-blocking-races-red.log` is a fixture setup failure
(host pthread sched_attr collision). The effective semantic RED is
`task-10-control-blocking-races-red-2.log`; GREEN is
`task-10-control-blocking-races-green.log`. Tests also cover two independently
subscribed readers, disconnect wakeup, pending signal readiness and full-queue
fallback. A fixture signal-constant setup failure is retained in
`task-10-control-blocking-boundaries-green.log`; it is not passing evidence.
Nine subsequent `task-10-*-regression-blocking.log` files exit 0.

The pinned Linux v6.14 `snd_ctl_read` reference requires `-EBADFD` for an
unsubscribed descriptor; `task-10-control-unsubscribed-red.log` reproduces the
old `-EAGAIN`, and `task-10-control-unsubscribed-green.log` is the final passing
control fixture after the errno fix. The reference is retained as ignored
`task-10-linux-v6.14-control-reference.c`, SHA256
`ffaad410b8627be220e3f8cac884bd7937f89a1a161b0ab8a187f58c06767983`, downloaded
from https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/refs/tags/v6.14/sound/core/control.c?format=TEXT .
This is source comparison, not a native Linux/guest audio probe.

Build provenance correction: an agent-owned accidental subrepo forced build
(`make -C kernel/reliefnt -B kernel`) was stopped by its exact PID tree; no
user process was stopped. An ensuing subrepo retry hit that build lock and
overwrote `task-10-control-blocking-kernel.log`; that file now holds only the
retry failure, not the forced-build transcript. Neither command is valid
production validation, and the lost transcript is not claimed preserved.
All existing production sources and remaining audit files were retained.
Actual supported root builds are `task-10-root-build-blocking.log` and
`task-10-root-build-blocking-final.log`, both exit 0, using ordinary
`make O=out/audio-hda -j8 kernel drivers` without forcing recompilation.

A3/A4/A5 full acceptance remains open, including synchronization/OFD lifetime,
dynamic control lifecycle and standard application/reference probes. No module
load, guest playback/capture, physical-device or Doom audio result is claimed.
No source was committed, pushed or archived; no new subagent was used.

The first final whitespace audit is retained as
`task-10-blocking-final-audit.json`: the root check and all ten selected
source/document checks are clean, but the whole subrepo check exits 2 on the
unchanged Linux-derived `include/uapi/linux/soundcard.h` whitespace. The
accepted A1 export adaptation and the raw upstream fixture both retain their
respective A1 SHA256 values; these inherited upstream lines are not rewritten.
`task-10-blocking-final-audit-2.json` preserves an incorrect audit assumption
that the adapted export must equal the raw fixture. Its focused whitespace
checks passed, but that equality assertion failed; it is not a passing audit.
The corrected `task-10-blocking-final-audit-3.json` excludes only that header
from the whitespace check and verifies both files against their accepted A1
snapshot hashes instead.

## 26. Per-OFD pending control queue and subscription semantics (2026-10-02)

A5's bounded 256-entry global event history had two verified defects: a new
notification during element-ID lookup could be acknowledged without being read,
and slow readers could overflow the history rather than retain merged masks.
Effective semantic failures are `task-10-pending-race-red.log` and
`task-10-pending-burst-red.log`. The production registry now attaches each OFD's
preallocated queue, with one pending mask per fixed element and first-notify
FIFO ordering. Notification/dequeue/detach share the IRQ-safe registry lock;
dequeue happens before metadata callbacks, so a later notification remains
pending. The unused sequence/history helper APIs were removed; the module ABI
table is unchanged. Before-source identities are retained in
`task-10-pending-queue-before.json`.

The retained v6.14 control source also establishes negative SUBSCRIBE_EVENTS
inputs as queries, arbitrary positive inputs as enable, repeated enable as
preserving pending events, and disable as clearing them. The effective RED is
`task-10-subscription-query-red.log`; the adapter now implements these rules.
`task-10-subscription-queue-final-green.log` exits 0 and covers these rules,
notification during read, 1024 notification rounds consumed independently by
fast/slow readers, FIFO/merged literal masks including ADD, short-buffer
nonconsumption, close/reuse, and all 64 OFDs receiving all 64 fixed elements.
Previous actual wait/scheduler boundary tests still run in that fixture.

The eight other `task-10-*-regression-pending-queue.log` host modes all exit 0.
`task-10-root-build-pending-queue.log` exits 0 through ordinary root
`make O=out/audio-hda -j8 kernel drivers`; kernel.sys was rebuilt, drivers were
already current. This is not HDA module linkage, guest loading or sound proof.

The A5 brief fixes the element set after card registration and excludes user
defined controls. Earlier open-finding wording about dynamic control generation
must not require an invented dynamic registration API. Pending mask forwarding
is now tested; real descriptor dup/fork/SCM_RIGHTS/final-close integration,
remaining reference ioctl/errno review, and A3/A4 contracts still need closure.
Seven of nineteen tasks remain formally accepted. All work is inline; no
subagent, commit, push or archive was used, and old failure logs were preserved.

## 27. A3-A5 inline continuation (2026-10-02)

The successor continued inline after the user cancelled subagents. Production changes
now gate unknown standard PCM ioctls before user-pointer inspection (`ENOTTY`), return
real start timestamps through `snd_pcm_status`, execute configured playback silence
threshold/size by zero-filling the DMA ring, and complete the OSS direction/format,
fragment-bound, RESET/trigger, epoch-safe DRAIN, and hardware-pointer semantics.
The OSS fixture supplies only no-op scheduler symbols so production wait branches link;
it does not claim guest scheduler behavior.

Fresh evidence was created without overwriting prior artifacts:
`task-10-inline-a3-a5-final-audit.json`, `task-10-inline-a3-a5-final-audit-2.json`,
and `task-10-inline-a3-a5-final-audit-3.json`; the latest root command
`make O=out/audio-hda -j8 kernel runtime sdk` exits 0. Full ABI/device/PCM/VMA/params/
ioctl/mmap/control/OSS/mixer host fixtures exit 0 in the first audit; latest focused
OSS/ioctl/device reruns and the final root build also exit 0. A3-A5 remain partial in
formal count (7/19 accepted) pending real devfs permission/dents/stat, guest syscall,
signal-restart/OFD contract, HDA module/load, physical audio, and Doom evidence.

## 28. A3–A5 local source/host/build closeout (2026-10-02)

The user's latest priority was to complete A3–A5 quickly. Work and review stayed inline; no new agents or Git mutations. A3–A5 local gates are closed, bringing local milestones to10/19. The approved ABI plan explicitly schedules actual VM behavior at H6/U6 and unmodified upstream ALSA runtime at U1/U6; none of those runtime gates is claimed passed. See task-8-9-10-inline-closeout-review.md for the requirement matrix and task-10-a3-a5-closeout-audit.json for17 passing host commands plus one root kernel/runtime/SDK build, exact log/source/artifact hashes and limits. Current top-level status and SDD ledger supersede earlier7/19 snapshots while preserving all history.

New production repairs cover capability-filtered sparse devfs enumeration and stat minors; syscall ioctl OFD pin across wait/close/fd reuse; real-fd LINK lookup; trigger timestamps; silence independent of appl_ptr and acquire mmap commit before IRQ silence; OSS core lease acquisition, actual caps negotiation, epoch-safe read/write/SYNC, DROP-based RESET/trigger, partial/full byte-tail submission, U8/sparse direction support, whole-frame odd-channel geometry and actual-boundary counters; control unknown ENOTTY before bad argument validation. Actual descriptor, SCM_RIGHTS, DAC, VMA and signal fixtures are used. Final affected logs are oss-final-6, control-final-2, descriptors-final-7 and root-build-final-6 under task-10-closeout-*.log. The CLOEXEC native reference38checks is generic fd ABI evidence only, using the worktree SDK; its older kernel evidence identity precedes the last OSS/control repairs and is not the final artifact identity.

All prior failed/setup/aggregate logs remain. Odd-channel RED is effective; initial boundary RED and oss-final-5 missed the simulated period notification and are fixture setup failures, corrected in final-6. The final selected audit excludes those failures. Missing PAM was fetched only into the worktree-owned cache against its lock hash. ABI.md/SYSCALLS.md now describe the implemented interface and runtime limits.

Next breakpoint is A7(Task19), then A6 → H6 → U1–U6. Earlier A7 minimal draft was found unsafe for page/VMA/fork ownership. Its source is preserved behind RELIEFNT_SYSV_SHM_DRAFT, its production dispatch/lifecycle hooks and export whitelist additions were removed; default sysv_shm_test deliberately fails until a real implementation exists. Do not enable this draft or use its old stub GREEN as acceptance. Import canonical Linuxv6.14 shm/shmbuf headers, reuse current A4 VMA/MM ownership hooks and test actual page reclamation/attachment splitting before integration. All uncommitted code/audits retained; no staging, commit, branch, push, PR or archive.

## 29. A7 local closeout and A6 breakpoint (2026-10-02)

A7 local source/host/build gate is closed (11/19). The unsafe disabled draft was retained in task-19-before-* and replaced with canonical native SHM, production registry/PTE/VMA/scheduler ownership and fork/exec failure coverage. task-19-report.md, task-19-inline-review.md and task-19-closeout-audit.json record current sources/artifacts and13passing logs. Root build final-3 exits0; glibc and worktree musl same-source native82checks each pass on Linux only. Main ABI/SYSCALLS/CSV updated with limitations. All old RED/setup logs and path-restoration evidence preserved. No subagents or Git changes. Next task11/A6; H6 must execute the SHM probe on ReliefOS, and U1/U6 must validate real dmix/dsnoop. Physical/Doom gates remain.

## 30. A6 local closeout and H6 breakpoint (2026-10-02)

A6 local source/host/guest-build gate is closed:12/19. task-11-report.md and task-11-inline-review.md document five-mode standard libc probe, no DMA addresses,10-second deadline, byte/frame short transfer and restoration cleanup, canonical Linux v6.14 production/reference/musl ABI comparison, actual SDK include closure and97-ioctl manifest. Final evidence is task-11-closeout-audit-2.json (sources/logs/kernel/SDK/probe hashes); audit1 retains pre-document-correction identity. Latest guest binary is out/audio-hda/qa/audio-guest-accepted; its ABI only ran on Linux.

A6 found and repaired prior A5 adapter rejecting1224-byte control value due1024 buffer limit; task-11-control-value-red-1.log fails actual public adapter, host-final-1 passes new canary/bad-pointer/read/write regression. Root whitelist had no sound/asound.h or sound/tlv.h and no patchkey; closure RED and actual musl compilation exposed the omissions; all now exported by ordinary root make. linux/time.h now respects both libc guards. Generic musl ABI fixture now saves independent libc expectations before isolating obsolete raw Linux ipc_perm definitions, leaving canonical production IPC untouched. Two failed logs named green and all RED/setup history are retained, not selected as acceptance.

Next Task12/H6 brief was read fully. Must link/stage hda.drv alongside old five modules; add real module init/fini and manager retention bridge when destroy cannot quiesce DMA, then root full image build and owned isolated QEMU guest probe/waveform. hda_controller_destroy() error must retain DMA/MMIO/image, stop IRQ and allow retry. Original brief example /usr/lib/reliefos/drivers conflicts with current canonical /drivers root layout; use existing documented production layout after actual stage inspection. QMP socket must be on worktree disk, not /tmp/tmpfs. No new module, guest sound, upstream ALSA, physical audio or Doom proof yet. Continue inline only; preserve uncommitted sources and audits, no commit/push/archive.


## 31. H6 inline integration in progress (2026-10-02)

Correction to section30: actual rootfs staging and manager scans use /usr/lib/reliefos/drivers; /drivers is the ESP path and the prior docs were stale. Root products retain all six modules. Root images build2 and product checks pass; current build3 follows a real QEMU PREPARE repair. H6 remains open (12/19 local gates closed).

Actual manager ELF init rollback/unload now respects append-only teardown failure reporting: IRQ stopped, unsafe DMA/MMIO/image retained, first negative error diagnosed and retry reclaims actual provider leases. manager-green1 and module-host4 pass actual paths; module fixture covers20Nth metadata/DMA failures. Codec PCM precision/format verb encoding, real path amplifier and position-DMA stop bugs have separate RED/GREEN evidence.

QEMU qa/hda registered a usable HDA codec card. qa/hw1 boots normal OpenRC with standard-libc guest probe: lifetime/control restoration pass; play/capture PREPARE EIO and WAV invalid. QEMU fixed pin controls are immutable; six exact codec IDs with input/output-only pin caps now keep verified fixed pins and gate converter amps. task-12-fixed-pin-red1 is effective, green1 codec+phase pass. host-final1 phase link failure was a fixture dependency regression, corrected without claiming its aggregate passed. qa/hda-output MSIoff/pc loading failure retained. Await rebuilt guest/waveform before claiming playback.

Pending H6 matrix, input-data capture, deferred multicard failure/jack handling, legacy v1 adapter and final inline review. Continue inline only, no subagents and no commit/push/archive. All source/evidence retained.


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


## 37. H6/U1/U2 接受与 U3 断点（2026-10-03）

H6 实际三后端波形和 guest module 身份、U1 upstream/SDK/cache/recovery/dry-run 已通过最终内联审查。U2 新增控制库/CLI/tone，实际目标构建、rootfs/SDK 和 ASan/UBSan 客户端通过；整体15/19。具体证据和限制见 SDD progress 第37–38节、task-12-final-h6-review-1.md、task-13-u1-review-1.md、task-14-inline-review-1.md 与 task-14-closeout-audit-1.json。root 全套测试仍有旧 Pages 链接及必须保留的脏子仓合同失败，不宣称全套通过。用户取消子代理，所有后续工作内联。下一步按原序 U3 设置页/worker/原子状态持久化，再 U4/U5 Doom 和 U6 全链路验收；无提交、推送或归档。


## 38. U3 接受与 U4 断点（2026-10-03）

U3 已在当前唯一 worktree 内联完成并接受，整体 16/19。settings 声音页采用 worker/model 快照，安装器路径为无 ALSA 的 no-op，OpenRC 状态保存具备临时文件与失败回滚；目标构建、rootfs/package 检查和 fixture 全部 exit 0。证据为 task-15-build-1.log、task-15-packages-green-1.log、task-15-settings-run-1.log、task-15-inline-review-1.md、task-15-closeout-audit-1.json。剩余顺序为 U4 Doom 音效、U5 MIDI/OPL3、U6 客体和最终审计。


## 14. 2026-10-03 U4/U5 successor continuation

U4 与 U5 已按原顺序在指定 worktree 内联闭合到 host/source/target-build gate，保留所有未提交源码与旧审计证据。U4 证据为 task-16 inline review/closeout audit 及 output/mixer/app-doom logs；U5 证据为 task-17 inline review/closeout audit 及 music/app-doom logs。当前总体 18/19，剩余唯一任务为 U6。

U6 仍不能由这些 host/build 结果替代：要在新生成或明确同一来源的镜像上执行标准 ALSA 多客户端、录音、soundctl、设置页、Doom WAD 和长时压力，并保存 QEMU/波形/串口证据；无物理 codec 时如实保留 physical gate 未运行。

## 16. 2026-10-03 U6 software gate closeout

继任者已在指定数据盘 worktree
`/home/xiaobai/Projects/Projects/ReliefOS-intel-hda-audio-plan` 内联完成剩余可执行
U6 门。ALSA `dmix`/`dsnoop` 默认配置配套 direct-plugin server patch，标准默认播放、
录音、并发多客户端、soundctl test 和 controls 全部通过。Doom `/dev/dsp` 使用阻塞
OSS 提交，并在首个 `-EPIPE` 时执行 `SNDCTL_DSP_RESET` 后重试；真实 Freedoom WAD
headless 关卡记录 MIDI 注册和首个非零 PCM 混音块，最终无静默降级日志。

生产镜像构建 exit 0，最新 QEMU 结果为
`out/audio-hda-u6/qa/std17-final/result.json`：guest core ABI、timer、播放波形、
采集数据、controls、lifetime、83 项 SysV SHM 及 standard probe 全部 PASS，QEMU
exit 0。镜像、串口和结构化结果 SHA 以及剩余 Xorg GUI、physical codec、600 秒压力
边界记录于 `docs/superpowers/progress/2026-10-03-u6-closeout-audit.json`。

因此可执行软件 U6 已闭合；正式账本仍按 18/19 保留，GUI 按用户决定转入后续 Xorg，
当前环境没有 physical codec，长压与物理门未运行。无提交、推送或归档。

## 15. 2026-10-03 U6 preflight continuation

在同一指定 worktree 内联完成 U6 预检入口与新镜像运行。q35 Intel HDA
duplex/4CPU/MSI-on 以及 q35 ich9-intel-hda duplex/4CPU/MSI-on 均通过
启动、ALSA 播放、波形、录音、controls/lifetime 和 SysV SHM；pc/1CPU/
MSI-off/output-only 的严格波形时长失败被原样保留。`--duration 600` 入口
明确把可见 GUI、Freedoom WAD 长时运行和物理 codec 标为未运行，因此 U6
尚未接受，整体仍 18/19。证据集中在 `task-18-inline-review-1.md`、
`task-18-closeout-audit-1.json` 和 `docs/superpowers/audio-validation/2026-09-30.md`。

## 17. 2026-10-04 post-restart U6 continuation

After reboot, the preserved 9.6G `/tmp` backup passed its complete `SHA256SUMS`
check. The QEMU capture fixture now provides enough known input for the requested
Doom duration, and the long-run reporter scopes Doom XRUN checks to the Doom
serial interval while retaining a separate whole-run field.

On image `fe47bff3c733f1283ae8f6334dfecb3c1e26c3d0205704761e1e9620701966d0`,
`qa/lts28` passed the core capture/waveform checks and all standard probe modes
with a 32 KiB PCM ring. `qa/e2e660d` completed 600 seconds of headless Freedoom
audio and 60 seconds of CPU/I/O load with no Doom-interval XRUN. Its default
2,048-frame core capture preflight nevertheless returned EPIPE, and two
zero-duration overrun messages followed Doom during lifetime capture. U6 stays
open until default-geometry capture is repeatable.

Host-only enumeration now confirms AMD HDA and Realtek ALC897 hardware, but no
ReliefOS physical-driver run was performed. The desktop was not rebooted into
ReliefOS and PCI was not detached or passed through. The visible GUI remains
deferred to Xorg. Detailed hashes, commands, results, and the remaining gates
are recorded in `docs/superpowers/progress/2026-10-04-u6-resume-audit.json`.

## 18. 2026-10-04 H3 Repair 2 / U6 strict-gate closeout

继续修复从 H3 Repair 2 断点内联完成。为覆盖 Doom 运行期调度停顿，核心动态 PCM
上限与 Doom OSS 环统一为 512 KiB：Doom 使用 64 个 8 KiB period；最大 fragment
环在第一次 `write()` 前必须填满，普通小环仍保留一周期唤醒余量。PCM 扩展缓冲仍由
64 KiB inline storage 按需分配和释放，HDA BDL 能力保持在 64 period 范围内。一次性
HDA/PCM 诊断日志已全部移除，源码中不再包含 `hda-debug`、`pcm-debug`、`PCM_DEBUG`
或临时 `serial_write`。

红绿证据：`out/audio-hda-u6/qa/pcm-oss-512k-20261004.log` 中 PCM、OSS 控制/队列/
fragment 及所有可中断等待（含 `full-prefill`）通过；`out/audio-hda-u6/qa/
doom-output-512k-green-20261004.log` 通过。最终源码回归
`out/audio-hda-u6/qa/final-host-pcm-oss-20261004.log` 和
`out/audio-hda-u6/qa/final-host-doom-20261004.log` 均 exit 0；后者覆盖 Doom
output、mixer、music 的 ASan/UBSan 测试。

最终生产镜像构建 exit 0，VMDK
`out/audio-hda-u6/images/reliefos.vmdk` SHA-256 为
`0da07364d997aba7c6f304883788d76f0307186f663f999bd36c63f0cfb363a9`。
新的真实内核门禁 `out/ds13/e2e-result.json` exit 0，结构化结果 SHA-256
`43fd818f482e11650ae3243e6147d5cca4f4b3668c159a9da9817ae57caee5a8`；启动、HDA
注册、默认捕获 4096 frames/16384 bytes、48 kHz S16 stereo 1000/2000 Hz 波形、83
项 SysV SHM、standard probe 全部通过。Doom 5 秒统计为
`written_bytes=994008 startup_recoveries=0 xrun_recoveries=0 suspend_recoveries=0`，
因此 Doom PCM/XRUN 严格门均通过。`all_serial_xrun_clean=false` 仍只表示独立的
非 Doom 生命周期捕获区间有历史 overrun 标记；本次 Doom 区间为 clean。

GUI 按用户决定继续留给后续 Xorg；物理 codec 需要原生启动或独占 PCI passthrough，
当前环境未运行，故 `physical_codec_matrix=false` 仍如实保留。未提交、未推送、未归档；
所有源码、未提交变更、临时备份和审计证据均保留在指定数据盘 worktree。
