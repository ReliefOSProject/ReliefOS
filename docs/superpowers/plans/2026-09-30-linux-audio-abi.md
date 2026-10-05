# Linux ALSA/OSS 音频 ABI 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 subagent-driven-development（推荐）或 executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 使上游 ALSA 程序通过标准 PCM/control/mmap 接口播放、录音和控制声音，并修正 OSS/POSIX 文件行为。

**架构：** PCM 核心统一流状态、frame 计数和硬件 ops；ALSA 与 OSS 是同一核心的协议适配。音频 open-file-description 与 VMA 引用进入现有文件、调度和内存生命周期；所有 UAPI 输入/输出经过已存在的 usercopy 检查。

**技术栈：** x86_64 LP64、Linux v6.14 UAPI、C、Make；host C fixture/ASan/UBSan、musl guest probes。

**规格：** `docs/superpowers/specs/2026-09-30-intel-hda-audio-design.md` 第 4、6、9 节；内部 ops 定义在 `2026-09-30-intel-hda-hardware.md` H1。本计划已获用户批准，实施进度由复选框和验证证据记录。

## 全局约束

- Linux 音频兼容目标是 x86_64 LP64 的 ALSA PCM/control 和 OSS PCM/mixer ABI。
- Linux UAPI 基线固定为 **Linux v6.14**。
- 单次 syscall 拷贝最多 4096 字节，调度前释放执行锁。
- 独立 open 使用独立描述对象；dup/fork/SCM_RIGHTS 共享同一个 open-file-description。
- 设备无 seek 为 `ESPIPE`；未知 ioctl 为 `ENOTTY`。
- 主仓 `AGENT.md` 的无擅自提交/push 规则优先于技能的频繁 commit 建议。

---

## 文件结构

| 创建或修改 | 职责 |
| --- | --- |
| 创建子仓 `include/uapi/sound/asound.h`、`sound/tlv.h` | Linux 导出形式的标准 PCM/control UAPI |
| 修改子仓 `include/uapi/linux/{soundcard,types,time}.h` | 完整 OSS 常量、Linux ABI scalar/time 依赖；保留既有布局 |
| 修改子仓 `configs/header-export.list` 和主仓同名文件 | 唯一导出白名单，各仓构建独立使用 |
| 创建子仓 `kernel/reliefnt/audio/{pcm,device,alsa_pcm,alsa_control,oss,proc}.c` | 分别实现状态/设备/ALSA PCM/控件/OSS/诊断 |
| 修改子仓 `kernel/reliefnt/include/reliefnt/audio.h` | 内部 stream/设备函数声明 |
| 修改子仓 `kernel/reliefnt/{syscall,syscall_device,syscall_mm}.c`、`kernel/reliefnt/include/reliefnt/sched.h`、`kernel/reliefnt/sched/sched.c` | open/read/write/ioctl/poll/epoll/select/seek、VMA 与 fd 生命周期 |
| 修改子仓 `drivers/bootstrap/storage/storage_vfs.c`、`fs/procfs.c`、`kernel/reliefnt/include/reliefnt/storage.h` | 设备目录、stat/权限、proc/asound；已有文件路径已核查 |
| 创建 `tools/tests/{audio_abi_layout,audio_pcm_state,audio_device,audio_alsa_params,audio_mmap,audio_control,audio_oss}_test.c`、`audio_fake_card.h` | 实际核心的 fake card 测试 |
| 创建 `tools/test_linux_audio.py`、`tools/tests/audio_guest_test.c` | host suites 与 guest 标准设备探针 |
| 修改 `tools/test_linux_abi_contract.py`、`tools/test_uapi.py`、`tools/test_header_export.py`、`docs/{ABI,SYSCALLS,DRIVERS}.md` | ABI 与 SDK 回归和实际支持清单 |

实施前检查两仓状态；使用 `O=out/audio-hda`。A1 可独立进行；A2 依赖 H1 的 card ops，使用 fake card 即可；A3–A6 依赖 A2。真实 VM 行为在 H6/U6 门槛执行，不能将 guest probe 编译标成 guest 已通过。

## 任务 A1：固定 Linux 音频 UAPI 与 SDK 布局

**文件：** 创建 sound UAPI 和 `tools/tests/audio_abi_layout_test.c`；修改上述 export/types/time/soundcard 与检查工具；创建 `tools/test_linux_audio.py abi`。

- [ ] **步骤 1：写独立 C/C++ layout 测试。** 使用 Linux v6.14 导出头作为 reference translation unit，与 ReliefNT 导出头的另一个 translation unit 比较。已用一手头在 host x86_64 编译测得以下数值，加入静态断言：

