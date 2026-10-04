# 音频库、系统控制程序与 Doom 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 subagent-driven-development（推荐）或 executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 用上游 ALSA 工具验证 Linux 音频接口，让程序和设置页控制声音，并让 Doom 稳定播放高质量音效与 OPL 音乐。

**架构：** alsa-lib 在用户态提供 plug/dmix/dsnoop；独立音频 helper 库向 CLI/GUI 提供基于标准 ALSA control 的操作。Doom 分离非阻塞传输、音效混音和基于音频 frame 的 MUS/MIDI/GENMIDI/OPL3 音源，避免音频提交阻塞游戏绘制。

**技术栈：** C、musl、alsa-lib 1.2.14、alsa-utils 1.2.14、固定提交的 Nuked OPL3 1.8、GNU Make/Shell、现有 UI/OpenRC；非生产 Python/C 波形测试。

**规格：** `docs/superpowers/specs/2026-09-30-intel-hda-audio-design.md` 第 6–9 节。H/A 合同见另外两份同日计划；本文已获用户批准，实施进度由复选框和验证证据记录。

## 全局约束

- 参考库固定 **alsa-lib 1.2.14、alsa-utils 1.2.14**。
- 默认 48 kHz/stereo/S16 的交互播放采用 512-frame period、4-period buffer。
- 程序调用标准 ALSA/OSS，不增加私有声音 syscall。
- 生产链保持 C/汇编 + Make/Shell。
- 正常负载下 XRUN 为 0；Freedoom 音乐与音效同时运行 10 分钟。
- 真机未提供时明确记录未验收，不把 fixture 当作真机。
- 主仓 `AGENT.md` 的无擅自提交/push 规则优先于技能的频繁 commit 建议。

---

## 文件结构

| 创建或修改 | 职责 |
| --- | --- |
| 修改 `configs/dependencies.lock.json`、`mk/upstream.mk`、`tools/build/upstream.sh` | 固定 alsa-lib/utils，configure/Make，与真实文件成员联动 |
| 创建 `mk/components/audio.mk`、`tools/build/opl3.sh` | 纯 C OPL3 动态库及音频 helper 库构建 |
| 修改 `tools/build/rootfs-stage.sh`、`mk/rootfs.mk`、`mk/sdk.mk`、`tools/build/musl-sdk.sh` | 音频库/插件/配置/工具/许可证/SDK 的 staging 和依赖 |
| 创建 `system/rootfs/etc/asound.conf`、`etc/init.d/reliefos-audio` | 默认共享 PCM 与控件持久化 |
| 创建 `include/reliefos/audio_control.h`、`userland/audio/audio_control.c` | opt-in `libreliefos-audio.so.1`，不让基础 runtime 新依赖 libasound |
| 创建 `userland/apps/soundctl/{main.c,soundctl.app.ini}`；修改组件 TOML/Kconfig.components | 声音 CLI、设备选择、读写控件、监听、扬声器测试 |
| 创建 `userland/apps/settings/{sound_page.c,sound_page.h}`；修改 settings/main.c | 声音设置页，事件和 worker 更新 |
| 创建 `userland/apps/doom/{audio_output.c,audio_output.h,audio_mixer.c,audio_mixer.h,music.c,music.h,music_opl.c,music_opl.h}` | 传输、音效、事件时钟、GENMIDI/OPL 音源 |
| 修改 `userland/apps/doom/reliefos_audio.c`、`mk/userland.mk` | Doom 模块接口转发与显式 ALSA/OPL 依赖 |
| 创建 `tools/tests/{audio_control_client,settings_sound,doom_audio_output,doom_audio_mixer,doom_music}_test.c` | fake ALSA/transport 与确定性音频测试 |
| 创建 `tools/test_audio_userland.py`、`tools/test_audio_e2e_qemu.py`、`tests/build/test-audio-packages.sh`、`docs/AUDIO.md` | 构建/共享播放/波形/GUI/Doom 验收与文档 |

