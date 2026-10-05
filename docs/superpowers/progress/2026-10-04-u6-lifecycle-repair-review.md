# U6 生命周期与 stream position 修复：内联审查

工作树：`/home/xiaobai/Projects/Projects/ReliefOS-intel-hda-audio-plan`。按用户要求全部内联完成；本记录不是独立代理审查。未提交、推送、归档、reset 或清理既有源码/证据。旧审计与失败日志保持原样。

## 根因与最小修复

1. 原 raw-null 的“首次 EIO”来自诊断探针，不能当成 PCM read 失败。冷启动和标准 arecord 后均读到 2048 帧，但 `/dev/null` 的 8192 字节 write 合法返回 4096；旧探针将短写自行变成 `-EIO`，尚未累计帧数。新 QA 探针重试正短写/EINTR，零写或真实错误仍失败。内核 null 语义未修改。
2. PCM DROP 后，旧 core period notification 尚可留存；PREPARE 清零指针后再导入它会污染新 epoch。IRQ 已将状态置为 XRUN 时，task-side event 不再进入原 RUNNING stop 分支。`audio_pcm_prepare()` 现在先确认 XRUN 的 STOP 成功，再消费旧通知，最后清空/准备 DMA。STOP 失败保留旧状态、指针与 DMA；断连仍返回 ENODEV。
3. HDA 诊断的一次 position GAP：WALLCLK 差 `0x3be71`（约 10.2 ms），系统 tick 差 9（名义 90 ms），DMA position 2712 字节。原粗 tick 下界错误地按小 ring 时长拒绝进度。保留真实 WALLCLK 整圈检测，只将粗 tick 用于整个 32 位 WALLCLK 的回绕歧义（约 179 秒）。硬件 SDSTS/FIFO/descriptor 错误及真实整圈间隔仍失败。
4. 已知信号的默认 capture 将左声道复制到右声道。`CHANNEL_INFO` 历史编码 `_IOR`，但调用者的 channel 是输入选择器。实际设备 usercopy 原先未读入它；两次查询都落在 channel 0。只为此 ioctl 加显式 copy-in；channel 1 的 first=16/step=32 和非法 channel=2 经实际适配层测试。行为与 Linux v6.14 `snd_pcm_channel_info_user()` 的双向复制一致。

原诊断源码、RED 日志、修复前文件及所有新探针保留于 `out/audio-hda-u6/qa/u6-lifecycle-20261004/`。临时 `[hda-life]`/`[pcm-life]` 已从生产源码移除。

5. HWSYNC 原来只消费 IRQ 通知，未调用驱动 pointer。硬件前进 240 帧、没有 period IRQ 时，返回位置仍为 0；这是独立的 Linux 合同缺口。现在 RUNNING/播放 DRAINING 查询 pinned driver pointer（不跨 PCM lock），并在 commit 前重查断连/XRUN；PREPARED/PAUSED 不查询。IRQ/event 更新比较同一 PREPARE epoch 的累计 driver frames，再按 boundary 取模，避免较旧 IRQ 回退已由 HWSYNC 观察的位置，也保留非二次幂 boundary 跨越。驱动 pointer 错误转 XRUN/STOP，失败保留 DMA，fatal disconnect 不复活。`hwsync-position-red/green.log` 与 `strict-host-r8.log` 覆盖真实 core/PCM、SYNC_PTR、旧 IRQ、pointer 故障和断连。第一版新增故障 fixture 错调同步 unregister 导致 host 超时，已改用真实 IRQ-safe disconnect request；`strict-host-r6/r7.log` 是该 fixture 编译/超时证据，不是生产修复 RED。

## RED/GREEN 与边界

