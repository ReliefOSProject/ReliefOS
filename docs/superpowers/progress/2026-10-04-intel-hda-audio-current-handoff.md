# ReliefOS Intel HDA Audio 当前交接

唯一操作目录 `/home/xiaobai/Projects/Projects/ReliefOS-intel-hda-audio-plan`。2026-10-04 Asia/Shanghai。保留全部dirty/untracked源码、镜像与成功/失败证据；未代理，未提交/推送/归档/reset/删除原成果。GUI deferred to Xorg；physical codec gate **not_run**。

**U6仍18/19，未闭环。8192/65536仍为候选；原始512/2048无额外静音的完整有限流、长时XRUN仍红。第二次raw-file通过不得接受2048/4096或其他几何。**

## 阅读与目录

先本文件，再旧successor prompt、handoff、status、U6 batch review及final/resume两个audit。旧目录/代理要求被当前用户指令覆盖。当前C=`out/audio-hda-u6/qa/u6-closeout-20261004/`；此前L=`out/audio-hda-u6/qa/u6-lifecycle-20261004/`。原handoff/status/AUDIO及每次最小修复before源码在C完整保存；旧审计不覆盖。根HEAD bafaa59081f8c998037524fd5aea8865dd0468bd、kernelHEAD f8f5a4908807e81cca7ed766fb6c60e7829767dc，均detached。用户另有ReliefOS_Xorg QEMU，不得操作。

## 保留修复

1. raw-null首次EIO是QA把合法短写8192→4096、errno0合成EIO；helper重试正短写/EINTR，真实错误仍失败。未改null。
2. PCM XRUN PREPARE先同步STOP，失败保留DMA/state；消费旧通知后重置epoch。HDA保留真实WALLCLK full-ring拒绝，coarse ticks仅检测32bitWALLCLK完整alias。CHANNEL_INFO复制输入channel，HWSYNC/SYNC_PTR pinned pointer及累计高水位、真实ALSA boundary含OSS非二次幂回归。
3. dmix/dsnoop 8192/65536：32KiB period、256KiB ring、8period，48k下170.7ms/1.365s；有明显延迟代价。generic arecord client8192/24576、slave65536。
4. PCM DRAIN清零所有未提交ring，不推进appl、保留queued/full/wrap/U8；真实RED134/GREEN0。canonical probe仍512/2048/start512/96000、无静音尾；非阻塞DRAIN通过STATUS_SETUP确认正常完成，XRUN/EINVAL/断连仍失败。
5. HDA mixer恢复在format绑定后、START前；失败STOP并保留backing。旧amp寄存器readback正确但QEMUvoice重建全幅，module有效增益模型真实RED/GREEN。R8客体实际mute、half、balance、restore正确，但完整静音时长仍失败。
6. R9 STATUS按Linux保留超过ring的avail，运行中播放欠量有符号；非运行delay0；XRUN DELAY仍EPIPE。真实ioctl RED134/GREEN0（wrap/direct/capture）。position失败last/WALLCLK锁内快照，不改门限。
7. R10位置拒绝IRQ先清RUN并确认再通知XRUN，像FIFO/descriptor错误；STOP卡住保留RUNNING lease/backing，下一IRQ重试。真实tiny-ring/stuck-stop RED134/GREEN0；stream strict ASan/UBSan0，相关module/codec/control回归0。它不证明Doom/EOF已修复。
8. Doom只有诊断新增：实际scene计数及EPIPE epoch提交量/GETOPTR/GETODELAY/距上次写与io周期数；errno恢复、样本/恢复行为不变。reporter whole-serial增加正xrun_recoveries，真实RED1/GREEN0。原错误report不改，只新增重新解释。

## 当前冻结镜像R10

`out/audio-hda-u6/images/reliefos-lifecycle-r10.vmdk`

```
image: 6f446b63b460e3fbcf9dca5681cea4c713aa27ae89b003bcf98c5ae9bc5ad780
kernel.sys: cbfdbc8d23c7013fc2ee3ae4efabe30bb7036d31301f32087de93b8f9945e0f7
hda.drv: 2e4f2d77932609d2a2988d0f9db10219fd853c5d488833253b798d46f93d5c97
doom.elf: 900133757fe3490f527872393f98618c3bc9b3ffdec6cb52c6429dc9452267fa
alsa_pcm.c: 8dbee03cafe5bb4ca1ec387faede503d6beb114e628f4e70176548392a90fe7a
HDA stream: 6543fef0deeeb9d898bb171236b10d1d5ef952fb682cfe0b8cf563b3d6c0a1e7
Doom output: 6c649d03871a1539b40c00f2301c9cb802b8486927539af36a6906771f955af6
```