```c
#include <sys/ioctl.h>
#include <sound/asound.h>
_Static_assert(sizeof(struct snd_pcm_hw_params) == 608, "hw params ABI");
_Static_assert(sizeof(struct snd_pcm_sw_params) == 136, "sw params ABI");
_Static_assert(sizeof(struct snd_pcm_status) == 152, "status ABI");
_Static_assert(sizeof(struct snd_pcm_sync_ptr) == 136, "sync ptr ABI");
_Static_assert(sizeof(struct snd_xferi) == 24, "xfer ABI");
_Static_assert(sizeof(struct snd_ctl_elem_info) == 272, "control info ABI");
_Static_assert(sizeof(struct snd_ctl_elem_value) == 1224, "control value ABI");
_Static_assert(sizeof(struct snd_ctl_elem_list) == 80, "control list ABI");
_Static_assert(sizeof(struct snd_ctl_event) == 72, "event ABI");
_Static_assert(SNDRV_PCM_IOCTL_HW_PARAMS == 0xc2604111UL, "HW_PARAMS ioctl");
_Static_assert(SNDRV_PCM_IOCTL_SW_PARAMS == 0xc0884113UL, "SW_PARAMS ioctl");
_Static_assert(SNDRV_CTL_IOCTL_ELEM_READ == 0xc4c85512UL, "ELEM_READ ioctl");
_Static_assert(SNDRV_PCM_IOCTL_WRITEI_FRAMES == 0x40184150UL, "WRITEI ioctl");
```

比较全部 PCM/control/OSS ioctl 数值、enum、sizeof/alignof/关键 offsetof，包括 64-bit timestamp 变体。测试不依赖宿主随发行版变化的 `/usr/include/sound` 版本。

- [ ] **步骤 2：红测试。** `python3 tools/test_linux_audio.py abi`，预期当前缺少 `<sound/asound.h>` 而失败。

- [ ] **步骤 3：导入有来源的 UAPI 并解决独立 include。** 原始一手文件与已核查 SHA-256：

```text
https://raw.githubusercontent.com/torvalds/linux/v6.14/include/uapi/sound/asound.h
ec647abccd554aab1ebbf77af42efac11748384f027870eb90cba974b6bcda0a
https://raw.githubusercontent.com/torvalds/linux/v6.14/include/uapi/sound/tlv.h
74b97d2cae70e67fd7bc5750dd46e0b9976d390fb817d98630df6ba9c2d33d61
```

使用 headers_install 等价处理移除 `__user`，将 `__packed` 保留为 packed 属性。用户可见头独立 include `linux/ioctl.h`、types/time，并在本地 linux/types.h 补 ABI 相同的 `__le16/32/64`、`__be16/32/64`、kernel scalar 所需类型；不要导入完整 Linux kernel 编译依赖。保持 x86_64 `snd_pcm_[su]frames_t` 为 64 位，timespec 的布局与 native Linux 一致。OSS 取同版本完整 soundcard 头，按相同导出语义处理。源文件前记录 URL、版本、SHA 和 SPDX；参考导出形态置于 `tests/fixtures/audio-uapi/linux-v6.14/`，仅测试使用。构建不下载这些文件。

- [ ] **步骤 4：绿测试与真实导出。** `python3 tools/test_linux_audio.py abi`；`python3 tools/test_uapi.py`；`python3 tools/test_header_export.py`；`make O=out/audio-hda headers runtime sdk`。Linux 头 C/C++ standalone、SDK 安装后的调用编译都通过；旧 ABI layout 不变。

- [ ] **步骤 5：审查来源与边界。** diff 不包含 Linux 内核驱动代码。授权后子仓提交 `feat(audio): publish Linux ALSA and OSS UAPI`，主仓测试/export 修改独立提交；未授权不提交。

## 任务 A2：以 frame 为单位的 PCM 状态机和 fake card

**文件：** 创建 `audio/pcm.c`、`tools/tests/audio_pcm_state_test.c`、`audio_fake_card.h`；扩展 audio.h 和 core.c 事件入口。

- [ ] **步骤 1：写状态转换、短传输与 underrun 失败测试。** fake card 在共享测试头定义 identity、H1 ops、`audio_test_card_init(c)`、`audio_test_advance(c,frames,error)`；prepare 记录 params/DMA，trigger 记录 start/stop/pause，不模拟 PCM 状态机。定义测试卡为 48k/stereo/S16、buffer 2048、period 512，可设置当前硬件 frame。使用核心接口：