- `sink-red.log` / `sink-green.log`：实际 sink helper 的短写/EINTR/零写/错误。
- `pcm-reprepare-red.log` / `pcm-reprepare-green.log`：实际 core/PCM 的 stale period 与 IRQ XRUN；`strict-host-r5.log` 还覆盖 STOP 失败、DMA 保留与 fatal disconnect。
- `wallclk-drift-red.log` / `wallclk-drift-green.log` / `hda-strict-r2.log`：9 ticks 对 10 ms 控制器进度、真实整圈、长时 alias、wrap 和 pause/resume。
- `channel-adapter-red.log` / `channel-adapter-green.log`：实际设备 ioctl usercopy，不只测试绕过 usercopy 的 PCM API。
- `strict-host-r5.log`：PCM/device/params/ioctl/mmap，ASan/UBSan + Wall/Wextra/Werror；仅关闭既有 fixture 的 unused-function。
- `hda-related-r3.log`、`controls-oss-mixer-r3.log`、`vma-green-r3.log`、`userland-green.log`、`descriptors-green.log`：对应 sanitizer 回归。stream strict 单独通过。
- 扩展 strict 的 `strict-host-r3/r4.log`、`strict-hda-r3.log` 保留失败：既有 paging fixture main 缺 return、control fixture 格式警告、hda/module/lifetime 缩进与 fixture 警告；未为此改写无关代码，也不把扩展 strict 失败说成通过。
- `stop-reprepare-red.log` 在原 HDA module 已通过，只是回归补强，**不是有效 RED**。若干编译/参数调用失败日志也是基础设施失败，不计入功能 RED。
- ds55/56 的 lifecycle 返回成功并不足以接受：录音 stereo continuity 均失败（通道选择缺陷）；ds55 另有输入 EOF。短输入被扩大至至少 180 秒余量，长 Doom 使用 780 秒已知输入；原失败结果不覆盖。

## 几何证据

生产默认在本轮验证期间仍为 768/12288。原候选 2048/12288、4096/12288 的后续 raw-file 或同句柄 DROP→PREPARE raw-file 有真实 EPIPE，第二次文件成功不能代替生命周期门禁。

ds54 的失败 status 显示 dsnoop avail=14295 > buffer=12288，slave 仍在推进，真实客户端积压成立。小块文件写入约 23–75 ms，高于 768 帧的 16 ms；不是用忽略 XRUN 或丢弃录音样本修复。临时候选 4096/32768 使用 128 KiB、8 个 HDA period，已由真实 module geometry host fixture 验证；接受仍须双声道样本、重复 close/reopen、同句柄 DROP/PREPARE、upstream fatal-errors、60 秒录音、完整标准矩阵与 600 秒 Doom/压力全通过。

## 审查检查点

检查了所有本轮 production diff 与锁/生命周期边界：不跨 PCM IRQ-safe lock 调用 module，不在 STOP 失败后清 DMA，不复活 detached generation；position 算术保留边界/对齐与 WALLCLK 全圈错误；CHANNEL_INFO user range 的读写检查均保留。未新增 core/syscall/descriptor 或全局调度策略改动。定向 whitespace check 通过；整个 kernel diff 的既有 UAPI whitespace 保留。

## 验证工具边界修正

`tools/test_hda_qemu.py` 将已知输入预算从 Doom+60 增至 Doom+180 秒，避免重复生命周期与 60 秒流式录音耗尽输入。实际 ds55 的输入 EOF 和 ds57 的完整连续样本构成验证证据。

`tools/test_audio_e2e_qemu.py` 原先在所有软件 gate 已通过时，仍因 GUI/physical 两个 false 返回 1（ds57 的原结果保留）。按最新用户要求，退出码现在只由真实软件 gate 及整个 serial XRUN-clean 决定，GUI=deferred、physical=not_run 单独保留。`software-report-red/green.log` 测试外部 gate 未执行时的软件成功、任一软件失败、全串口 XRUN 与缺失证据，不会放松实际音频 gate。

## 最终客体结果

ds57（r3 / q35 / Intel / 4 CPU / MSI-on）的软件长测成功：600 秒 Doom 写入 164,552,756 字节，startup/XRUN/suspend 恢复均 0；60 秒 CPU/I/O 完整重叠，I/O 10,190,848 字节/38 圈；整个 serial XRUN-clean。完整标准 suite failures=0；60 秒流式文件 2,883,584 帧/11,534,336 字节，左右 400/800 Hz 全部连续（样本容差 2）。旧 wrapper exit=1 原因是 GUI/physical 外部门禁，原结果保留。

但 r3 的完整矩阵证伪了 4096/32768 的接受：ds58 Intel MSI-off upstream wav-file/长文件 overrun（长文件仅 393,216 帧），ds60 ICH9 MSI-off 的核心播放尾部连续性失败，ds61 pc/1CPU/MSI-on 有 underrun 和录音 overrun，ds63 dual 有 upstream wav-file overrun。ds59 ICH9-on、ds62 pc/1CPU/off 和 output-only/no-audio 成功。全部 run.json/result.json 与连续采样检查保留；不能由一次 600 秒主组合成功替代矩阵。