C/r10-source-products.json含VMDK/raw/liveISO/installer、所有相关source及QA binary SHA；完整root正常Make终止0，未-B。raw/VMDK compare0。R10实际镜像ESP两kernel路径、root HDA/asound/Doom五组件逐项SHA匹配（C/r10-extracted-components.json）；并未沿用R9提取。C/r10-git-status.json记录保留的dirty/untracked。

## 关键证据/严格红门

- R6 ds94–99六组合通过、ds100双codec long-file EPIPE 1884160帧、write最大1259.819ms、slave avail72995>65536，当时并发full构建。R7 ds103双codec全部10capture/2883584连续帧通过，不撤销ds100。
- R8 ds108 native16+timer API全0，但g0截349、g8截16；ds109 controls功能正确但静音95875/96000少125；ds110 g24 hw73823/appl73774，真欠49帧，无hda-pos，timer DRAIN EPIPE。这些与host make test并发，只是压力诊断。
- R8 ds107实际guest五节点权限owner/primary/supp/other/root、EACCES/EPERM及0666恢复0，waveform not_applicable。ds112无声卡/dev/dsp open ENODEV、PCM/control ENOENT，Doom静默20秒正常退出、SHM83；ds111错误假设节点不存在的QA失败保留。
- R9 ds124新版600秒标题→demo/60秒CPU+IO完成music4/SFX3869/attack1230/voices8，**XRUN恢复2，e2e退出1**。standard/core API和10capture全完成；long2883584帧连续、容差2、phase24、SHA7e92b80ffe7302e5af56a028734c4595ab306353e70af0f073ab1ee74ab520a5。native前16匹配，timer47614/48000起截386帧，完整native1。原e2e whole-serial字段漏恢复计数错误true；修正reporter解释false，原report完整保存。
- native-prefix checker只划定随后明确启动标准应用前的完整17native阶段，所有提交帧、全部中间gap和最终512帧零停止gap仍严格；后续标准应用自有声音另行验证。合成完整/截尾/额外旧样本分别0/1/1，不是中段窗口或容差放宽。原whole-only checker保存。
- affected PCM/device/params/ioctl/mmap strict（仅继承unused helper抑制）通过；R10 HDAstream无警告strict及相关sanitizer0、Doom output/refill strict sanitizer0。整份module strict继承misleading-indentation/unused仍失败，不假称全suite严格绿色。
- root make test实际2：test-pages branding-compatibility六个旧文档链接缺失，完整C/r8-make-test.log；未改无关文档或跳过。R9 audio packages/SDK0。完整kernel diff旧固定UAPI空白错误保留。

## 当前执行断点

R10完整标准矩阵C/r10-standard-matrix.py运行中（session87665）；ds125 Intel/MSI-on、ds126 Intel/MSI-off、ds127 ICH9/MSI-on的runner/standard/native API、全部10capture与60秒已知输入均通过，但完整native17流截尾，三项严格结果false。当前ds128 ICH9/MSI-off；全部8项不因早期失败停止。进度与每项真实退出见C/r10-standard-matrix-results.json。

WAV节拍对照ds133因不支持ADC而在abi原cold raw-null capture阻塞，使用自己的QMP终止；Doom120未运行，完整scope-stop证据保留。修正诊断适用范围后的ds134使用hda-output，仅native/SHM及Doom/controls，明确capture not_run：120秒Doom/60秒负载music2、SFX429、attack71、voices8，written25534604、恢复0/0/0。但native完整波形g0截547帧、timer截819帧，runner/完整wave均1。不能据120秒WAV零XRUN与600秒ALSA-file两XRUN归因于后端。

1. 等待ds125–132全部真实终止；顺序保留runner、完整native、long输入、lifecycle、whole-serial及所有反例。8192/65536仍未接受。
2. R10补none真实Doom、legacy实际能力/波形和controls fullwave；随后R10新版600秒/60秒ALSA-file长测，读取真实Doom EPIPE epoch/hw/cycles，不凭增大buffer或尾静音绕过。
3. R10镜像实际五组件SHA已匹配；补packages/SDK最终退出与源审计，更新status/AUDIO/ABI inventory。受影响host strict已通过，但完整module继承strict警告及make test旧链接失败均保留。
4. 继续定位有限流EOF及长时XRUN；账本18/19不关闭。GUI deferred Xorg；physical not_run。

磁盘空间紧，只对独占已结束的证据及boot前的私有staging做零洞/相同字节Btrfs dedup；每项before/after SHA、size一致，无删除/替换内容。staging-byte-preservation.json明确preboot，不伪造run完成；私有disk写入仍COW且原image SHA验证。C/preserve-r10-run.py用于完成后的保留维护。所有操作只在指定树，外部cache仅只读。