```c
struct audio_pcm *audio_pcm_open(uint32_t card, uint32_t device,
                                enum audio_direction direction, int *error);
int audio_pcm_hw_params(struct audio_pcm *, const struct audio_params *);
int audio_pcm_prepare(struct audio_pcm *);
int audio_pcm_start(struct audio_pcm *);
int audio_pcm_drop(struct audio_pcm *);
int audio_pcm_pause(struct audio_pcm *, bool);
long audio_pcm_transfer(struct audio_pcm *, void *kernel_data, uint32_t frames);
void audio_pcm_release(struct audio_pcm *);
```

```c
struct audio_test_card c; audio_test_card_init(&c);
int error; struct audio_pcm *p = audio_pcm_open(c.id, 0, AUDIO_PLAYBACK, &error);
assert(p && !error);
assert(audio_pcm_start(p) == -EBADFD);
struct audio_params params = {48000, 2, 16, 4, 512, 2048};
assert(!audio_pcm_hw_params(p, &params) && !audio_pcm_prepare(p));
int16_t samples[4096] = {0};
assert(audio_pcm_transfer(p, samples, 2048) == 2048);
assert(audio_pcm_transfer(p, samples, 1) == -EAGAIN);
assert(!audio_pcm_start(p)); audio_test_advance(&c, 512, 0);
assert(audio_pcm_transfer(p, samples, 512) == 512);
audio_test_advance(&c, 4096, -EPIPE);
assert(audio_pcm_transfer(p, samples, 1) == -EPIPE);
assert(!audio_pcm_prepare(p)); audio_pcm_release(p);
```

另验 capture 空/满/overrun、drain 完成、duplicate start、pause/continue、独占、LINK group 同步触发、disconnect 后 ENODEV、frame 乘法溢出、boundary 多圈。

- [ ] **步骤 2：红测试。** `python3 tools/test_linux_audio.py pcm`；缺核心函数失败。红阶段分别运行 playback/capture/state/lifetime 测试名。

- [ ] **步骤 3：实现 PCM 内部结构和状态。** `audio_pcm` 保存 card 引用、HW stream、Linux state、params、DMA、lock、hw/appl/boundary frame、avail_min/start/stop thresholds、generation、映射/文件引用、linked group、wait sequence。申请 buffer 前验证 `frame_bytes=channels*container_bytes` 与 period/buffer 整除、大小/能力；实际数据先进入 kernel ring。ring 可用数量的核心为：

```c
uint64_t queued = (p->appl_ptr + p->boundary - p->hw_ptr) % p->boundary;
if (queued > p->params.buffer_frames) return -EPIPE;
uint32_t room = p->params.buffer_frames - (uint32_t)queued;
uint32_t take = frames < room ? frames : room;
if (!take) return -EAGAIN;
```

capture 使用反方向差值；不将未产生样本算作可读。copy 分段绕 ring，提交指针最后发布。buffer 初始置零，XRUN 停止硬件，prepare 重置 pointer/错误；drain 状态等待 queued=0 才变 SETUP。period event 与 wake sequence 在 lock 内更新，通知在锁外完成。LINK/UNLINK 按 PCM fd 引用并规定统一锁序；HDA 支持 SSYNC 时 group START 使用 controller 同步，其他硬件不宣传 SYNC_START。

- [ ] **步骤 4：绿测试。** `python3 tools/test_linux_audio.py pcm`；`python3 tools/test_hda.py stream lifetime`。ASan/UBSan、资源引用归零，playback/capture 都经过真实核心。

- [ ] **步骤 5：审查 Linux 状态与单位。** 核对 frame、字节、boundary 和 Linux 枚举，不能用 present/active 两布尔取代状态机。授权后提交 `feat(audio): implement PCM state and buffering`。

## 任务 A3：标准设备节点、open-file-description 与 POSIX 等待

**文件：** 创建 audio/device.c、proc.c、`audio_device_test.c`；修改 storage、sched.h/sched.c、syscall.c/device.c 与读写/select/epoll/seek 分派。

- [ ] **步骤 1：写设备与 descriptor 回归。** host fixture 使用真实 `task_file_description()`、close/dup/fork 文件路径；fake card 为 A2 头。Linux syscall probe 断言：