U1 可以先构建库但运行验收依赖 H/A。U2 依赖 U1+A5，U3 依赖 U2。U4/U5 可以在 host 同源测试，真实运行依赖 U1+H6。使用独立 `O=out/audio-hda`，所有依赖只在显式 `make fetch` 获取。

## 任务 U1：上游 ALSA/OPL3、共享 PCM 和 SDK 闭环

**文件：** 修改 lock/upstream/rootfs/SDK；创建 audio.mk、opl3.sh、asound.conf、test-audio-packages.sh。

- [ ] **步骤 1：写完整成员、可重现与真实链接失败测试。** shell fixture 检查 libasound SONAME、plugins/config/public headers、aplay/arecord/amixer/alsactl/speaker-test、libopl3 和许可证。用 SDK 的驱动编译实际 ALSA client，不能只 `readelf`：

```c
#include <alsa/asoundlib.h>
int main(void) {
    snd_pcm_t *pcm;
    int r = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (r < 0) return 1;
    r = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                          SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 1, 50000);
    snd_pcm_close(pcm);
    return r < 0;
}
```

SDK 测试只编译/链接，不在 host 运行 target binary；实际执行交给 U6。删除 staged `alsa/pcm/dmix.conf` 或 plugin 后重复 make，要求恢复；`make -n` 不下载、不修改 staging。

- [ ] **步骤 2：红测试。** `sh tests/build/test-audio-packages.sh`；预期没有音频库/工具/SDK 文件失败。

- [ ] **步骤 3：加入已核实身份的依赖并构建。** 规划的 lock 条目如下，下载 checksum 在本次调查中从一手 URL 实测，生产仍自行核验：

```json
[
  {
    "id": "alsa-lib", "kind": "tarball", "version": "1.2.14",
    "url": "https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.14.tar.bz2",
    "sha256": "be9c88a0b3604367dd74167a2b754a35e142f670292ae47a2fdef27a2ee97a32",
    "directory": "alsa-lib-1.2.14", "script": "tools/build/upstream.sh",
    "target": "upstream-alsa-lib", "license_in_source": "COPYING"
  },
  {
    "id": "alsa-utils", "kind": "tarball", "version": "1.2.14",
    "url": "https://www.alsa-project.org/files/pub/utils/alsa-utils-1.2.14.tar.bz2",
    "sha256": "0794c74d33fed943e7c50609c13089e409312b6c403d6ae8984fc429c0960741",
    "directory": "alsa-utils-1.2.14", "script": "tools/build/upstream.sh",
    "target": "upstream-alsa-utils", "license_in_source": "COPYING"
  },
  {
    "id": "nuked-opl3", "kind": "tarball", "version": "1.8",
    "commit": "765ec962e473aeb767e4cba74ffdc8f588ffbfe8",
    "url": "https://codeload.github.com/nukeykt/Nuked-OPL3/tar.gz/765ec962e473aeb767e4cba74ffdc8f588ffbfe8",
    "sha256": "2fad908c3904d3ef51b55e0d6a8980c7142942e2d0baa7e99bf038cf0a41199d",
    "directory": "Nuked-OPL3-765ec962e473aeb767e4cba74ffdc8f588ffbfe8",
    "script": "tools/build/opl3.sh", "target": "upstream-nuked-opl3",
    "license_in_source": "LICENSE"
  }
]
```

沿既有 upstream macro 增加两包，`upstream_alsa-utils_deps := alsa-lib`；将 `usr/include/alsa/asoundlib.h`、`usr/lib/libasound.so.2` 和工具列为 grouped primary，installed-files/presence signature 覆盖全部插件/配置/软链接。alsa-lib configure 使用 target musl、shared+static、关闭 Python；alsa-utils 关闭 alsamixer/bat/alsaconf/NLS/XML 文档，保留五个强制工具。用该版 configure 的 `--help` 校核实际选项，不在生产构建运行 Python/docs 工具。utils 的 pkg-config sysroot 只指向其 private deps；不得误链宿主 alsa。

OPL3 使用 `opl3.c`、`opl3.h` 的原始 C 实现，target CC 编译 PIC，链接 `libopl3.so.1`；复制头、LICENSE 和 source identity，保留 LGPL 归属，不改成 OS 的 Apache 标头。动态链接便于替换库。脚本复用 upstream tar 安全检查、digest、私有 O、原子 stage 和 manifest 模式。