## 用户转交 VMware 测试

2026-10-04 用户明确表示本轮足够，将自行进行 VMware 测试，目标已暂停。已结束矩阵协调进程并通过 QMP quit 停止自己的 ds130 VM，全部原证据保留。ds125–129 完成，ds130 interrupted，ds131/132 not_run；不将主动停止当成音频回归结论。R10冻结 VMDK SHA 仍为6f446b63b460e3fbcf9dca5681cea4c713aa27ae89b003bcf98c5ae9bc5ad780。停止记录见 C/user-vmware-stop-20261004.json。U6未完成，GUI延后Xorg，物理codec not_run。

## 独立的 VMware GUI 输入诊断

用户报告 VMware 登录画面、键盘和鼠标均不响应，禁用 3D 同样发生。已独立复现并修复 i8042 配置回复被 IRQ1 抢读导致配置写成 0x02 的启动输入故障，以及遗留空 IRQ1 读取旧数据的问题。正常 root Make kernel、12 项 host 输入回归、严格 IRQ/ASan/UBSan 和 QEMU VMware-SVGA 的 SMP8/SMP1（各含 60 秒后交互）通过。新诊断镜像 `out/audio-hda-u6/images/reliefos-vmware-input-fix-r2.vmdk`；真实 VMware 复测仍 pending，不能将模型验证视为 VMware 通过。详细交接见同目录 `2026-10-04-vmware-gui-input-handoff.md`，证据在 `out/audio-hda-u6/qa/vmware-login-20261004/`。用户原 R10 VMDK 已被其测试写入，不再沿用旧冻结 SHA；本轮没有覆盖它。U6 仍暂停且未完成，整体 GUI/Xorg gate 仍延后，physical codec gate 仍未执行。

## 独立的 VMware wavplay 诊断

用户在 input-fix-r2 上报告无声：第一次 PCM 写入后队列满退出，第二次报设备不可用。已用实际源码回归修复 POSIX EAGAIN 重试、旧队列上的同速协商、块后休眠导致的停播及相对 nanosleep 混用原始/校时时钟的问题；信号中断剩余时间同步修复。候选 audio-fix-r2 的第二次失败保留，最终 audio-fix-r3 在 QEMU AC97 连续两次 30 秒播放/关闭/重开与波形通过，相关严格 host/ASan/UBSan、正常根 Make、VMDK check/compare、source/image SHA 通过。最终复测文件 `out/audio-hda-u6/images/reliefos-vmware-audio-fix-r3.vmdk`，SHA256 `d2641c787eb5a2635e90d2c75ff2960c82e8b21d46a787cf934253a2da2ca169`。详细交接见 `2026-10-04-vmware-wavplay-handoff.md`，证据在 `out/audio-hda-u6/qa/vmware-audio-20261004/`。真实 VMware ES1371 出声仍待用户复测，不能用 AC97 模型代替；U6 仍暂停未完成，未恢复 HDA 标准矩阵、Doom/controls/600 秒长测，Xorg GUI deferred，物理 codec not_run。原用户磁盘及源码/证据均保留。

## VMware 改用 HDA 后的 OSS 打开失败

用户 R3 ES1371 播放 30 秒正常退出但仍报告无声，不能关闭其实际出声 gate。改为 `hdaudio` 后，codec `15ad1975` 已注册，但 `/dev/dsp` open 返回 -22。实际 VMware 图重放确认默认 association 为 4 个 stereo 成员/8 声道；真实 OSS fixture 复现 16 字节帧超出原 tail[8] 的 EINVAL。已把 OSS 播放/捕获拆分帧容量改为 32 字节，wavplay 按协商出的 2–16 声道将源 stereo 放到前两声道、其余零 PCM。相关 RED/GREEN、严格 host/ASan/UBSan、17 项 OSS 等待、正常根 Make、镜像 check/compare 和 source/image SHA 通过。

新诊断镜像 `out/audio-hda-u6/images/reliefos-vmware-hda-fix-r4.vmdk`，SHA256 `d77f2aa6b5dc955f15c6a1e3e802b391be5604b76345544f94d7a7c08e3c02f6`。QEMU HDA 两次 wavplay 均退出 0，但完整 WAV 共记录 59.911770833 秒/应提交 60 秒，少 4235 帧，严格波形结果仍 false。原失败证据不覆盖，不能将退出 0 视为完整播放通过。VMware 的 8 声道实际播放和 Sunshine 出声待用户复测，未更改 Sunshine 或用户 VM。详细交接 `2026-10-04-vmware-hda-wavplay-handoff.md`，证据 `out/audio-hda-u6/qa/vmware-hda-20261004/`。用户自行清理旧产物后要求继续；本轮没有删除原文件。U6 仍暂停未闭环，GUI/Xorg deferred，physical codec not_run。

