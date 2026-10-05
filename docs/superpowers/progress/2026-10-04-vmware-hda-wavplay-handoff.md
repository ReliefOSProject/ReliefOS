# VMware HDA wavplay 打开失败交接

2026-10-04。唯一操作目录 `/home/xiaobai/Projects/Projects/ReliefOS-intel-hda-audio-plan`。用户暂停的 U6 未恢复；本轮只处理 VMware 换用 `sound.virtualDev = "hdaudio"` 后 `wavplay` 报 `open /dev/dsp failed errno=-22` 的问题。没有子代理、提交、推送、归档、reset 或删除原源码/证据，没有修改或停止用户 VMware。用户随后自行清理旧产物并明确要求继续。宿主音频通过 Sunshine，未更改其路由。

## 根因与修复

原 VMware R3 串口已识别 codec `15ad1975` 并注册可用声卡，CORB/RIRB 初始化失败后使用 immediate transport。用完整日志拓扑重放实际 `hda_codec_probe` / `hda_find_route_group`，默认输出 association 2 有四个立体声成员，共 8 声道：pin14/DAC02、pin16/DAC03、pin15/DAC04、pin17/DAC05，起始声道为 0/2/4/6。

实际 OSS/core/PCM fixture 在 8 声道上复现 `audio_oss_open=-22` 并 SIGABRT：S16 单帧需要 16 字节，原 `tail[8]` 容量不足。只扩大 OSS 播放/捕获暂存及局部拆分帧至 32 字节，支持 HDA 最大 16 声道的 S16 单帧；未绕过格式、lease、XRUN 或位置校验。

修复打开后，原 `wavplay` 仍会拒绝协商出的 8 声道，实际应用 fixture 也先 SIGABRT。播放器现在接受 2–16 声道的 S16 格式，按真实交错布局把源立体声放到前两声道，其余填零 PCM。对多声道按帧宽选择 OSS 的八个 fragment，每 period 至少保持原默认的 512 帧；8 声道为 8192 字节、512 帧，环仍 4096 帧/约 85.3 ms。这不是 U6 ALSA 候选几何验收，也没有在播放结束追加静音 PCM 来掩盖截尾。

改动为内核 `audio/oss.c`、`userland/apps/wavplay/main.c` 及对应测试。HDA、ES1371、Doom 驱动/应用产物未改。

## 已执行验证

证据根目录：`out/audio-hda-u6/qa/vmware-hda-20261004/`。

- `replay-topology.log`：来自实际 VMware 日志的图重放，4 成员/8 声道。
- `oss-multi-red.log`：真实 8 声道 open=-22；`wavplay-multi-red.log`：原播放器拒绝协商 8 声道。两项编译成功后实际失败，退出 -6。
- `host-final.json`：OSS 与 wavplay 严格 `-Wall -Wextra -Werror`、ASan/UBSan 编译及运行均 0；OSS 仅保留原 fixture 的 unused helper 抑制。覆盖 8/16 声道播放与捕获的拆分帧、字节内容、关闭重开，以及 17 声道拒绝后的 lease 释放；播放器检查前两声道、其余零 PCM、相位跨块、原重试/排空路径及不支持格式拒绝。
- `host-oss-regressions.log`：原立体声/ABI-v1 fixture 和 17 项真实 OSS 等待回归全部通过。
- 正常根 Make `kernel app-wavplay` 退出 0，未使用 `-B`。`production-build.log` 保留 ioctl 宏重定义和 loader `text_in` 警告；不宣称生产全链严格无警告。
- `qemu-hda-r4/`：独立 QEMU Intel HDA duplex 立体声模型，连续两次 `wavplay` 均退出 0，正常关闭后重开，QEMU 退出 0。WAV 后端无法提供 ADC，捕获未在此 VM 运行；不能替代 VMware 的 8 声道 codec 或 Sunshine 出声验证。
- **完整波形仍失败**：两次共提交 2,880,000 帧/60 秒，记录 2,875,765 帧/59.911770833 秒，少 4235 帧。`qemu-hda-wave.json` / `qemu-hda-wave-detail.json` 保留严格 false 结果，不能因应用退出 0 宣称完整有限流通过。两次音频在录音中连在一起，未把未证明的分段长度记为成功。原 WAV 未收尾的长度字段为零，只在独立 reflink 副本修正两个容器字段，全部原 PCM 字节保留。
- `vmdk-check.log` / `raw-vmdk-compare.log`：无错误、RAW/VMDK 字节相同。`r4-source-image-sha.json` 确认 RAW 完整 root 分区、两个实际 ESP kernel 路径、实际 wavplay 及保留的 HDA/ES1371/mouse/Doom 产物与 source SHA。

