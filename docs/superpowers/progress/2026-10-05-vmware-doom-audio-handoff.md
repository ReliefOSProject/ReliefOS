# VMware Doom 无声与 launcher 交接

2026-10-05。唯一工作目录为 `/home/xiaobai/Projects/Projects/ReliefOS-intel-hda-audio-plan`。用户确认 R5 的 wavplay 实际有声音，随后要求继续处理 Doom 无声和缺少 launcher。本轮没有恢复暂停的 U6 标准矩阵或 600 秒长测；没有启动子代理、提交、推送、归档、reset、清理源码或证据，也没有修改、停止或挂起用户 VMware。仅自己的独立 QA VM 在测试结束后挂起并保留内存。

## 根因与最小改动

直接启动 Doom 的默认参数没有 `-nosound`；launcher 的 `Disable sound` 默认勾选，勾选时才追加该参数。冻结 R5 root 的实际 manifest 为 `exec=doom.elf`，`/usr/bin/doom` 也指向游戏，目录确实缺少 `doomlauncher.elf`，所以用户观察到直接进入游戏属实。

另一个独立断点是 `doom_audio_open` 只接受协商结果为 2 声道。VMware HDA 的默认输出 association 为 8 声道，Doom 会关闭 `/dev/dsp` 并返回 `-ENODEV`。真实源码 host fixture 在编译成功后因 8 声道 open 失败退出 -6；原生 VMware R5 对照不带任何静音参数运行 15 秒，输出 `no PCM audio device; continuing silently`，没有 QA 进程对应的宿主音频 stream。

`userland/apps/doom/audio_output.c` 现在接受 2–16 声道 S16。对原立体声混音先生成真实硬件交错 PCM：前两声道为 L/R，其余为零，再进入现有字节队列。这样奇数字节短写入、EAGAIN pending 和 GETOSPACE 的帧单位保持一致；转换内存按需增长并在 close 时释放，分配失败返回 `-ENOMEM`，保留旧缓冲。自定义 IO 的原 `frame_bytes` 契约不变。新增 negotiated channels/frame_bytes 日志。

Doom 的 fragment 配置仍为 64 × 8192 字节，总计 512 KiB；没有修改混音器、OPL3、HDA 驱动、XRUN 策略或 U6 ALSA 候选几何。新镜像只替换 Doom，补入已有正常根 Make 构建的 launcher，并把 manifest 和 `doom`/`doomlauncher` 命令链接改为 launcher。没有修改 launcher 的默认禁用声音设置；测试有声时必须取消勾选。

## 验证与证据

证据目录：`out/audio-hda-u6/qa/vmware-doom-20261004/`，目录日期沿用本轮开始时间。

