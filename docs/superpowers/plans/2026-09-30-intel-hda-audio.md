# Intel HDA 音频项目计划索引

**状态：** 用户已于 2026-09-30 批准；正在隔离 worktree 中实施。

[完整设计草案](../specs/2026-09-30-intel-hda-audio-design.md) 包含现有源码调查、架构选择、Linux/POSIX 边界和验收范围。按职责拆成三个可独立测试的实现项目：

1. [HDA 硬件与音频核心：H1–H6](2026-09-30-intel-hda-hardware.md)。
2. [Linux ALSA/OSS ABI：A1–A7](2026-09-30-linux-audio-abi.md)。
3. [音频库、声音控制与 Doom：U1–U6](2026-09-30-audio-userland-doom.md)。

执行顺序为 H1–H5 -> A1–A5 -> A7（SysV SHM 必要前置）-> A6 -> H6 -> U1–U6。U1 的库构建和 U4/U5 的 host 测试可在其前置接口固定后独立进行。

**当前进度（2026-10-01，H3 已验收）：** H1 资源与生命周期、H2 控制器/命令传输及 H3 codec 拓扑/路由已通过各自 host 测试、相应编译和审查。H3 Repair 2/3 已闭合 I1/I6/I7；H4 合同刷新已完成，stream/DMA 实现尚未开始。详细证据与限制见 [进度交接文档](../progress/2026-10-01-intel-hda-audio-handoff.md) 和 `.superpowers/sdd/2026-09-30-intel-hda-audio/task-3-fix-3-re-review.md`。H4 以后未实施，HDA 生产模块接入、QEMU 音频、Doom 和真机仍待验证。

**已确认目标：** QEMU Intel HDA，并以常见真机的扬声器、耳机和录音为目标。模拟音频、标准 PCM/control、程序控制、共享播放、Doom 音效与 OPL 音乐是本轮功能。HDMI/DP/SOF/SoundWire 需要独立扩展和硬件验证，不将模拟codec测试当作这些能力的证明。

**实施检查点：**

- [x] 用户批准架构、ABI 和硬件边界。
- [ ] H1–H5 的资源/transport/codec/stream/control host 门槛。
- [ ] A1–A7 的 UAPI、PCM、fd、mmap、control/OSS、SysV SHM 门槛。
- [ ] H6 的模块产物、QEMU tone 与录音门槛。
- [ ] U1–U5 的库、SDK、CLI/GUI、Doom 输出/音源门槛。
- [ ] U6 的上游软件与 10 分钟游戏验收。
- [ ] 两种真机codec的证据，或明确的未验收记录。

当前采用 subagent-driven-development，串行实现，每任务做规格与质量审查。用户指定实现与修复代理为 GPT-6-Luna、xhigh；审查使用 GPT-6.1-Sol。任何提交/push/PR 均遵守主仓 `AGENT.md` 的授权规则。

## 规划覆盖检查

| 规格要求 | 实现任务与验证门槛 |
| --- | --- |
| 多卡、旧模块兼容、DMA/IRQ 所有权与卸载 | H1、H4；资源与生命周期 host fixture，H6 模块回归 |
| HDA 命令、通用 codec 路由、播放/录音、耳机与音量 | H2–H5；codec/stream/control fixture，H6/U6 真实 I/O |
| Linux UAPI、POSIX 文件行为、PCM 状态、参数与映射 | A1–A4；Linux 固定参考、fd/VMA/用户指针回归，A6/U6 标准程序 |
| ALSA control、锁、事件、TLV、OSS mixer/queue | A5；双协议互操作与错误参数，A6/U6 amixer/alsactl |
| 上游库、共享播放/录音、SDK 与可复现依赖 | A7、U1；原生 SysV SHM、锁定下载、原子 stage、独立 SDK，U6 dmix/dsnoop |
| 程序音量/路由/录音源、声音设置页与持久化 | U2–U3；helper/页面/服务 fixture，U6 GUI 与外部控制事件 |
| Doom 短写入、混音、重采样、音乐时序与 OPL | U4–U5；真实生产函数 host 回归，U6 波形与 600 秒游戏 |
| 构建产物、QEMU 矩阵、真机、证据分级 | H6、U6；未运行项目明确保留为未验收 |

这是规格到任务的覆盖检查，不是功能测试结果。规划文件检查包括相对链接、代码块闭合、原 18 个任务的步骤结构；实施中补入的 A7 独立记录前置和验证、空白错误和占位符扫描；已验收任务按证据逐步标记，其余实施检查框保留未完成状态。

2026-10-01 依赖补充：已核验上游dmix/dsnoop调用原生SysV SHM，而当前内核只提供private共享内存设备。A7补齐已授权目标的必要接口，在ABI阶段先完成；此处为实施裁决，原批准日期保持不变。
