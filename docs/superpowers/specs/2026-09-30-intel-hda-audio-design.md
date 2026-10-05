# Intel HD Audio、Linux 音频 ABI 与 Doom 音频设计草案

**状态：** 用户已于 2026-09-30 批准本设计并要求开始实施。本文描述目标能力，实施进度以计划和验证证据为准。已确认的验收环境是 QEMU Intel HDA，并以常见真机的扬声器、耳机和录音为目标。架构与本轮硬件边界已获批准。

**基线：** ReliefOS `bafaa59081f8c998037524fd5aea8865dd0468bd`，ReliefNT `f8f5a4908807e81cca7ed766fb6c60e7829767dc`。两仓调查时均无未提交修改。宿主 QEMU 为 11.1.1，已检查 `intel-hda`、`hda-duplex` 的参数和音频后端清单；没有启动目标系统验证音频。

## 1. 用户需求与边界

用户要求 Intel HD Audio 的广泛支持、Doom 音效和音乐稳定运行、严格遵循 Linux ABI 和 POSIX，并让系统程序控制音量、扬声器等设置。

本轮交付完整的传统 HDA 模拟音频路径：控制器枚举、多个 codec、播放与录音、硬件能力协商、DMA、中断、耳机检测、扬声器自动静音、音量、录音源、多卡选择和可恢复错误。支持通用 codec 拓扑；不能把一个 QEMU codec 的固定 NID 写成通用实现。

“几乎完全支持”按功能矩阵验收，不按驱动文件是否存在判断。真机支持状态必须记录 PCI ID、subsystem ID、codec ID、revision 和测试结果。当前没有指定真机型号，因此不得预先承诺某个 Realtek、Conexant、IDT 或 Cirrus 型号已通过。

传统 HDA 控制器连接的模拟 codec 是本轮强制目标。S/PDIF、5.1/7.1 只有在硬件报告并完成相应测试时才标记支持。HDMI/DP 的 ELD、音频包与 GPU 电源联动是独立扩展，不能仅凭枚举到数字 pin 宣称支持；本轮不将它作为交付完成条件。SOF DSP、SoundWire、I²S、USB Audio 和蓝牙需要各自的传输驱动，不能用 HDA 驱动替代。

Linux 音频兼容目标是 x86_64 LP64 的 ALSA PCM/control 和 OSS PCM/mixer ABI。POSIX 约束适用于 `open/read/write/close`、文件描述符、非阻塞、信号、等待和内存映射；POSIX 没有定义 ALSA/OSS 音频协议。完成此项目不能据此宣称整个操作系统已经获得 POSIX 认证或实现 Linux 的全部 syscall、ALSA sequencer、raw MIDI、压缩音频和硬件私有接口。

## 2. 源码调查