## 新诊断镜像

`/home/xiaobai/Projects/Projects/ReliefOS-intel-hda-audio-plan/out/audio-hda-u6/images/reliefos-vmware-hda-fix-r4.vmdk`

```
VMDK:       d77f2aa6b5dc955f15c6a1e3e802b391be5604b76345544f94d7a7c08e3c02f6
RAW:        f24ce7620385bbada0671e71999eb3a01337da0016061b89f2e169c156a5dfa1
kernel.sys: f0c36dd0b61136a9679c7a65f1f48c5cb07a72854fc31b908e4069dbab3d20e2
wavplay:    fc32419caa58e80ac11bb50bb226cd1f801c736790eabf74327fd553384a127c
```

基于冻结 R3 RAW，只替换 OSS 内核和 wavplay；交付镜像不包含 QA 自启动服务。未执行全套 distribution/packages/SDK 重建。R3 VMDK 正被用户 VMware 写入，不沿用其旧交付 SHA；本轮未覆盖它。

用户关机后换用 R4 VMDK，保留 HDA 设置，运行 `wavplay`，正常结束后立即再运行一次。预期协商显示 `8 output channels`；真实 VMware 成功打开、写入、完整播放及通过 Sunshine 出声均待复测。若失败，结合具体操作/errno 和新的完整串口继续查；DMA 前进或退出 0 都不等于实际有声。

U6 仍暂停、18/19 未闭环，标准矩阵、raw-null/raw-file、Doom/controls 和 600 秒长测本轮未重跑，候选 ALSA 几何未接受。GUI/Xorg gate **deferred**，物理 codec gate **not_run**。空间紧张期间只对闲置、结束的产物做字节相同 Btrfs dedup/零洞整理，before/after SHA 均一致，证据 JSON 保留。

## R5：VMware PCM write EAGAIN 与 DMA 权限

用户 R4 复测返回 `PCM write failed errno=-11`，并自行把宿主默认输出从 Sunshine 改为 AB17X。本轮只在原工作树继续这一播放故障，没有恢复暂停的 U6，也没有修改、停止或挂起用户 LeonOS4 VM、覆盖它正在写入的 R4 VMDK，或更改宿主音频路由。独立 QA VM 的创建、磁盘、串口、VMware 日志及挂起内存均在本工作树内；只挂起自己新建的 QA VM 保留证据。

证据目录 `out/audio-hda-u6/qa/vmware-bdl-20261004/`：

- `user-r4-serial.log` / `user-r4-vmware.log` / `user-r4.vmx` 为外部文件的只读快照。用户 VMware 26.0.1 拒绝 stream 4，报 `invalid BDL 302af000`；guest wavplay 约两秒后退出 1。
- `native-r4-memory/` 用实际 R4 驱动、8 声道 codec `15ad1975` 连续两次复现 errno=11/退出 1，主机日志均有 invalid BDL。播放器确实执行了有限 EAGAIN 重试，并非需要再把负 errno 改写或增加等待时间。
- `native-dma-state-comparison.json` 来自自己的 VMSS/VMEM，内存 region 映射验证后读取真实 BDL。R4 stream4 CBL=65536、LVI=7、FMT=0x17；8 个 BDLE 各 8192 字节、IOC=1，PCM/BDL 地址均在 RAM 中，长度合计正确。PCI command 却为 **0x0002**，LPIB 与 position DMA 都为 0。
- 根因在 `hda_enter_immediate_mode`：CORB/RIRB 失败后安全停机和回收 ring 时关闭了 PCI bus mastering，成功进入 immediate 命令模式后没有恢复。Codec 的 PIO 命令可用，但 PCM/position 仍需要 DMA，VMware 因 DMA 权限关闭无法读取 BDL。

最小改动只在 `kernel/reliefnt/drivers/hda/controller.c`：确认 command ring 停止、完成 reset 并释放失败 ring 的 IRQ/DMA 后，恢复 bus-master 位并验证 PCI readback；失败返回 -EIO，沿原安全清理路径退出。保留其他 command/status 位。未改变 PCM 几何、WALLCLK/XRUN 规则、OSS 或 wavplay。