默认 `/etc/asound.conf`：

```text
pcm.reliefos_mix {
    type dmix
    ipc_key 1380273241
    ipc_key_add_uid true
    ipc_perm 0600
    slave { pcm "hw:0,0" rate 48000 format S16_LE channels 2 period_size 512 buffer_size 2048 }
}
pcm.reliefos_record {
    type dsnoop
    ipc_key 1380273242
    ipc_key_add_uid true
    ipc_perm 0600
    slave { pcm "hw:0,0" rate 48000 format S16_LE channels 2 period_size 512 buffer_size 2048 }
}
pcm.!default {
    type asym
    playback.pcm { type plug slave.pcm "reliefos_mix" }
    capture.pcm { type plug slave.pcm "reliefos_record" }
}
ctl.!default { type hw card 0 }
```

无 card0/capture 时库返回真实错误；不创建 fake sink。其他卡或格式通过用户 `.asoundrc`/显式 PCM 名选择，不能宣称固定配置适用于全部硬件。默认卡注册顺序按 BDF+codec 稳定排序；多卡设置更换用户态 PCM 后重新打开客户端。SDK 和 rootfs stage 包含实际库、配置与许可证；installer settings policy 如启用声音页，也必须 stage 所需音频库，或按 U3 禁用该页及其链接依赖。

- [ ] **步骤 4：绿构建与脱离源码的 SDK 验证。** `make fetch`；`make O=out/audio-hda -j8 upstream-alsa-lib upstream-alsa-utils upstream-nuked-opl3 runtime sdk`；`sh tests/build/test-audio-packages.sh`；`make test`。解压 SDK 到含空格路径，驱动编译 ALSA client；检查 ELF NEEDED/SONAME 指向目标库，损坏 cache digest 必须失败、缺失 stage 文件重建。

- [ ] **步骤 5：审查 staging 与 LGPL/GPL 归属。** 核对 lib/headers/plugins/config 的归属与 SDK dependency；不能只改一个 `-lasound`。授权后提交 `feat(audio): build ALSA tools and OPL3 runtime`。

## 任务 U2：标准音频控制 helper 与 soundctl

**文件：** 创建 audio_control.h/audio_control.c、soundctl 目录、`audio_control_client_test.c`；修改组件清单/Kconfig.components、audio.mk、SDK/staging。

- [ ] **步骤 1：写真实 ALSA 调用的客户端错误测试。** fake ALSA backend 返回两个卡、增益/开关/输出 enum 和事件；helper 只用 libasound control API，测试记录实际 element read/write/poll 调用。公开 API 与 POSIX 错误约定为：

```c
struct reliefos_audio_control;
struct reliefos_audio_control_desc {
    uint32_t numid, type, count, items;
    int64_t minimum, maximum, step;
    char name[44];
};
int reliefos_audio_control_open(uint32_t card, struct reliefos_audio_control **out);
int reliefos_audio_control_list(struct reliefos_audio_control *c,
                               struct reliefos_audio_control_desc *out,
                               uint32_t capacity, uint32_t *count);
int reliefos_audio_control_get(struct reliefos_audio_control *c, uint32_t numid,
                              int64_t *values, uint32_t capacity);
int reliefos_audio_control_set(struct reliefos_audio_control *c, uint32_t numid,
                              const int64_t *values, uint32_t count);
int reliefos_audio_control_wait(struct reliefos_audio_control *c, int timeout_ms,
                               uint32_t *numid, uint32_t *event_mask);
void reliefos_audio_control_close(struct reliefos_audio_control *c);
```

```c
struct reliefos_audio_control *c;
assert(reliefos_audio_control_open(0, &c) == 0);
int64_t v = 999;
errno = 0;
assert(reliefos_audio_control_set(c, 1, &v, 1) == -1 && errno == EINVAL);
/* fixture 定义 numid=1 合法范围 0..40，写超界时后台 write 次数仍为 0。 */
v = 20; assert(reliefos_audio_control_set(c, 1, &v, 1) == 0);
reliefos_audio_control_close(c);
```