| 已检查的路径 | 当前行为 | 对实现的影响 |
| --- | --- | --- |
| `kernel/reliefnt/drivers/ac97/ac97.c`、`drivers/es1371/es1371.c` | 两个既有 PCM 驱动 | 保留旧驱动和旧模块 ABI；增加回归测试 |
| `kernel/reliefnt/include/reliefos/driver.h` | v1 音频 ops 只有 configure/write/state；kernel API 没有通用 PCI 中断注册 | 追加带尺寸的 v2 音频注册能力与平台服务，保留 v1 前缀 |
| `kernel/reliefnt/kernel/reliefnt/driver_manager.c` | 一个全局 `audio_ops` 指针 | 改成音频核心管理多卡，旧函数只转发默认 legacy 卡 |
| `kernel/reliefnt/kernel/reliefnt/syscall.c` | OSS 播放；录音返回 `EBADF`；`SYNC/RESET` 直接成功；空间来自固定常量 | 独立音频设备分派，以真实 stream 状态实现操作 |
| `kernel/reliefnt/kernel/reliefnt/syscall_mm.c` | 设备映射处理 framebuffer、共享内存等 | 增加 PCM data/status/control 映射与 VMA 引用 |
| `kernel/reliefnt/drivers/bootstrap/storage/storage_vfs.c` | `/dev/dsp`、`/dev/audio` | 增加 `/dev/snd` 和 `/dev/mixer`，联动 stat/getdents/权限 |
| `kernel/reliefnt/arch/x86_64/irq.c`、`idt.c` | 主要服务 PIT、键盘、鼠标；`0x30..0x4f` 共用一个 stub | PCI 中断必须携带真实向量，不能复用丢失向量的 stub |
| `mk/kernel.mk`、`mk/boot.mk`、子仓 `mk/boot.mk` | 明列五个 `.drv` | 三处及 install、staging 联动加入 `hda.drv` |
| `mk/run.mk` | 固定 AC97 | 增加 HDA 选择，保留 AC97/ES1371 回归选项 |
| `userland/apps/doom/reliefos_audio.c:142` | 非阻塞 OSS；固定 48 kHz/stereo/S16；自有音效与 MUS/MIDI 合成 | 分离传输、混音、音乐时钟，主路径使用上游 alsa-lib |
| 同文件 `flush_pending()`、`update_sound()` | libc `write()` 返回值与 `-EAGAIN` 比较 | 回归必须使用 `-1` 和 `errno=EAGAIN`，避免正常背压关闭声音 |
| `mk/upstream.mk`、`tools/build/upstream.sh` | 锁定 tarball，configure/Make，原子 stage | 同一机制构建 alsa-lib/alsa-utils；禁止生产阶段隐式下载 |

## 3. 方案选择

1. **推荐：通用 HDA + 原生 ALSA PCM/control + OSS 兼容 + 上游 alsa-lib。** 内核只负责硬件、PCM 状态、控件与标准设备接口；`plug/dmix/dsnoop` 在用户态处理格式转换和多客户端。Doom 和设置工具共用标准接口。
2. **只扩展 OSS。** 能较快让 Doom 出声，但无法满足正常 ALSA 程序、标准控件、共享播放和完整录音路径的目标，不作为最终方案。
3. **搬入 Linux ALSA/HDA 实现。** codec quirks 覆盖更广，但依赖 Linux 设备模型、DMA、工作队列、电源和同步框架，改动远超本项目边界。只将官方文档和 UAPI 用作行为参考，不把 GPL 内核实现复制进 Apache-2.0 内核。

## 4. 架构与职责

```text
Doom / 设置程序 / aplay / arecord / amixer
            | alsa-lib；legacy 程序走 OSS
            v
Linux /dev/snd PCM/control + /dev/dsp + /dev/mixer
            | 检查用户内存、权限、open-file-description
            v
ReliefNT audio_core / pcm / control / ALSA / OSS
            | 有引用的 card、stream、control；无用户地址
            v
hda.drv: controller / codec graph / stream / controls / jack
            | MMIO、受限 DMA、MSI；受控降级轮询
            v
PCI HDA 控制器 -> codec -> DAC/ADC -> 扬声器/耳机/麦克风
```

核心文件放在 `kernel/reliefnt/kernel/reliefnt/audio/`。驱动拆在 `kernel/reliefnt/drivers/hda/`。`syscall.c` 只保留设备分派钩子，避免继续添加几百行协议代码。公开用户接口采用 Linux UAPI；card ops、DMA 地址和 codec 节点是内核/模块内部接口，不放进 `<reliefos/audio_abi.h>`。

内部接口中，`audio_card_ops` 带 `version/size`，有 `pcm_caps/open/prepare/trigger/pointer/close` 和 `control_count/info/read/write`。PCM buffer 由核心分配、pin 并交给驱动；驱动收到内核 buffer 和 DMA 地址，绝不保存用户指针。period 和 control 事件只提交 card/stream ID 与计数，IRQ 不进入 syscall、不访问 WAD、不做日志格式化。

v1 `RELIEFOS_DRIVER_ABI_VERSION` 和现有四个音频 ops 的布局保持不变。扩展只能追加到 `reliefos_driver_kernel_api`，新模块先验证 `struct_size`。v2 注册是新回调，不通过延长无尺寸的 v1 `audio_ops` 猜测布局。音频卡持有模块引用；流/VMA/回调存在时普通卸载返回 `EBUSY`；强制卸载先断开、停止 DMA、取消并同步回调，再释放模块。