```c
int fd = open("/dev/snd/pcmC0D0p", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
assert(fd >= 0);
int other = dup(fd); assert(other >= 0);
assert(fcntl(fd, F_SETFL, 0) == 0);
assert(!(fcntl(other, F_GETFL) & O_NONBLOCK));
errno = 0; assert(lseek(fd, 0, SEEK_SET) == -1 && errno == ESPIPE);
assert(close(fd) == 0);
assert(ioctl(other, SNDRV_PCM_IOCTL_PVERSION, &(int){0}) == 0);
assert(close(other) == 0);
```

测试完整 card/dev 编码、getdents/stat major/minor、录音权限、zero count、错误访问模式、O_NONBLOCK 背压、short transfer、阻塞中信号、有进度后信号返回数量、SCM_RIGHTS 引用、进程 exit、driver disconnect、poll/epoll/select 就绪一致。

- [ ] **步骤 2：红测试。** `python3 tools/test_linux_audio.py device`；初始节点/open 分派缺失。

- [ ] **步骤 3：集成标准音频设备和等待。** `audio_device_open(task,file)` 在 open 成功发布 descriptor 前获取 core 引用；`audio_device_close(file)` 在最后 description release 时调用；`audio_device_read/write(task,file,address,count)` 每次复制 <=4096 bytes；`audio_device_ioctl(task,file,request,arg)`、`audio_device_poll(file)`、`audio_device_seek(file)` 是协议钩子。description 追加专用 `audio_file *`，不能占用其他设备的 aux；dup/fork/SCM_RIGHTS 自动继承 description 引用，open 新建对象。ioctl 重定向到 A4/A5。

节点名解析十进制 N/M，溢出和尾随垃圾拒绝；major 116，control minor `N*32`、playback `N*32+16+M`、capture `N*32+24+M`，M 为 0..7；超过该范围用 Linux 扩展 minor 分配合同或拒绝注册该设备，禁止碰撞。OSS major 14，dsp minor `N*16+3`、mixer `N*16`。节点存在和权限来源于卡注册表及现有 DAC，设备写不会创建常规磁盘文件。`/proc/asound` 的 read/getdents 与 registry snapshot 一致。

阻塞通过现有 syscall retry state 保存未提交的进度/stream 引用，以 wait sequence 防止丢唤醒；无可用空间时释放执行锁再 park，非阻塞返回 EAGAIN。调度 epilogue 的内部 EAGAIN 与用户态 EAGAIN 分开，不能自动重试 O_NONBLOCK；signal 唤醒遵守 SA_RESTART 和进度返回。poll readiness 使用 avail_min，XRUN 为 POLLERR，断开为 ERR/HUP；select/epoll 复用同一 mask。非法用户范围在实际传输前验证；完成数据量大于 0 时返回该数量。

- [ ] **步骤 4：绿测试与现有 fd 回归。** `python3 tools/test_linux_audio.py device pcm`；`python3 tools/test_linux_descriptors.py`；`python3 tools/test_linux_ioctl_cloexec.py`；`python3 tools/test_linux_permissions.py`。所有 close/exit/disconnect 路径无泄漏；输出页检查使用 writable。

- [ ] **步骤 5：审查信号与 DAC。** 录音拒绝路径、EINTR、SA_RESTART、SCM_RIGHTS 与动态 minor 均有证据。授权后提交 `feat(audio): expose Linux sound devices and file semantics`。

## 任务 A4：ALSA 参数协商、PCM ioctl 与 mmap

**文件：** 创建 audio/alsa_pcm.c、`audio_alsa_params_test.c`、`audio_mmap_test.c`；修改 syscall_mm.c、VMA 结构、VMA copy/release/mprotect/fault 路径与 audio.h。

- [ ] **步骤 1：写约束和用户内存失败测试。** 根据 Linux mask/interval 写约束 case，假卡只允许 S16_LE、RW/MMAP_INTERLEAVED、48k、2 channels。测试不支持格式会被从 mask 删除，period/buffer 关联传播，空交集返回 EINVAL：

```c
int audio_alsa_refine(struct audio_pcm *, struct snd_pcm_hw_params *);
struct snd_pcm_hw_params h;
memset(&h, 0, sizeof(h));
/* 测试头定义 audio_test_hw_any(): mask 全 1、所有 interval 为 [0,UINT_MAX]。 */
audio_test_hw_any(&h);
assert(!audio_alsa_refine(p, &h));
assert(h.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min == 48000);
assert(h.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max == 48000);
```

`audio_test_hw_any()` 在 A2 测试头添加真实初始化循环，不能从实现复制 refine：

