# 接替 Agent 的提示词

将下面整段发给接替 Agent。它需要访问当前机器及保留的实施 worktree。

~~~text
请接替前一个 Agent，继续 ReliefOS 的 Intel HDA、Linux ALSA/OSS/POSIX 音频接口、程序声音控制及 doomgeneric 音效/音乐项目。设计与实施已经得到我批准，不要重新规划或重复询问是否开始；先读取交接文档和当前任务证据，再从精确断点继续完成全部任务。

先完整阅读：
/home/xiaobai/.codex/worktrees/intel-hda-audio-plan/ReliefOS/docs/superpowers/progress/2026-10-01-intel-hda-audio-handoff.md

唯一实施与构建目录：
/home/xiaobai/.codex/worktrees/intel-hda-audio-plan/ReliefOS
原仓 /home/xiaobai/Projects/Projects/ReliefOS 与其下载缓存只读。主仓及 kernel/reliefnt 子仓均 detached HEAD，所有未提交/新文件和 .superpowers/sdd 证据必须保留。读根 AGENT.md；没有 stage/commit/branch/push/PR 或 archive worktree 授权，不用 reset/checkout 清理。

沿用 using-superpowers、writing-plans、subagent-driven-development、TDD、systematic-debugging、receiving/requesting-code-review 和 verification-before-completion；读取当前技能实际文件，尊重现工具与我的模型要求。控制者负责协调、文档、证据和核验，生产实现/修复交给 GPT-6-Luna，reasoning_effort 必须 xhigh。审查禁止 GPT-6-Astra，使用 GPT-6.1-Sol，H3 定向复审 high。同一时刻一个实现者；实现者/审查者不要派子 Agent。

当前断点：H1/H2 已通过各自关卡；H3 原实现和 Repair 1 已写且冻结，但 Repair 1 独立复审仍有 I1/I6/I7 Important 未闭合。I2/I3/I4/I5/I8/M1 原具体缺陷已闭合，I8 整体仍依赖 I6。H4 以后全部未实施。第二轮修复只准备了简报，还没有派发。

审计目录：
/home/xiaobai/.codex/worktrees/intel-hda-audio-plan/ReliefOS/.superpowers/sdd/2026-09-30-intel-hda-audio

立即读取完整 task-3-fix-1-re-review.md、task-3-fix-2-brief.md、task-3-brief.md、task-3-context.md、task-3-fix-1-brief.md 及 task-3-report.md 的 Repair 1 附录；核验 current source SHA/HEAD/status。task-3-fix-2-before.json 已保存，禁止覆盖。

Repair 2：
1. I1 修复 group 的早成员替代分配：P(seq0)→A/B、Q(seq2)→A 必须解出 P→B/Q→A；整条路径包含中间 widget 的实际 channel capacity，不能只 min(pin,DAC)。
2. I6 发生 power SET 后必须 fresh GET，拒绝新 PS_Error/不匹配 Set；不能按 SET 前 Act==target 返回成功。apply 和 restore 两方向覆盖，失败保留 last lease/dirty state/retry。
3. I7 全 route amp 无 mute 且 prior pin enabled 时，apply/restore 在 topology/gain 操作前真实关闭并读回 IN/OUT/HP pin gate；不确定写入保留恢复 metadata，保持原 pin/VREF/EAPD 和左右声道 rollback。
先真实生产函数语义 RED，再最小修复/GREEN。冻结后完成受影响 strict host/TU/nm/Doxygen/普通 Make/whitespace，保存完整真实命令/输出/exit，不覆盖旧失败日志、不 make -B、不重复无关 broad suites。然后由新的 Sol/high 定向复审，全部闭合才接受 H3。

若同会话可访问 /root/implement_h3_luna_xhigh，优先 followup 原实现者；若新会话无法访问，先确认无旧源码写入者，再使用完整保存简报创建一个 Luna/xhigh 接替，不假装旧 Agent 已被唤回。

后续严格按 H1–H5 → A1–A5 → A7(Task19) → A6(Task11) → H6 → U1–U6。H4 format/geometry extension、H5 volume persistence、H6 manager retention/MSI pending reuse/BDF ownership/multi-codec disconnect/真实 guest launch、A7 native IPC_64=0、Doom errno/64-bit phase 等详情均在交接文档及草案中；到相应任务前刷新实际接受接口，勿把草案当已实施。

目标验收为 QEMU Intel HDA，并面向常见真机扬声器/耳机/录音。必须区分源码、host fixture、freestanding TU、普通 kernel target、真实 hda.drv 链接/stage、guest 波形/录音、Doom600秒及 physical 证据。当前没有真实 HDA module/guest/Doom/真机验证，不要声称已可用。持续推进至原目标完成，中文报告有效进度，别在中间关卡结束或询问“是否继续”。
~~~