## 5. 控制器、codec 和 PCM

发现按 PCI class `0x04`、subclass `0x03`，结合配置和能力排除无法直接使用的 DSP 模式。枚举所有 BDF，不能只 `pci_find()` 一个 ID。BAR 支持 32/64 位，验证地址、长度和映射属性；DMA 分配遵守 `GCAP.64OK`，32 位 DMA 不允许截断高地址。CORB/RIRB/BDL 按规范对齐并用显式内存屏障。

初始化按有截止时间的状态机执行：关闭 DMA/中断 -> CRST reset -> 等待 codec presence -> 建 CORB/RIRB -> 读取 root/AFG/widgets -> 建路由 -> 注册 card。所有等待有截止时间和错误码；无 codec 返回 `ENODEV`；超时返回 `ETIMEDOUT`；证明 DMA 已静默后撤销资源，若无法证明静默则保留 leases 和模块 owner 供销毁重试，禁止释放可能仍被 DMA 使用的页。H6 必须验证 manager 的卸载与初始化回滚确实保留这些资源。命令按 codec address 配对响应；unsolicited 响应独立解码；RIRB overrun 不能将错误响应交给下一个命令。

codec parser 读取 connection list 的短/长格式与 range 编码，检查 NID 边界和循环。能力从 AFG 继承或 widget 覆盖；依据 pin default association/sequence、pin caps 和连接图寻找 DAC->输出、输入->ADC 的路径。输出设置 selector/mixer、amp、pin control、D0、EAPD；录音设置 input pin、VREF、ADC 和输入增益。quirks 以 PCI subsystem/codec/revision 精确匹配，只添加有证据的修正。

采样格式、频率、声道取 controller、converter 和路由能力的交集。必验 48 kHz、stereo、S16_LE 播放；录音使用 QEMU codec 实际报告的格式。44.1 kHz 和其他格式只在真实能力允许时启用；8/20/24/32 bit、单声道和多声道也遵守相同规则。用户态 `plug` 转换不等于硬件支持。

DMA 使用循环 BDL、period completion 与单调 hw/appl frame 计数。position buffer 异常可切到 LPIB，记录采用的方法；处理环绕、过早中断、FIFO/descriptor 错误。playback underrun/capture overrun 进入 XRUN 并停止流，`PREPARE` 恢复，不能无限重放旧数据。`DRAIN` 等最后一个 frame 完成，`DROP` 立即丢弃，pause 保留位置。静止状态不忙等。

默认 48 kHz/stereo/S16 的交互播放采用 512-frame period、4-period buffer；允许硬件协商更小/更大值，目标队列延迟 20–60 ms。单次 syscall 拷贝最多 4096 字节，调度前释放执行锁；不能在中断关闭时等待一整段音频。MSI 为主要路径；没有安全 IRQ 路由的机器可采用与现有 100 Hz tick 一致的 10 ms 有界服务轮询，标明较大的可协商 period 和该降级模式，不能伪装为 INTx 支持或使用未知 GSI。初始化和单条 codec 命令的短时硬件等待不属于持续 PCM 服务轮询。

## 6. Linux ABI 与文件语义

Linux UAPI 基线固定为 **Linux v6.14** 的 `sound/asound.h`、`sound/tlv.h` 和 `linux/soundcard.h`，使用 Linux headers_install 的导出语义，保留 SPDX/来源。x86_64 结构中的 `long`、指针、timespec 与 ioctl size 保持原布局，不能为“定宽”重写这些 Linux 类型。参考库固定 **alsa-lib 1.2.14、alsa-utils 1.2.14**，以可复现性为选择依据，不宣称是最新版。

设备提供 `/dev/snd/controlC<N>`、`pcmC<N>D<M>p`、`pcmC<N>D<M>c`。`/proc/asound/cards`、`devices`、`pcm`、`version` 和 codec 诊断与真实卡注册表一致。字符设备 major/minor 按 Linux 分配，权限通过现有 DAC/ACL 处理；录音节点权限单独核验。无硬件时不生成有效 PCM 节点。