测试 ALSA `-errno` 转为 libc `-1/errno`、缺卡、只读 jack、double close 防护、list capacity、enum bounds、EINTR wait、control events 外部改变、错误控制名和CLI退出码。

- [ ] **步骤 2：红测试。** `python3 tools/test_audio_userland.py controls`；缺 helper/CLI 编译失败。

- [ ] **步骤 3：实现 opt-in 库与 CLI。** opaque handle 保存 `snd_ctl_t *`、卡 ID、element snapshot。一次 get/set 先读 info、验证 type/count/access/range，再调用 ALSA；backend 错误统一转换为 `errno=-result; return -1`。程序不依赖新增 kernel-private 结构。CLI 使用以下稳定形式：

```sh
soundctl cards
soundctl --card 0 controls
soundctl --card 0 get 'Master Playback Volume'
soundctl --card 0 set 'Master Playback Volume' 50%
soundctl --card 0 set 'Master Playback Switch' on
soundctl --card 0 route headphones
soundctl --card 0 capture-source microphone
soundctl --card 0 watch
soundctl --pcm default test --channels 2 --seconds 2
```

按 control list 精确匹配名称/numid，enum item 获取真实 label，不硬编码 item=1 就是耳机。百分比按 min/max 转换并遵守 step；多 channel 显式支持两值或同值广播。route/capture-source 选择真实 Output Source/Input Source enum，未提供则显示可用控件并 ENOTSUP；不可对所有 codec 盲写 NID。test worker 播放左右相异 tone。cards 枚举用 libasound card API。list/query/watch 不要求 root；权限由节点 DAC 决定。SDK 导出 helper 库和 header，组件没有选择声音工具时不影响其他程序。

- [ ] **步骤 4：绿测试与实际构建。** `python3 tools/test_audio_userland.py controls`；`make O=out/audio-hda -j8 app-soundctl sdk`；`sh tests/build/test-audio-packages.sh`。U6 用 amixer 和 soundctl 互操作验证真实内核，不把 fake backend 结果当运行验收。

- [ ] **步骤 5：审查公共接口。** 数据布局只在用户态 helper 库使用，返回值不混用 errno；授权后提交 `feat(audio): add sound controls library and soundctl`。

## 任务 U3：声音设置页、事件更新与状态持久化

**文件：** 创建 settings/sound_page.c/.h、`settings_sound_test.c`、OpenRC 服务；修改 settings/main.c、audio.mk/userland.mk、rootfs staging、docs/AUDIO.md。

- [ ] **步骤 1：写页面模型和持久化失败测试。** 定义声音页独立模型：card/control 列表、selected card、volume/switch/route/source 值、loading/error、dirty control IDs。页面函数 `settings_sound_init/model_poll/handle_event/render/shutdown` 使用 U2 helper。测试 card removal、外部 amixer 更改、耳机插拔、失败 set 不显示成功、滚动/键盘 hitbox、小窗口、主题。服务 fixture 用 fake alsactl 返回成功/缺硬件/损坏文件，验证只恢复已存在文件，stop 的原子存储不损坏上一个状态。

```sh
test -x "$audio_root/etc/init.d/reliefos-audio"
test -f "$audio_root/etc/asound.conf"
test -f "$audio_root/usr/lib/libasound.so.2"
test -f "$audio_root/usr/lib/libreliefos-audio.so.1"
```

- [ ] **步骤 2：红测试。** `python3 tools/test_audio_userland.py settings`；`sh tests/build/test-audio-packages.sh`；预期没有页面/服务或共享库 staging 失败。

- [ ] **步骤 3：实现可响应声音页和 OpenRC 服务。** 在 settings 添加第八个 Sound tab，页面绘制/事件两处分派一致；所有新页面代码放独立文件，不把 helper 和 PCM 循环加进 main.c。volume/route/source 采用真实 UI 控件并遵守两主题、键盘导航、窗口布局。长 tone test、control event wait 放 worker，GUI poll 只消费状态快照；退出取消 worker、关闭PCM/control，不在 render 中 drain。无设备显示实际状态，禁止装饰性可点击 slider。