```c
for (unsigned i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; ++i)
    for (unsigned w = 0; w < 8; ++w) h->masks[i].bits[w] = UINT32_MAX;
for (unsigned i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; ++i)
    h->intervals[i] = (struct snd_interval){ .min = 0, .max = UINT32_MAX };
h->rmask = UINT32_MAX;
```

mmap case 包括 data/status/control offsets、status PROT_WRITE 拒绝、capture data 允许 PROT_READ|PROT_WRITE（共享映射权限仍受文件访问模式约束，缺 PROT_READ 为 EINVAL）、overflow、只读/COW output、跨页 nested pointer、SYNC_PTR invalid appl_ptr、close 后 mapping 存活。存在 data mmap 时 HW_FREE 和非 OSS HW_PARAMS 必须返回 EBADFD；fork/unmap/forced unload 无 UAF。加入 PVERSION=0x020012、USER_PVERSION、TSTAMP/TTSTAMP、RESET、XRUN 的状态和非法输入测试；LINK 的参数是直接 fd 值，不当作用户指针解引用。

- [ ] **步骤 2：红测试。** `python3 tools/test_linux_audio.py params mmap`；初始没有 ALSA adapter 和 VMA ownership。

- [ ] **步骤 3：实现 fixed-point constraints、ioctl 和 VMA pin。** hw_refine 先与硬件 rate/format/channels/access 求交，再传播 sample_bits/frame_bits/period_size/period_bytes/periods/buffer_size/buffer_bytes，任何计算使用 64 位检查溢出；仅 integer interval 可成为 frame。改变的参数置 cmask，迭代到无变化。HW_PARAMS 要求最终参数单值且满足 H2/H4 编码，成功后在 hw_params 中回传精确字段；HW_FREE 真释放硬件配置。

按规格列出所有 PCM ioctl 分派并调用 A2 状态机；每个 PCM 文件保存 USER_PVERSION，TSTAMP 保持 Linux 兼容行为，TTSTAMP 校验 realtime/monotonic/monotonic-raw 类型并调用对应时钟。RESET 重置报告指针和统计，不能等同于 DROP；XRUN 强制进入 XRUN/停止硬件。LINK 将 ioctl arg 直接解释为目标 fd，通过 description 表取得 stream，不做用户指针读取。处理 READI/WRITEI 的 nested buffer 后在 `snd_xferi.result` 写 frame 数，ioctl 自身的返回和结构 result 分开。STATUS/DELAY 使用真实 hw/appl 与所选 timestamp。REWIND/FORWARD 限制合法已提交/可提交 frame，越界只返回实际移动数量；未支持 access 不广告。unknown request ENOTTY，已知且状态不允许 EBADFD。

VMA 追加 device mapping object（stream 引用、kind、generation、page list），使用 Linux UAPI 中的 data、OLD/NEW status/control offsets。data 页面直接映射核心的 DMA ring；status 只读、control 可读写。Linux header 的 `snd_pcm_mmap_status/control` 按原布局发布。SYNC_PTR 根据 HWSYNC/APPL/AVAIL flags 决定读回或提交，boundary 校验拒绝非法跳变；实现 memory-order 的 acquire/release。映射/fork 持有引用，unmap/exit 减引用；Linux v6.14 中存在 data mmap 的 HW_FREE/HW_PARAMS 为 EBADFD，不能释放其 DMA buffer。HW_FREE 允许的 SETUP/PREPARED 与 HW_PARAMS 允许的 OPEN/SETUP/PREPARED 状态逐个测试。disconnect 停止 DMA、发布断开状态，保留 fd/VMA 所持页面直到最后引用释放；mprotect 不能突破 max_prot。mapper 不暴露 MMIO、BDL 或内核对象。实际 Linux 参考行为存在差异时修正到同版本参考，不能用方便的内部语义替代。

- [ ] **步骤 4：绿测试与 MM 回归。** `python3 tools/test_linux_audio.py params mmap pcm device`；`python3 tools/test_linux_memory.py`；`make O=out/audio-hda -j8 kernel runtime sdk`。比较 OLD/NEW timestamp 布局与 alsa-lib 1.2.14 调用序列；U1/U6 的 upstream runtime 是本任务最终兼容门槛。

- [ ] **步骤 5：审查能力诚实性。** MMAP、PAUSE、SYNC_START flags 只有实际支持才置位；未知输入和失败 copy-out 不留已提交部分配置。授权后提交 `feat(audio): implement ALSA PCM negotiation and mapping`。

## 任务 A5：ALSA control/events/TLV 与完整 OSS 适配