## R5：VMware write -11 的 DMA 权限遗漏

用户改宿主输出为 AB17X 后，R4 播放返回 -11。外部日志只读快照及独立原生 VMware 复现两次 errno=11/退出 1，宿主均报 invalid BDL。QA VMSS/VMEM 证实 CBL=65536、LVI=7、FMT=0x17、BDLE/RAM 范围正确，但 PCI command=0x2、LPIB/position=0。`hda_enter_immediate_mode` 为回收失败 command ring 关闭了 bus mastering，成功 fallback 后遗漏恢复，阻断 PCM DMA。只在 command ring 安全停机/reset/释放后恢复 master 位并验证 readback，失败仍按原安全清理路径返回。

证据 `out/audio-hda-u6/qa/vmware-bdl-20261004/`：controller RED 编译0/运行-6；controller/stream strict ASan/UBSan GREEN；正常根 Make0；原生 VMware 8 声道两次30秒均退出0、无 invalid BDL，close/reopen0；PCI command=0x6，LPIB/position=0xa000。确认 QA VMware 两个 stream 都接到 AB17X，软件录音有960Hz PCM，**右声道仍零**，宿主8声道映射作为待查线索，真实听感待用户验证。QEMU HDA两次退出0，但完整波形仍false、少6334帧，不能关闭有限流或完整音频 gate。

新交付 `out/audio-hda-u6/images/reliefos-vmware-hda-fix-r5.vmdk`，SHA256 `e813d9b75fcc6029eb2e518b6395d00c904040409abb55502af31df31379e503`；只替换冻结R4内的HDA，kernel/wavplay/mouse/ES1371/Doom不变，check/compare及全root/实际组件/source SHA通过。详细交接上述 VMware HDA 文档 R5 章节。没有操作用户运行的 VM、改宿主路由、提交、推送、reset、归档、删除原文件或启动子代理。U6继续暂停18/19，候选几何未接受；标准矩阵/raw-null/raw-file/Doom/controls/600秒本轮未重跑，GUI/Xorg deferred，物理codec not_run。

## R6：用户确认 wavplay 有声，继续修复 Doom

2026-10-05 用户确认 R5 wavplay 实际有声，Doom 仍静音，且打开时没有 launcher。冻结镜像确实缺少 launcher，入口直接指向 doom.elf；直接启动默认没有 -nosound。Doom 的 OSS 协商检查却只接受 2 声道，拒绝 VMware 的 8 声道后关闭设备并继续静音。host RED compile0/run-6 和原生 VMware 无静音参数对照均复现。

最小修复将立体声混音转换为协商的 2–16 声道硬件交错后进入原字节队列，使用真实帧宽核算 pending/GETOSPACE，保留 fragment 64×8192 几何、XRUN 和自定义 IO 契约。R6 补入 launcher，manifest 与 doom/doomlauncher 链接指向它；默认 Disable sound 仍勾选，复测需取消。Doom/launcher 正常根 Make0，native/output/refill/mixer/music strict ASan/UBSan 均0。

原生 VMware 8 声道两次各60秒 Doom 都退出0，close/reopen0、XRUN0，每次音乐2/SFX354/攻击57；AB17X 软件录音左声道非零，**已知右声道仍零，严格分离单次录音未证明**。QEMU HDA 立体声两次各60秒同样退出0/XRUN0，波形另行记录；不关闭 U6 完整有限流 EOF gate。新交付 `out/audio-hda-u6/images/reliefos-vmware-hda-fix-r6.vmdk`，SHA256 `2ed813419bea40057f084403c1d2b85f92d44bfc129ca165c94b3d37c178a11e`，check/compare、完整root、实际组件和 source/image SHA通过，交付没有QA服务。

详细交接 `2026-10-05-vmware-doom-audio-handoff.md`，证据 `out/audio-hda-u6/qa/vmware-doom-20261004/`。用户 VM/R5 VMDK 未操作；自己的 QA VM 已挂起保留。用户已确认 R6 在 VMware 实际有声音；右声道录音和完整波形问题仍单独保留。launcher GUI 未运行；GUI/Xorg deferred，physical not_run。U6 仍暂停18/19，标准 arecord、raw-null/raw-file、controls 和600秒本轮未恢复，候选几何未接受，无子代理/提交/推送/reset/归档/源码证据清理。