OpenRC 服务 `depend(){ after reliefos-runtime; }`，start 在 `/var/lib/alsa/asound.state` 存在且可读时 restore；不存在时保留驱动初始值，无硬件不阻塞系统启动。stop 先将 `alsactl -f <owned-temp-file> store` 写入同目录临时文件，检查成功后 rename 到 asound.state；失败保留原文件并记录错误。目录/file 权限为 root 管理，用户通过控制节点调整音量。标准 alsactl 的命令参数和退出码真实验证，不使用 shell 吞错伪装成功。installer policy 隐藏声音页并不链接 helper/asound，或同时加入完整依赖；本计划选择隐藏，以免扩张安装器音频流程。

- [ ] **步骤 4：绿测试与 GUI 验收。** `python3 tools/test_audio_userland.py settings controls`；`make O=out/audio-hda -j8 app-settings installer-userland image-vmdk installer`。QEMU U6 展示页面、调音量、静音、切换可用输出/录音源；外部 amixer 操作和重启恢复必须反映到UI。没有实际 GUI 观察不能记页面通过。

- [ ] **步骤 5：审查用户配置和授权提交。** 不覆盖任意 `.asoundrc`，不把设备路由状态和默认 PCM 配置混为一谈；授权后提交 `feat(settings): expose audio controls and persistence`。

## 任务 U4：Doom 非阻塞传输与音效混音

**文件：** 创建 audio_output.c/.h、audio_mixer.c/.h、`doom_audio_output_test.c`、`doom_audio_mixer_test.c`；修改 reliefos_audio.c 和 userland.mk。

- [ ] **步骤 1：写能重现 libc EAGAIN 的测试。** 注入 transport ops，测试脚本依次返回 short write、`-1/EAGAIN`、`-1/EINTR`、剩余帧；ALSA mock 则返回 `-EAGAIN/-EPIPE/-ESTRPIPE`，在 adapter 边界转换成统一状态。定义输出函数和 pending queue：

```c
struct doom_audio_output;
struct doom_audio_io {
    long (*write_bytes)(void *opaque, const void *pcm, uint32_t bytes);
    int (*recover)(void *opaque, int error);
    uint32_t (*available_frames)(void *opaque);
    void *opaque;
};
int doom_audio_open(struct doom_audio_output **out, const char *pcm,
                    uint32_t *actual_rate);
int doom_audio_submit(struct doom_audio_output *, const int16_t *, uint32_t frames);
int doom_audio_flush(struct doom_audio_output *);
void doom_audio_close(struct doom_audio_output *);
```

OSS adapter 核心回归用实际 libc convention：

```c
static long oss_write_bytes(int fd, const void *pcm, uint32_t bytes) {
    ssize_t n = write(fd, pcm, bytes);
    if (n < 0) return -errno;
    return n;
}
```

测试把剩余 byte/frame 保存完整，不重复已提交片段，EAGAIN 不关闭 audio，EINTR 不丢数据，recover 后清过期 queue。mix 测试含一段超过 65536 frame 的音效（检测现有 16.16 uint32 position 环绕）、48/44.1k、pitch、左/右 separation、重叠通道、缓存不足与坏 lump。

- [ ] **步骤 2：红测试。** `python3 tools/test_audio_userland.py doom-output doom-mixer`；保留当前 `written == -EAGAIN` 行为时测试必须失败，记录复现。

- [ ] **步骤 3：分离输出和音效模块。** ALSA 主路径 `snd_pcm_open(default,PLAYBACK,SND_PCM_NONBLOCK)`，HW/SW params 协商 S16 stereo、rate-near、period/buffer-near，读回实际值；`start_threshold` 预填两个 period，avail_min 一个 period。仅 ALSA 无可用路径时尝试 OSS，明确日志 backend/errno；两个都缺失保持无声可玩。内部传输返回字节或负 errno；ALSA adapter 将 writei 的 frame 数乘以 frame_bytes，OSS adapter 保留 write 的真实字节数。只有 OSS adapter 接触 libc `errno`，pending 保存 byte offset；测试注入 2 字节短写，确保不足一 frame 的尾部不被整数除法丢弃。