PCM 状态为 OPEN、SETUP、PREPARED、RUNNING、XRUN、DRAINING、PAUSED、SUSPENDED、DISCONNECTED，保留 Linux 枚举数值。实现 PVERSION/USER_PVERSION/INFO/TSTAMP/TTSTAMP、HW_REFINE/HW_PARAMS/HW_FREE、SW_PARAMS、PREPARE/RESET/START/DROP/DRAIN/PAUSE、STATUS/STATUS_EXT/DELAY/HWSYNC/SYNC_PTR、CHANNEL_INFO、WRITEI/READI、REWIND/FORWARD、LINK/UNLINK/XRUN。PVERSION 为 `0x020012`，control version 为 `0x020009`；USER_PVERSION 保存在每个 PCM 文件对象，OLD/NEW timestamp 协商遵守该版本。没有电源恢复能力时不宣称 RESUME；非交错访问不在本轮能力集合中，拒绝对应协商。未知 ioctl 为 `ENOTTY`。

`HW_REFINE` 必须传播 mask、interval 和 period/buffer 约束直至固定点；不能只把 `HW_PARAMS` 当作配置结构。Linux PCM data/status/control mmap offset、保护属性、boundary 和 SYNC_PTR flags 都按参考头和上游库实测。映射通过 VMA 引用 pin 页面；close/fork/munmap/mprotect/HW_FREE/重配置/卸载都不能让 DMA 页被复用给其他进程。Linux v6.14 的 `HW_FREE` 只允许 SETUP/PREPARED，存在 data mmap 时返回 `EBADFD`；非 OSS 的 `HW_PARAMS` 在存在 data mmap 时也返回 `EBADFD`。不得用失效页面或方便的内部行为替代这个合同。断开后的 fd/VMA 继续持有页面引用并停止 DMA，最后引用释放才回收。读写用户可修改的 control 页时取快照、校验并使用内存屏障。

独立 open 使用独立描述对象；dup/fork/SCM_RIGHTS 共享同一个 open-file-description，最后一个 fd/VMA 引用释放才关闭资源。`O_NONBLOCK`、访问模式、FD_CLOEXEC 按现有 Linux 机制共享或独立。独占 hw PCM 再次打开返回 `EBUSY`；多应用由 alsa-lib dmix/dsnoop 共享，内核不隐式混合两个独占流。

read/write 可短传输；没有传输且非阻塞为 `EAGAIN`，信号前无进度为 `EINTR`，已有进度返回数量。设备无 seek 为 `ESPIPE`。所有用户数据先验证/复制，ioctl copy-out 使用 `user_range_writable()`，只读页、COW、跨页、溢出与非法 nested pointer 都有回归。PCM write/read 单位为字节，WRITEI/READI 为 frame，混用单位必须被测试发现。

control 实现卡/PCM 枚举、element list/info/read/write/lock/unlock、订阅事件、TLV dB 描述。控件锁随 open-file-description 的引用生命周期释放。控件来自实际拓扑：Master/PCM/Headphone/Speaker 的音量与开关、Capture 音量/开关/Input Source、Output Source、Auto-Mute、jack state；没有对应硬件或可执行软件策略时不创建伪控件。注册后控件集固定，每个 control fd 的待处理事件按 numid 合并，容量覆盖全部控件，不发明私有“刷新”事件。枚举顺序、numid 和事件稳定。OSS PCM/mixer 映射到相同核心；GETOSPACE/GETISPACE/GETODELAY/GETxPTR 读取真实容量与计数，SYNC/RESET/trigger/fragment negotiation 真正生效。

## 7. 程序、Doom 与设置

构建上游 alsa-lib 与 alsa-utils，stage `libasound.so.2`、公共头、插件、配置、`aplay/arecord/amixer/alsactl/speaker-test` 和许可证。SDK 只消费导出 UAPI 和该库的安装结果。默认 pcm 经 `plug -> dmix`；capture 经 `plug -> dsnoop`。配置使用独立会话/UID 的 IPC key，并测试共享内存、SysV semaphore、pthread/futex、timer 使用；不能靠关闭这些插件来掩盖 Linux ABI 缺口。