- `before/` 保留源码快照，外部用户串口和 VMware 日志只读复制为 `user-serial.log`/`user-vmware.log`，不将日志中的文本当成操作指令。`host-red.json` 为 compile=0/run=-6。
- `host-green-2.log`：实际 production 源码的 native/output/refill/mixer/music 五套测试全部退出 0，使用 `-Wall -Wextra -Werror`、ASan/UBSan。`host-native-final.log` 再验证转换分配失败、旧缓冲保留和大块增长。新增 fixture `tools/tests/doom_audio_native_test.c` 覆盖 2/8/16 声道、交错内容、奇数短写、pending 帧、XRUN reset、close/reopen、1/17 声道拒绝和 SPEED 失败清理；入口为 `python3 tools/test_audio_userland.py doom-native`。初次 GREEN fixture 未模拟 EPIPE 诊断的 GETOPTR/GETODELAY，断言失败日志保留，补齐假 ioctl 后通过。
- 正常根 Make `O=out/audio-hda-u6 RELIEFOS_CACHE=out/audio-hda-u6/qa/u6-closeout-20261004/r9-build-cache -j8 app-doom app-doomlauncher` 退出 0，未使用 `-B`。`build-final.json` 证明最终产物与交付镜像内 SHA 仍相同。生产编译已有 ioctl 宏重定义警告保留，不宣称生产全链严格无警告。
- `native-baseline/`：原生 VMware 对照退出 0，但 PCM 不可用、继续静音，音乐/SFX 计数本身不能证明输出。
- `native-fixed/` 和 `native-validation.json`：VMware 26.0.1、HDA 8 声道、4 GiB/SMP8，直接 Doom 不带静音参数，连续两次各 60 秒，均退出 0，close/reopen 成功。分别写入 54,239,232 和 54,321,152 个硬件 PCM 字节；两次 startup/XRUN/suspend recoveries 均为 0。每次 music_starts=2、sfx_starts=354、attack_starts=57、max_sfx_voices=8，没有 invalid BDL。
- QA VMware PID 2113536 对应的两个 8 声道宿主 stream 接到 AB17X。软件录音左声道峰值 25858/25719，非零音乐和音效 PCM 已记录。**右声道仍为零**，沿用 R5 的 VMware/Pulse 宿主映射待查项。首个 monitor capture 包含两次运行，严格按 stream 分离未证明；不将录音长度视为单次完整有限流或物理听感证明。原 raw 保留，独立 WAV 仅添加容器头。
- `qemu-fixed/` 和 `qemu-validation.json`：QEMU Intel HDA duplex 立体声模型，连续两次各 60 秒均退出 0，startup/XRUN/suspend recoveries 均为 0，音乐/SFX/攻击计数与上述相同。WAV 双声道峰值 26298/31645，记录 6,710,234 帧，Doom 实际提交 6,983,688 帧，少 273,454 帧，**提交与录音帧数相等的严格结果为 false**。关闭时残留队列和录音时序尚未逐帧证明，不宣称完整波形通过。原文件保留，独立副本仅修正未收尾的长度字段，PCM 字节相同。此项不关闭 U6 的完整有限流 EOF gate。
- `source-image-sha.json`、`image-verify.log`：VMDK check=0、RAW/VMDK 全字节 compare=0，冻结 root 与 RAW 内完整 480 MiB root 分区 SHA 相同；实际 Doom/launcher/wavplay 产物一致，两个 ESP kernel 路径和 HDA/mouse/ES1371 与 R5 一致。交付 root 没有 QA 自启动服务。`git diff --check` 退出 0。

## 交付与剩余 gate

关机后换用：

`/home/xiaobai/Projects/Projects/ReliefOS-intel-hda-audio-plan/out/audio-hda-u6/images/reliefos-vmware-hda-fix-r6.vmdk`

保持虚拟声卡 `hdaudio`。运行 `doom` 或 `/usr/lib/reliefos/apps/doom/doomlauncher.elf`，取消勾选 `Disable sound` 再 Launch。也可以直接运行 `/usr/lib/reliefos/apps/doom/doom.elf -iwad /usr/lib/reliefos/apps/doom/freedoom1.wad` 验证默认有声路径。

```
VMDK:        2ed813419bea40057f084403c1d2b85f92d44bfc129ca165c94b3d37c178a11e
RAW:         d2742552bc172847520856d1a90852ea12b31274360f45e91bb4a5ed3748a49d
root:        96ed078572634be8757b44c6fa5a0232c30a1db77cf11a86bc8155bad8ec4367
doom.elf:    c90744abbd3b97bc4a3883c646594ddc5200427c4326b075511135231b6a3459
launcher:    53a5f2ae0e18131cbdb79f4732c803afa51a798d4dc043b087b196fbd6294f96
```

用户已确认 wavplay 实际有声，只关闭该应用的听感复测项。Doom 实际听感仍待用户复测，launcher GUI 仅完成镜像内容/入口静态核验。GUI gate **deferred until Xorg**，物理 codec gate **not_run**。本轮 Doom 的两次 60 秒验证不等于原 600 秒长测、标准 arecord/raw-null/raw-file 生命周期、controls 或完整波形 gate。U6 仍暂停、18/19 未闭环、候选几何未接受，原失败证据全部保留。