pending ring 字节上限为协商 buffer_frames*frame_bytes；只有成功放入该有限队列的完整 block 才推进混音游标。ALSA 传输要求 byte 数可整除 frame_bytes；OSS 允许 byte offset 暂时不对齐，当前 backend 未结束前不切换。写入使用 `snd_pcm_writei()`，EAGAIN 等下次主循环或独立 worker 唤醒，EINTR 重试有截止，EPIPE recover/prepare 后重新预填。ESTRPIPE 时有界尝试 resume，ENOSYS/失败再 prepare；退出 close/drop 在 worker 完成，不从绘制线程同步 drain。

音效保持 WAD 原始 rate/样本，混音用 64 位 phase（32.32）和线性插值，步进为 `(source_rate<<32)/output_rate * pitch_factor`，避免 16.16 游标限制。转换无有符号左移 UB，8-bit unsigned -> `(sample-128)*256`。多路在 int32/int64 accumulator 叠加，输出限制到 S16；资源缓存由 sound 模块 shutdown 释放，只有可复用引用进入缓存。既有 DG_sound_module/music_module 保留公共入口，由 reliefos_audio.c 薄转发。

- [ ] **步骤 4：绿测试与 Doom 构建。** `python3 tools/test_audio_userland.py doom-output doom-mixer`；`make O=out/audio-hda -j8 app-doom`。ASan/UBSan覆盖坏 WAD、长音效、全部输出错误；U6 再做真实波形/游戏验证。

- [ ] **步骤 5：审查背压和时钟所有权。** GUI 不被阻塞、queue 有限、sample/music 时钟按实际提交策略一致。授权后提交 `fix(doom): handle audio backpressure and resampling`。

## 任务 U5：GENMIDI/OPL3 音源与按 sample 排序的音乐事件

**文件：** 创建 music.c/.h、music_opl.c/.h、`doom_music_test.c`；修改 reliefos_audio.c 模块绑定与音源链接；使用 U1 已固定的 OPL3 库。

- [ ] **步骤 1：写 MIDI/MUS/音源确定性测试。** 定义完整最小 MIDI 格式 0 fixture：

```c
static const uint8_t midi_note[] = {
    'M','T','h','d',0,0,0,6, 0,0,0,1,0,96,
    'M','T','r','k',0,0,0,12,
    0,0x90,60,100, 96,0x80,60,0, 0,0xff,0x2f,0
};
```

`MTrk` length 必须与数据一致；该示例实际事件数据为 12 字节。固定 96 PPQN 和默认 500000 us/quarter，note-off 应在 48k 输出的 frame=24000。加入 format1 多轨 tempo、更长VLQ/running-status、tempo event、MUS140Hz delta、channel15->MIDI9、sustain/all-notes-off、pitchbend、program/percussion、双voice GENMIDI、坏 bank和截断曲目。相同曲目一次 render 4096 frames 与分块 render 512x8 的 PCM 必须逐样本一致。

- [ ] **步骤 2：红测试。** `python3 tools/test_audio_userland.py doom-music`；旧“每游戏 tick advance，再生成整块音乐”的实现应在事件位置/块大小不变性测试失败。

- [ ] **步骤 3：实现纯 C parser/scheduler 与 GENMIDI->OPL。** 公开内部接口 `doom_music_register(bytes,length)`、`doom_music_play(song,loop)`、`doom_music_pause(bool)`、`doom_music_set_volume(level)`、`doom_music_render(stereo,frames,rate)`、`doom_music_stop()`、`doom_music_unregister(song)`；register 拒绝不完整/不支持格式并返回 NULL，不能返回无法播放的句柄。限制 32 MIDI tracks、4-byte VLQ、事件和整数乘法溢出；解码自有 bounds-checked cursor，绝不遍历到下一个 lump。format2/SMPTE division 明确拒绝并日志说明；不把它们按 format1/PPQN 错播。