错误路径专用诊断 ds67 在 Intel MSI-off 再次复现 upstream wav-file overrun，**没有** HDA GAP/SDSTS 或 PCM error notifier；同轮 60 秒文件已知 stereo 连续完整。证据定位在客户端积压，不以关闭真实 XRUN 检测作修复。r4 增加 HWSYNC 修复后正在同几何复测，生产默认仍 768/12288；当前 U6 和候选均未接受。

GUI 按用户要求延后到 Xorg；物理 codec 的 ReliefOS 原生启动/独占 PCI 透传 **未执行**，不得用 host/QEMU 替代。

参考：[Intel HDA spec](https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf)、[QEMU HDA 上游实现](https://raw.githubusercontent.com/qemu/qemu/master/hw/audio/intel-hda.c)、[Linux v6.14 PCM ioctl](https://raw.githubusercontent.com/torvalds/linux/v6.14/sound/core/pcm_native.c)。QEMU master 仅为时钟模型参考，当前安装版本运行证据见每个 run.json。


## r4 复测与新的原型（尚未接受）

r4 镜像 `bfbcc9d427caab79e6f3e622de5de2c5179b670e2874b86897e2448417aea999` 含 HWSYNC 修复，默认仍 768/12288。ds68 Intel/4CPU/MSI-off 完整 4096/32768 suite 和 60 秒 stereo 连续采样通过；但 ds69 有原生 512/2048 播放 EPIPE 与 wav-file overrun，ds71 ICH9-off 和 ds74 dual 仍有 wav-file overrun，ds73 pc/off 仍有 waveform EOF 尾部断裂。因此 4096/32768 **未接受**。ds70 ICH9-on、ds72 pc/on、output-only/no-audio 的相应 gate 通过；详见 `r4-matrix-results.json`，全串口复检独立于命令退出码。

8192/65536 原型尚在验证：文件周期 32 KiB，匹配既有 regular-file syscall 的单块上限，周期约 170.7 ms，buffer 256 KiB/8 个 period。真实 module host fixture 验证可选几何；没有修改 regular-file syscall、调度器或 IRQ 策略。还增加 generic default arecord（不指定 period/buffer）的 raw/wav null/file。

新独立 native 波形探针使用 512/8192，并在 tone 后显式提交 8192 帧静音。原失败显示新 tone 在上次 tone 的尾部提前开始，与 QEMU codec STOP/reopen 重置 frontend 队列的实现相符（推断，非物理 codec 证明）。显式尾部用于把完整已知 tone 交给 backend；checker 对全部 96000 帧的连续性、频率、幅度和时长要求完全不变，提交 tone 与静音帧数分别记录。**此探针改进不宣称修复生产 DRAIN 的 DAC EOF 边界**，原 EOF/小 buffer 失败仍单独保留。源码、编译参数与 SHA 见 `candidate8192-flush-manifest-r2.json`。


## r5 的累计位置回归与验收条件

初版 HWSYNC 的 boundary 内半圈比较拒绝了 OSS fixture 的近 boundary 累计帧数，并在新增 HWSYNC 跨界 fixture 中实际失败。`relevant-hwsync-green.log` 和 `hwsync-boundary-red.log` 是有效失败证据。现使用内部 `driver_frames` 高水位，PREPARE/HW_PARAMS/HW_FREE 随 PCM epoch 清零；只从不小于高水位的累计帧数发布 `hw_ptr = frames % boundary`。较旧 IRQ 不回退，真实累计跨界仍推进。`strict-host-r9.log` 与 `relevant-hwsync-boundary-green.log` 通过全部相关 host 回归，含 OSS 非二次幂边界。未改 core/descriptor/syscall 的契约。

r5 镜像 SHA256：`6eecf87ec056307d635efe17524b005601bc48d11aab76921e4111111413cf73`。生产 asound.conf 暂置 8192/65536 供精确最终镜像验证；在完整矩阵和长测完成前保持待验收。与 r3/r4 不同，标准脚本不在 guest 替换配置，直接核对镜像中两处默认几何。

当前 native 波形专用 probe 是 512/8192 + 8192 帧显式静音尾部，原 canonical probe 仍为 512/2048，源码未改。标准 suite 仍包括真实 `aplay hw:0,0` 512/2048。此组合的完整 tone checker 不放宽，但不作为原始无静音尾部的 EOF/DAC drain 问题已修复的证明。

独立计数 gate 要求 10 个 streaming END：冷开 null、标准 arecord 后交替 null/file 四次、两种 sink 各两次同句柄 DROP→PREPARE、最后 60 秒文件。短项均必须 result=0、147456 帧/18 写；长项必须 result=0、2883584 帧/352 写。再检查四个 generic default raw/wav null/file、全部已知输入样本、core/standard 零失败、整个 serial 无 XRUN，Doom 的 startup/XRUN/suspend 恢复计数全为 0。仅返回 0、fstat 非空、或第二次文件成功均不充分。

已知输入周期为 120 帧；全样本连续匹配不能观察恰好整数个 120 帧的输入跳跃，按信号的实际分辨能力记录。普通 arecord 不指定 geometry 时，client 为 8192/24576，底层 dsnoop 为 8192/65536。硬件 buffer 的 1.365 秒与 170.7 ms 周期是当前候选明确的延迟代价，不宣称最低延迟或实时保证。


## r5 完整矩阵结果

`r5-matrix-results.json` 和 `final-matrix-lifecycle-counts.log` 已通过：

| 输出 | 组合 | 结果 |
| --- | --- | --- |
| ds78-r5-intel4on | q35 / Intel / 4CPU / MSI-on | core、完整标准 suite、60秒连续 stereo、全串口 XRUN-clean |
| ds79-r5-intel4off | q35 / Intel / 4CPU / MSI-off | 同上 |
| ds80-r5-ich9on | q35 / ICH9 / 4CPU / MSI-on | 同上 |
| ds81-r5-ich9off | q35 / ICH9 / 4CPU / MSI-off | 同上 |
| ds82-r5-pc1on | pc / Intel / 1CPU / MSI-on | 同上 |
| ds83-r5-pc1off | pc / Intel / 1CPU / MSI-off | 同上 |
| ds84-r5-dual | q35 / dual codec / native card1、standard card0 | 同上 |
| ds85-r5-output | q35 / output-only | core、waveform、controls/lifetime/SHM；无 capture 证明 |
| ds86-r5-none | 无音频设备 / boot-only | 正常启动、模块/卡状态正确；无 I/O 证明 |

七个双工组合各含 10 段计数精确的 streaming capture，Doom startup/XRUN/suspend 恢复均为 0。60秒文件均为 2,883,584 帧/11,534,336 字节，已检查全部左右声道样本，容差 2；每轮独立 phase 与 SHA 位于对应输出的 `long-capture-verification.json`。上述 waveform 是明确静音尾部的已知 tone 试验，保留前述原始 EOF 限制。该矩阵检查点时，`ds87-r5-long600` 尚在进行最终 600秒 Doom 与 60秒 CPU/I/O 压力，当时未据矩阵单独接受候选；最终长测结果与裁决见下一节。


## r5 长测与本轮最终裁决

最终长测 `out/ds87-r5-long600` 的 wrapper 与两个独立数据/计数验证退出0：600秒Doom写入164,659,280字节，startup/XRUN/suspend恢复均0；60秒CPU/I/O完整重叠，I/O完成7,839,744字节/29轮。整个serial XRUN-clean，core/standard均failures=0；最后60秒stereo为2883584帧/11534336字节，全部已知样本连续匹配。GUI和physical没有参与软件退出码，分别记录deferred与not_run。

默认捕获生命周期的软件范围已验证通过，并接受 dmix/dsnoop 8192/65536。host 定向 strict、9组合QEMU矩阵、重复 null/file 生命周期、known waveform/stereo、600秒Doom与60秒压力均通过。U6 的原始小 buffer/无尾静音 EOF 问题未闭合，物理 gate 未执行，不能宣称整体完成。

生产 dmix/dsnoop 均为 `period_size=8192`、`buffer_size=65536`：32KiB周期、256KiB buffer、8个HDA period。周期约170.7ms、buffer约1.365s；稳定性与延迟代价明确记录，不宣称最低延迟或实时保证。普通 arecord 不指定参数时，client 协商8192/24576，底层dsnoop为8192/65536。

最终审计为 `2026-10-04-u6-lifecycle-audit.json`。最终源码与镜像的精确SHA、所有RED/GREEN及失败候选均保留；原始产品镜像和两个旧audit不覆盖。全程无子代理和Git写操作。
