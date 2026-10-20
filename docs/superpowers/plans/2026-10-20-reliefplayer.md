# ReliefPlayer 实施计划

> 使用 executing-plans 在当前开发工作区实施；用户此前要求直接执行，不设置批准关卡。
> 本任务不提交、不推送、不创建分支，保留无关 result/。

**目标：** 交付独立 FLTK MP3/WAV 播放器，真实音频输出及可响应的播放控制。

**架构：** GUI → 非阻塞控制/状态管道 → 解码和 OSS 工作进程。
核心解码、定位、重采样独立于 GUI 与音频设备，可以在宿主机验证。

## 1. 流式解码与采样转换

- [x] 新增 `tests/host/test_reliefplayer.cpp`、`tests/build/test-reliefplayer.sh`，
  先验证尚未实现的 WAV/MP3 解码接口失败。
- [x] 实现 `userland/apps/reliefplayer/decoder.{h,cpp}`：RIFF 边界、格式转换、
  MP3 帧索引、预解码定位、立体声输出，失败保留明确错误枚举。
- [x] 实现 `pcm.{h,cpp}`：`PcmStream::seek()` 与 `render()`，以源采样率/输出
  采样率的有理数位置实现线性插值，音量限制 0–100，额外声道清零。
- [x] 测试真实文件、边界、定位前后样本、输出帧数与静音，运行 ASan/UBSan。

## 2. 音频工作进程

- [x] 实现 `protocol.h`、`worker.{h,cpp}`：定长命令/状态，加载取消，播放、
  暂停、停止、定位、音量，设备协商、非阻塞短写、排空与错误报告。
- [x] 实现 `session.{h,cpp}`：父进程轮询、命令合并、切换文件、超时回收，
  只等待或终止自己创建的 PID。
- [x] 宿主机进程测试无设备错误、非法文件、加载取消和退出回收。
  另用 8 kHz 慢速 OSS 测试后端验证任意短写不丢字节、排空期间暂停与退出、
  定位到结尾、重新播放及补零尾段调音量不会误重播；MP3 尾标签有红绿回归。

## 3. 原生窗口与桌面集成

- [x] 实现 `main.cpp`、`window.{h,cpp}` 和 `reliefplayer.desktop`，FLTK 默认
  gtk+ 控件，固定清晰的控制分组，进度只在提交拖动时定位。
- [x] 更新组件、FLTK、minimp3 include、图标、中文翻译和文件管理器关联；
  使用标准 audio/mpeg 与 audio/x-wav MIME，不加入 Office。
- [x] 新增独立 APK 归属组，不修改已有 audio/soundctl 策略。
- [x] 运行组件、归属、文件关联、图标和翻译检查。

## 4. 构建与真实运行

- [x] `TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug -j4 app-reliefplayer nls image-vmdk`。
- [x] 从最终镜像读取 ELF、desktop、图标及组件信息核对。
- [x] 新增 `tools/test_reliefplayer_qemu.py`，使用基线私有副本注入最终 ELF，
  验证 MP3/WAV、暂停恢复、定位、停止、音量、错误提示和关闭。
- [x] QEMU HDA 输出至独立 WAV，检查非静音音频和采样率；保留真实窗口截图。
- [x] 写 `docs/apps/reliefplayer.md`，记录真实验证结果与限制，交付截图。

所有测试与构建使用仓库 `out/tmp`。最终运行 `git diff --check`；不将构建产物
或 result/ 纳入源码改动。默认 VMDK 是否启动与 VMware 验证分别记录。

## 2026-10-21 实际验证记录

- 解码、定位、跨缓冲区重采样、音量、ID3v1/APEv2 尾标签测试通过；
  ASan/UBSan 检查通过。工作进程切换、取消和回收，以及 8 kHz 慢速排空、
  短写保留、定位到末尾、重播、尾段音量修改的回归检查通过。
- 组件选择、APK 归属、文件关联、图标与 desktop、翻译、构建图、rootfs 和
  installer staging 检查通过；根测试仍在已有的 `soundctl` 归属检查处失败，
  没有修改该策略。记录在 `out/reliefplayer-root-tests.log`。
- 最终应用在 QEMU KVM / Xorg / HDA 中真实运行通过：WAV、MP3、暂停恢复、
  拖动定位、静音、停止、末尾定位和重播、自然结束、损坏文件提示及三次正常关闭。
  `out/rp-q4/result.json` 为 passed，工作进程本轮 underrun 恢复次数为 0。
- 音频采集独立识别了 124 个 440/880 Hz 立体声窗口和 122 个 660 Hz 单声道
  重采样窗口，每窗 4096 帧，输出为 48 kHz S16 双声道。QEMU WAV 头未收尾，
  保留原始采样文件，另存仅修正 RIFF 长度的副本后解析，未修改 PCM。
- 此前一次来宾启动中 HDA 探测超时后重试，随后播放器遇到 ENODEV；记录保留
  在 `out/rp-q3/`。新私有来宾正常初始化后完成上述验收，未修改内核。
- 最终截图位于 `out/rp-q4/reliefplayer-{wav,mp3,paused,stopped,complete,invalid}.png`。
  原始基线镜像哈希保持不变，运行使用注入当前 ELF 的私有副本。
- 应用、翻译和最终 VMDK 构建通过（`out/reliefplayer-complete-build.log`）。
  完整根分区与 ext4 一致，RAW/VMDK 逐块比较一致；从镜像读回 ELF、翻译、
  desktop、BMP 与六种尺寸的 PNG 均与源码/构建产物匹配，多媒体分组、文件扩展名
  和启动软链接已核对。证据：`out/reliefplayer-final-image-readback/result.json`。
  来宾运行与最终镜像 ELF 的 SHA256 均为
  `f3fc39eb753149e4d69fc7e9e92bbcb895f16469c841ab4532471c0679ffa428`。
- 默认 VMDK 未直接启动验收；VMware 验证未完成；真实硬件未验证。
- 未提交、未推送，内核子模块未修改，无关 `result/` 保留。