音乐时钟以 frame 为单位，保留整数 remainder；MUS 为 140 Hz，MIDI 每次 tempo 更新重新计算 tick->frame，跨轨全局取最早 event。使用此分块算法，在事件发生前只生成对应 frame：

```c
while (remaining) {
    if (next_event_frame <= cursor) {
        dispatch_events_at(cursor);
        continue;
    }
    uint64_t until_event = next_event_frame - cursor;
    uint32_t chunk = until_event < remaining
                       ? (uint32_t)until_event : remaining;
    OPL3_GenerateStream(&chip, stereo, chunk);
    stereo += chunk * 2; cursor += chunk; remaining -= chunk;
}
```

上段的 `dispatch_events_at(uint64_t)` 在 music.c 定义：取当前全部轨的同 frame 事件、调用 note/controller/tempo handlers、推进各轨下一个 event；所有轨结束时循环 reset 或置 `next_event_frame=UINT64_MAX` 并释放音符，确保不形成零进度循环。单个 frame 最多派发 4096 个事件，超过时拒绝该曲目并给出诊断；delta 时间向上溢出也拒绝，避免畸形曲目长期占据 worker。pause 输出静音并冻结 music cursor，resume 保持位置；stop 发送 key-off/清queue，unregister 不留指向 WAD 的音符引用。

GENMIDI bank 先验证 `#OPL_II#` 8-byte signature 与 `8+175*36` 最小长度，以 little-endian 读取每个 36-byte instrument；头4bytes后有两个16byte voice。读取 flags/fine-tune/fixed-note/operator envelope/waveform/level/feedback/base-note-offset，检查全部表索引；MIDI percussion 35..81 映射 bank128..174，其他 percussion note 不越界。18 OPL3 channels，double-voice 乐器使用两槽，voice stealing 先释放低优先级/最老voice；sustain、controller volume、expression、pan、pitch bend 更新当前音符。寄存器经 `OPL3_WriteRegBuffered()` 写入，初始化 `OPL3_Reset(actual_rate)` 并启用 OPL3。音量由软件/OPL TL 共管，不覆盖系统 master gain。