**文件：** 创建 audio/alsa_control.c、oss.c、`audio_control_test.c`、`audio_oss_test.c`；修改 device/proc audio.h；旧 OSS syscall 实现移入 oss.c。

- [ ] **步骤 1：写共享控制与真实 queue 失败测试。** fake card 的两个 volume 和一个 route enum 返回真实 caps。走协议函数 `audio_alsa_control_ioctl(file,request,arg)` 与 `audio_oss_ioctl(file,request,arg)`，测试 ALSA 设置、OSS 读取同一 gain；订阅收到 VALUE 事件且 POLLIN 与可读事件一致。至少包含：

```c
static uint8_t percent_to_gain(unsigned percent, uint8_t max_gain) {
    return (uint8_t)((percent * max_gain + 50u) / 100u);
}
assert(percent_to_gain(0, 40) == 0);
assert(percent_to_gain(50, 40) == 20);
assert(percent_to_gain(100, 40) == 40);
```

分别验证 ELEM_LIST 空/截断/offset、非法 numid/类型/enum、只读 jack、TLV capacity、ioctl nested 指针、订阅开关、同控件事件合并、音量超界。ELEM_LOCK/UNLOCK 测试另一独立 open 的写入为 EPERM、重复 lock 为 EBUSY、未锁定 unlock 为 EINVAL、非 owner unlock 为 EPERM；dup/fork 共享 owner，最后 description 关闭释放锁。对固定控件集中的每个 numid 连续发布 VALUE/INFO，要求待处理项数最多为控件总数且 mask 保留。OSS 验 SYNC 未 drain 前不成功、RESET 清 queue、fragment 协商真实改变、GETISPACE/OSPACE/PTR 单位、SETTRIGGER、O_RDWR duplex、未知 ioctl。

- [ ] **步骤 2：红测试。** `python3 tools/test_linux_audio.py control oss`；当前无 /dev/mixer，SYNC/RESET no-op 被明确测试检出。