`controller-red.log/json` 先以真实生产 controller 函数验证 fallback 后 master 位未恢复，编译 0、运行 -6。新增测试覆盖 CORB/RIRB 两种失败、其他 PCI 位保留、两次 acquire/release、恢复权限被拒绝时的错误和资源回收。`host-strict.json` 的 controller/stream 均以 `-Wall -Wextra -Werror`、ASan/UBSan 编译和运行 0。正常根 Make `kernel app-wavplay` 退出 0；kernel 与 wavplay SHA 均与 R4 相同，HDA 驱动 SHA 更新。`scoped-diff-check.json` 对两个修改文件的 before/after 检查通过；整份子仓 diff 的原 UAPI 空白问题仍保留。

`native-r5/` 为原生 VMware 26.0.1、8 CPU/4 GiB、EFI、HDA 8 声道的同路径复测。两次 30 秒 wavplay 均正常完成/退出 0，close 后 reopen 成功；无 invalid BDL、hda-pos 拒绝或 PCM 失败。挂起后的 PCI command=**0x0006**，LPIB 与 position DMA 均到 **0xa000**。先失败、再单点修复、再原平台通过，确认此处 DMA 权限遗漏导致本次 EAGAIN。

`native-r5/pulse-events.json` 用 `application.process.id` 匹配自己的 QA VMware，证明两次 48 kHz/8 声道 stream 都连接 AB17X sink 57、未 mute。`native-r5-monitor-wave.json` 记录软件 monitor 的非零 960 Hz、峰值 6000 PCM。**右声道全零**：VMware 上报的 8 声道 channel map 第 1 通道为 front-left-of-center，第 3 通道才是 front-right；guest wavplay 按 HDA front converter 的前两通道送 stereo。此为后续声道映射调查线索，尚未通过正常 Linux guest 对照确认，不据此重排 guest 通道或改宿主路由。Monitor 在流创建后附加，且首次文件包含两次播放，不能用它证明每次完整帧数或独立 stream 隔离；AB17X 实物是否听到声音仍待用户复测。

`vmware-hda-20261004/qemu-hda-r5/` 的 QEMU Intel HDA duplex 立体声回归，两次 wavplay/close/reopen 均退出 0，QEMU 退出 0。**完整波形仍 false**：`qemu-r5-wave.json` 共记录 2,873,666/应提交 2,880,000 帧，少 6334 帧（约 0.132 秒）；未用截取中段、尾静音或容差放宽掩盖。原始 WAV 保存，独立 reflink 只修正两个容器长度字段，PCM 全字节一致。

交付 `out/audio-hda-u6/images/reliefos-vmware-hda-fix-r5.vmdk`，从冻结 R4 RAW 只替换 HDA 驱动；交付镜像没有 QA 自启动服务。`vmdk-check.log` / `raw-vmdk-compare.log` 均成功，`r5-source-image-sha.json` 验证全 root 分区、两个实际 ESP kernel 路径、实际 HDA/wavplay/mouse/ES1371/Doom 和源文件 SHA：

```
VMDK:       e813d9b75fcc6029eb2e518b6395d00c904040409abb55502af31df31379e503
RAW:        b7521aadd20715ff929c983973dc5b53efc4c18d89def977586a1164b0c2eb05
HDA:        6908cde5d20407682c10baaa6e0fb6c675ebea4cea8f8ddd96e2605e503b660c
kernel.sys: f0c36dd0b61136a9679c7a65f1f48c5cb07a72854fc31b908e4069dbab3d20e2
wavplay:    fc32419caa58e80ac11bb50bb226cd1f801c736790eabf74327fd553384a127c
```

用户关机后换用 R5、保留 hdaudio/AB17X，并运行 wavplay 两次，验证真实听感。R5 关闭的是本次 DMA 停滞/EAGAIN 原因，完整音频验收没有关闭；右声道、有限流尾部、U6 标准矩阵/raw-null/raw-file/Doom/controls/600 秒仍未闭环，本轮未重建完整 packages/SDK。GUI/Xorg **deferred**，物理 codec **not_run**。

VMSS 内存映射解析方法参考 [Volatility Foundation VMware layer](https://raw.githubusercontent.com/volatilityfoundation/volatility3/develop/volatility3/framework/layers/vmware.py)，只用于读取本工作树内 QA 文件。