WAD 无可用 GENMIDI 时保留已注明音色限制的旧合成器 fallback，不把缺资源当声卡失效；正常 Freedoom/原版 WAD 使用 OPL。不随项目提交用户商业 WAD 或提取的音色资产。GENMIDI 结构行为参考 [Chocolate Doom 官方代码](https://github.com/chocolate-doom/chocolate-doom/blob/chocolate-doom-3.1.0/src/i_oplmusic.c)，OPL API 来自 [固定 Nuked OPL3](https://github.com/nukeykt/Nuked-OPL3/tree/765ec962e473aeb767e4cba74ffdc8f588ffbfe8)，如复用 GPL Doom 代码只放进 GPL Doom 程序并保留来源，不进入内核/helper。

- [ ] **步骤 4：绿测试、波形与实际曲目。** `python3 tools/test_audio_userland.py doom-music doom-output doom-mixer`；`make O=out/audio-hda -j8 app-doom`。测试库以相同commit在host编译，保存固定 fixture PCM digest和频率/能量趋势；不以“有非零值”代替节奏/loop/pause测试。U6 使用 Freedoom 菜单/关卡/打击音听感与持续运行记录，精确哈希只用于固定 fixture，真实游戏不要求每次同一波形。

- [ ] **步骤 5：审查音源质量和错误策略。** MIDI/MUS 音符时序、OPL bank、资源生命周期与无声卡模式分别验证。授权后提交 `feat(doom): render sample-timed OPL music`。

## 任务 U6：上游软件、共享音频、GUI、Doom 和硬件矩阵验收

**文件：** 创建 tools/test_audio_e2e_qemu.py、tools/test_audio_userland.py 的wave分析、docs/AUDIO.md、`docs/superpowers/audio-validation/2026-09-30.md`；修改 Make 测试入口将新host suite列入适当测试目标。

- [ ] **步骤 1：写行为验收脚本和可测阈值。** QEMU 启动使用独立 snapshot/输出目录，guest command 通道复用现有VM测试的键盘/串口启动机制，不新增隐藏debug syscall。脚本保留所有命令、exit code、guest日志、WAV、XRUN计数、GUI截图。wave 分析只用Python标准库，例如：

```python
with wave.open(str(path), 'rb') as stream:
    assert stream.getnchannels() == 2
    assert stream.getsampwidth() == 2
    rate = stream.getframerate()
    raw = stream.readframes(stream.getnframes())
samples = struct.unpack('<' + 'h' * (len(raw) // 2), raw)
left = samples[0::2]
crossings = sum(a <= 0 < b for a, b in zip(left, left[1:]))
frequency = crossings * rate / max(1, len(left) - 1)
assert abs(frequency - 1000) <= 10
```

分析窗口去掉头尾的明确启动/停止静音，记录窗口位置，不靠任意选择片段掩盖间断。测试静音/半幅/左右tone、漂移、录音输入相关性、负载XRUN和多客户端共享。

- [ ] **步骤 2：先运行验收确认它能检出缺失。** `python3 tools/test_audio_e2e_qemu.py --image out/audio-hda/images/reliefos.vmdk --output out/audio-hda/qa/e2e --smp 4 --duration 600`；未经 H/A/U 全部实现的镜像应在缺节点/库/工具/声音阶段明确失败。镜像不存在报告不能测试，不自动用旧用户镜像替代。

- [ ] **步骤 3：实现完整场景驱动与报告。** guest 顺序：`aplay -l`、`arecord -l`、`amixer -c 0 contents`、A6 ABI/lifetime probe、`aplay -D hw:0,0` 直接播放、`aplay -D default` 共享播放、两个同时 default 客户端、`arecord -D default`、`speaker-test -D default -c 2`、`soundctl`、设置页、Doom。EAGAIN/EPIPE故障恢复与权限矩阵单独执行。dmix/dsnoop 缺的 mmap/SysV semaphore/shared-memory/timer 行为必须回到 A4/现有相关syscall修复并加专门回归，不以禁用共享功能作为通过。

图形验收在可見 QEMU 窗口实际操作：改变 master音量/静音、输出端口、录音源；外部 amixer和jack事件及时更新。Doom指定 Freedoom WAD，菜单音乐->关卡->连续开火/多个音效，运行600秒，中间60秒CPU/I/O负载；记录正常负载XRUN=0、无持续崩溃/卡顿、恢复后再有音频。无声卡仍可进入游戏，输出明确诊断。wave中硬件静音必须是真的静音而不是只改GUI slider。

硬件矩阵写清 q35/pc、intel-hda/ich9-intel-hda、output/duplex、多codec、MSI on/off、1/4 CPU、no-device、旧AC97/ES1371。录音必须用有输入的音频后端或真实麦克风注入已知信号；仅none/WAV路径标“capture接口，未验证输入声音”。真机至少两个不同codec才称常见真机已验证；未提供设备则报告开放验收项，不能全部勾选。

- [ ] **步骤 4：执行最终门槛。**

```sh
python3 tools/test_hda.py resources lifetime controller codec stream controls
python3 tools/test_linux_audio.py abi pcm device params mmap control oss
python3 tools/test_audio_userland.py controls settings doom-output doom-mixer doom-music
make O=out/audio-hda -j8 kernel drivers userland sdk image-vmdk iso installer
make test
sh tests/build/test-audio-packages.sh
python3 tools/test_audio_e2e_qemu.py --image out/audio-hda/images/reliefos.vmdk \
  --output out/audio-hda/qa/e2e --smp 4 --duration 600
git diff --check
git -C kernel/reliefnt diff --check
```

每项保留实际输出，时间不足或硬件缺失标未运行；只有新变更/失败/未解决问题才扩大重复测试。

- [ ] **步骤 5：按证据交付与授权提交。** docs/AUDIO.md 给标准程序示例、支持列表、errno/权限、默认卡配置与故障诊断；validation文件分别记录host、构建、staging、VM、真机。授权后提交 `test(audio): validate ALSA controls and Doom end to end`，并按两仓合同整合 gitlink。不得把计划完成、编译完成或QEMU出声称为用户全部目标完成。