Doom 主输出改为 alsa-lib nonblocking PCM，保留 OSS fallback。音效的空间分离、音量、pitch、缓存保留；按实际协商 rate 重采样。传输维护有上限的待提交队列，内部用字节保留任意 OSS 短写尾部，ALSA 边界换算 frame，正确处理 EAGAIN/EINTR、`snd_pcm_recover()` 和无声卡模式；不在 GUI thread 中 drain。MUS/MIDI 事件按音频 frame 时间推进，tempo、running status、pitch bend、channel volume、sustain、percussion、loop/pause/stop 有确定性测试。正常 WAD 使用 GENMIDI 乐器和固定提交的纯 C Nuked OPL3 1.8 动态库；缺少有效 GENMIDI 时可使用明确注明音色限制的旧合成器。该方案不依赖内核 MIDI/声卡 FM 硬件，也不复制用户商业 WAD 的资源。

提供 `soundctl cards|controls|get|set|route|capture-source|watch|test` 和设置程序声音页。两者调用同一个基于 alsa-lib 的用户态 control helper，不增加私有声音 syscall。GUI 可选择卡、输出、输入、音量、静音与扬声器测试；使用标准 control events 更新外部 amixer 改动和耳机插拔。耗时 PCM 工作放到 worker；主题、键盘、窄窗口和无设备状态遵守现有 UI 规则。音量/路由状态可用 `alsactl` 存储到 `/var/lib/alsa/asound.state`，OpenRC 服务启动恢复；默认 PCM 选择是用户态配置，不等同于硬件 mixer。

## 8. 分项目与依赖

| 项目 | 可单独验收的产物 | 前置条件 | 计划 |
| --- | --- | --- | --- |
| H：HDA 与硬件核心 | 模块加载、真实 DMA 播放/录音、codec 路由、控件和生命周期 | 已有内核 PCI/MM/调度 | `../plans/2026-09-30-intel-hda-hardware.md` |
| A：Linux 音频 ABI | 标准设备、PCM/control/mmap、OSS 和 ABI 回归 | H 的内部 card/PCM 合同；可先用 fake card | `../plans/2026-09-30-linux-audio-abi.md` |
| U：库与程序 | 上游工具、Doom、soundctl、设置页、共享音频、端到端验收 | H + A | `../plans/2026-09-30-audio-userland-doom.md` |

实施顺序：H1–H5 -> A1–A5 -> A7（SysV shared memory）-> A6 -> H6 -> U1–U6；A7 是上游 dmix/dsnoop 的必要依赖，任务编号保持既有安排。H 的假设备与 A 的假卡测试不要求先启动 GUI。所有数字能力扩展复用此合同，另设真机验收项目。

## 9. 验收门槛

1. Host：实际生产函数在 fake PCI/MMIO/codec/DMA/card fixture 下运行，ASan/UBSan；涵盖超时、malformed 图、环绕、XRUN、并发、指针和卸载。ABI 常量/布局与固定 Linux 参考比较。
2. 构建：顶层 Make 的 kernel/drivers、headers、runtime、Doom、settings、SDK、rootfs、VMDK、live ISO、installer ISO；干净 checkout、增量 rebuild、删去 staged 成员后的恢复。生产链保持 C/汇编 + Make/Shell。
3. QEMU：q35 与 pc，1/4 CPU，`intel-hda` 与 `ich9-intel-hda`、hda-output/duplex、多 codec、无声卡、MSI on/off。WAV 后端验证输出样本，实际音频后端验证录音；`none`/WAV 不能证明麦克风录音正确。
4. 波形：1 kHz tone 的频率误差 <= 1%，有效输出时长误差 <= 2%，左右声道独立；静音输出 RMS <= 1 个 S16 LSB；半幅与全幅输出幅值变化有可重复结果。测量跳过头尾启动静音，保存采样参数和窗口。
5. Doom：Freedoom 菜单音乐、关卡音乐与音效同时运行 10 分钟；60 秒人为 CPU/I/O 负载期间 GUI 可响应；音量/静音/输出切换无需重启游戏；故意欠载后可恢复；正常负载下 XRUN 为 0。保存波形、日志、耗时和计数，不能只检查“first nonzero”日志。
6. 真机：分别验收扬声器/耳机/麦克风、热插拔、自动静音、独立增益、持续播放录音、驱动重新加载；至少两种不同 codec 的测试记录才称“常见真机已验证”。未提供硬件时明确记录未验收，不把 fixture 当作真机。