- [ ] **步骤 3：实现两个协议的 adapter。** control ioctls 实现 PVERSION/CARD_INFO、PCM_NEXT_DEVICE/INFO/PREFER_SUBDEVICE、ELEM_LIST/INFO/READ/WRITE/LOCK/UNLOCK、SUBSCRIBE_EVENTS、TLV_READ；type/count/name/enum 来自 H1 control info。value union 仅访问协商类型对应成员，checked copy-out；change mask 使用 Linux VALUE/INFO/ADD/REMOVE 位。卡注册后控件集固定，每打开的 control fd 预分配覆盖全部 numid 的 pending mask 数组，按同 element 合并事件，避免固定小 FIFO 溢出；card 断开走 ERR/HUP 而不发明私有事件。TLV DB_SCALE payload 用 H5 的 hundredths-dB 值，length 按字节。控件锁 owner 为 open-file-description，INFO 回传 LOCK/OWNER/owner PID；被锁定控件仅 owner 可以 WRITE，description 最后关闭时释放其锁。参考 [Linux v6.14 control 行为](https://github.com/torvalds/linux/blob/v6.14/sound/core/control.c)，不存在 element 为 ENOENT；所有锁查找和状态修改在同一同步域执行。用户定义控件/hwdep/rawmidi 不在本轮，已知不支持返回 Linux 参考 errno 并写支持清单。

OSS dsp 以 card 的首个可用 PCM 打开 A2；音频 alias 保留既有兼容行为，文档不声称标准 `/dev/audio` μ-law 默认。实现 RESET/SYNC/SPEED/SETFMT/STEREO/CHANNELS/GETFMTS/GETBLKSIZE/SETFRAGMENT/GETxSPACE/GETxPTR/GETODELAY/NONBLOCK/GETCAPS/GETTRIGGER/SETTRIGGER/SETDUPLEX，全部由实际 caps/queue/state 计算。SETFMT/CHANNELS/SPEED 采用 Linux OSS 协商回写；query 不重配置。OSS byte stream 在每个 description 保存不足一 frame 的有界尾部，read/write 按接受的字节数返回，完整 frame 才交给 PCM 核心；SYNC/RESET 的尾部处理与 Linux OSS 参考核对。GETPTR `blocks` 是上次读取以来完成 fragment 数。mixer DEVMASK/RECMASK/STEREODEVS/CAPS/RECSRC 与 READ/WRITE_VOLUME/PCM/SPEAKER/MIC/LINE 从控件产生，百分比 0..100，两声道分开；不支持的 channel 不出现在 mask。普通 stop/drain/drop 进入同一 state，不调用假成功。

- [ ] **步骤 4：绿测试。** `python3 tools/test_linux_audio.py control oss device pcm abi`；`python3 tools/test_hda.py controls`。标准控件与 OSS 互相可见，失败写不发布成功事件，fragment/capture/pointer 与真实状态一致。

- [ ] **步骤 5：审查 Linux 参考和旧用户程序。** unknown 与 unsupported errno 分开；`/dev/dsp` 旧程序仍使用 musl write/ioctl；授权后提交 `feat(audio): add ALSA controls and OSS compatibility`。

## 任务 A6：Linux 差分、guest probes 与支持清单

**文件：** 创建 `tools/tests/audio_guest_test.c`；扩展 tools/test_linux_audio.py；修改 ABI/SYSCALLS/DRIVERS 文档和 ABI contract 检查。

- [ ] **步骤 1：写可在 Linux 和 ReliefOS 同源运行的 probe。** binary 接收 `--card N --mode abi|play|capture|lifetime|controls`，标准 libc 入口，不调用私有 reliefos syscall。abi mode 打印 ioctl/sizeof/offset；play mode 通过 OSS 和直接 ALSA WRITEI 各播 1 kHz 左/2 kHz 右 2 秒；capture mode 固定 frame 数并保存 raw PCM；controls mode 先保存原值、做增益/静音/路由测试，所有退出路径恢复。错误日志保留 operation/errno，不输出 DMA 地址。

```c
static int write_all_nonblocking(int fd, const void *bytes, size_t length) {
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, (const char *)bytes + done, length - done);
        if (n > 0) { done += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            struct pollfd p = {fd, POLLOUT, 0};
            if (poll(&p, 1, 1000) > 0 && !(p.revents & (POLLERR | POLLHUP))) continue;
        }
        return -1;
    }
    return 0;
}
```

probe 设整体 10 秒截止，零返回不得无限循环。Linux 参考无可用声卡时只运行 abi/失败参数，不把跳过 I/O 标成通过；I/O 差分可在独立 Linux VM 中使用 snd-dummy。

- [ ] **步骤 2：红测试。** `python3 tools/test_linux_audio.py abi pcm device params mmap control oss`；新增 guest mode 的构建测试先因入口不存在失败。H6 前不运行真实 hardware guest，只完成 host 与 probe 编译。

- [ ] **步骤 3：实现 probe runner 与行为 manifest。** runner 编译 host 和 musl guest 二进制，使用 `--build-guest --output out/audio-hda/qa/audio-guest`；读取固定参考输出比较，不要求宿主录音权限。支持清单列每个 ioctl 的 supported/access/state/errno/mmap 条件，覆盖 known unsupported ioctl 和多卡/断开/权限。文档删除当前 OSS no-op 的已失效描述，仅在实际验证后更新“实现”栏；VM 与真机栏独立。

- [ ] **步骤 4：运行全套 host 和 guest 编译门槛。**

```sh
python3 tools/test_linux_audio.py abi pcm device params mmap control oss
python3 tools/test_linux_audio.py --build-guest --output out/audio-hda/qa/audio-guest
python3 tools/test_uapi.py
python3 tools/test_header_export.py
python3 tools/test_linux_abi_contract.py
make O=out/audio-hda -j8 kernel runtime sdk
```

H6/U6 启动 guest 后再执行各 mode，将波形、运行日志、errno 与 Linux reference 区分。任何失败回到对应 A 任务修复，再只重跑其受影响回归。

- [ ] **步骤 5：验证并记录合同覆盖。** 所有规格第 6 节 requirement 对应测试/证据路径；未实现非交错/rawmidi 不广告。授权后提交 `test(audio): cover Linux sound ABI and guest behavior`。只有 U1/U6 上游 ALSA 软件实际运行后，才对用户称“已通过 ALSA 应用兼容验收”。

## 任务 A7： 共享 PCM 所需的原生 SysV shared memory

**前置：** A1 的 canonical UAPI、A3/A4 的地址空间/VMA引用合同、现有 SysV semaphore/futex；在 A5 后、A6 前串行实施。编号19保留既有任务编号稳定，实际顺序见账本。

**已核验缺口：** `shm.c` 是 `/dev/shm0`/memfd backing；内核没有 `shmget/shmat/shmdt/shmctl` Linux syscall dispatch。alsa-lib 1.2.14 `src/pcm/pcm_direct.c` 实际调用这些接口；已有 private shared-memory device 不满足 SysV key/ID/DAC/IPC_STAT/IPC_RMID 合同。用户已授权 Linux ABI 及共享播放/录音；此任务补齐必要前置，不改变上游插件。

**文件：** 创建子仓 `kernel/reliefnt/syscall_sysv_shm.c` 和必要 private header；修改实际 syscall分派、VMA与fork/exec/exit lifecycle hooks；导入 Linux v6.14 canonical `linux/shm.h` /目标x86_64 `asm/shmbuf.h`及实际导出依赖，联动header-export/SDK/ABI文档；创建 `tools/tests/sysv_shm_test.c`、`sysv_shm_abi_test.c` 与非生产 runner。优先复用 A4 已完成的 VMA backing 引用接口；不要复制 syscall_mm 或整个内存管理器。

- [ ] **步骤1：写真实核心与同源 native ABI RED。** host fixture 链接生产 SysV SHM、page/VMA lifecycle 函数，仅替宿主硬件/用户地址/SMP primitive。相同 musl/raw-syscall probe 在 Linux reference 和 guest 使用。覆盖 IPC_PRIVATE、key重复连接、IPC_CREAT/EXCL、零size查找、size溢出/资源失败回滚、ID失效与generation、实际零页和两个独立进程共享数据、DAC/owner/capability、IPC_STAT/SET/RMID与nattch。native probe核对canonical `shmid_ds`、`shminfo`、`shm_info` 的x86_64布局和全部实现命令/flags/errno，不手工猜布局。覆盖SHM_RDONLY后mprotect WRITE拒绝、fork共享与CLONE_VM、exec/exit、partial munmap/mprotect splits、shmdt移除同一attachment的剩余VMAs、RMID延迟释放/已删除ID attach的Linux行为，以及partial failure原子回滚；测试last detach实际归还页，不能只清ledger。

- [ ] **步骤2：保存失败证据。** 在实现前运行新增聚焦runner及Linux reference probe。记录生产缺失/未分派的具体RED、命令和输出；reference通过仅证明测试参照有效，不证明guest已实现。

- [ ] **步骤3：实现 Linux 原生接口及持有关系。** 使用canonical `__NR_shmget/__NR_shmat/__NR_shmdt/__NR_shmctl`，返回原生负errno；x86_64 raw shmat返回用户地址。独立 SysV segment registry 保存key、ID/generation、ipc_perm、logical bytes/page backing、atime/dtime/ctime、cpid/lpid、attachment counts/deleted状态；finite resource limits必须可诊断，长度/roundup/count全防溢出、全页先清零。segment pages由registry和attachment共同持有：RMID删除key可见性并按Linux规则保留ID/页至最终attachment消失，不能随fd关闭释放。fork新MM保留共享物理页并增加真实attachment；CLONE_VM共用同一地址空间；exec/exit/partial unmap按最终VMA backing引用减少一次attachment。shmdt按original attach identity移除剩余split VMAs，不影响相邻映射；SHM_RDONLY约束max_prot。IPC_STAT/SET、IPC_INFO、SHM_INFO、SHM_STAT/SHM_STAT_ANY与SHM_LOCK/UNLOCK按Linux v6.14/reference的DAC、RLIMIT_MEMLOCK/capability和宽度实现；合法但平台未提供的特性只能按可验证的Linux拒绝语义处理，未知cmd EINVAL。所有用户copy/range/readonly/COW/nested长度先检查，失败无部分发表。必要/proc或限额报告应来自真实registry。独立模块文件承接协议，顶层仅分派钩子。

- [ ] **步骤4：聚焦GREEN与集成验证。** 运行SysV SHM host+reference，已有sem/futex/VM/fork相关受影响回归，canonical UAPI/header-export/ABI checks，普通root Make kernel/runtime/sdk；报告准确日志和结果。A6/H6 guest就绪后执行相同SHM probe并保存实际用户进程共享/生命周期证据。U1/U6仍必须用未修改上游dmix/dsnoop多client运行，不得靠关闭共享插件、替换private shm或fake success通过。

- [ ] **步骤5：规格与质量独立审查。** 审查真实page持有/VMAsplit/RMID/fork/exec/exit、DAC、canonical ABI和错误原子性；全部闭合后才标记本任务完成。无Git提交授权，保留reviewable diff。Linux reference/host/build和guest证据分别记录，guest未运行不能宣称Linux应用验收通过。

**一手来源：** Linux v6.14 UAPI和原生参考行为；https://github.com/alsa-project/alsa-lib/blob/v1.2.14/src/pcm/pcm_direct.c 。不得复制 GPL 内核实现。