完成报告分别给出源码、host、编译、staging、QEMU 与真机证据。任何强制项目失败都不能宣布目标全部完成。

## 10. 工作约束与审查记录

- 主仓 `AGENT.md` 的无擅自提交/push 规则优先于技能的频繁 commit 建议。计划给出可选提交命令，只有得到用户授权时执行。
- 内核每个新函数声明/定义配 Doxygen 注释，标清锁、IRQ、DMA 和引用所有权。
- 写入原子 stage；新增依赖只有显式 `make fetch` 下载，身份与 SHA-256 进入 lock；不修改现有用户镜像或结束其 VM。
- 实施前重新检查两仓状态与 gitlink。子仓改动先在 ReliefNT 保存，主仓只更新已批准的 gitlink，不打包未提交源码冒充发布版。
- [x] 已检查项目和调用链。
- [x] 已获得首要验收环境。
- [x] 已比较方案并形成可审查草案。
- [x] 用户批准架构与边界。
- [ ] 批准后执行各项目的红/绿测试循环和审查。
- [ ] 达到验收门槛后记录完成证据。

## 11. 一手参考

- [Intel High Definition Audio Specification Rev. 1.0a](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf)：控制器和 codec 协议的基准。
- [Linux HDA 官方说明](https://docs.kernel.org/sound/hd-audio/notes.html)：position、codec 配置与中断差异的参考；不能由其推断 ReliefOS 已支持对应硬件。
- [Linux v6.14 asound UAPI](https://github.com/torvalds/linux/blob/v6.14/include/uapi/sound/asound.h)、[TLV](https://github.com/torvalds/linux/blob/v6.14/include/uapi/sound/tlv.h)：ABI 布局与编号。
- [ALSA PCM 文档](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html)、[alsa-lib 1.2.14 hw PCM](https://github.com/alsa-project/alsa-lib/blob/v1.2.14/src/pcm/pcm_hw.c)：协议调用和库兼容验证。
- [Linux v6.14 control 行为](https://github.com/torvalds/linux/blob/v6.14/sound/core/control.c)：控件锁、owner、权限和事件行为。
- [Linux v6.14 PCM 状态与 mmap 行为](https://github.com/torvalds/linux/blob/v6.14/sound/core/pcm_native.c)：只作 ABI 行为核验，不能复制内核实现；已核查 mmap_count 与版本协商路径。
- [Nuked OPL3 固定提交](https://github.com/nukeykt/Nuked-OPL3/tree/765ec962e473aeb767e4cba74ffdc8f588ffbfe8)、[Chocolate Doom GENMIDI 参考](https://github.com/chocolate-doom/chocolate-doom/blob/chocolate-doom-3.1.0/src/i_oplmusic.c)：用户态音源和音色格式的来源。
- [POSIX write](https://pubs.opengroup.org/onlinepubs/9699919799/functions/write.html)、[read](https://pubs.opengroup.org/onlinepubs/9699919799/functions/read.html)、[poll](https://pubs.opengroup.org/onlinepubs/9699919799/functions/poll.html)：文件操作语义；本次网页读取受站点限制，执行时结合本地参考测试核验，不标为已完成标准审核。
- [QEMU 官方设备用法](https://github.com/qemu/qemu/blob/v9.2.0/docs/qdev-device-use.txt)：HDA controller/codec 设备关系；运行参数还已以本地 11.1.1 `-device ...,help` 核查。
