# Intel HDA 硬件与音频核心实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 subagent-driven-development（推荐）或 executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 提供可枚举、可恢复、支持播放/录音/控件的通用传统 HDA 驱动，并保持既有模块兼容。

**架构：** HDA 模块分离 controller、codec graph、stream 与 controls；内核 audio core 持有卡、流和模块引用。模块通过追加的 kernel API 使用 MMIO、受 DMA mask 限制的分配、中断与事件回调；v1 模块前缀不变。

**技术栈：** x86_64 C/汇编、PCI、APIC、HDA Rev. 1.0a、GNU Make/Shell；非生产 host 测试使用 Python + C + ASan/UBSan。

**规格：** `docs/superpowers/specs/2026-09-30-intel-hda-audio-design.md`，尤其第 4、5、9 节；用户已于 2026-09-30 批准，正在隔离 worktree 中逐任务实施。

## 全局约束

- 传统 HDA 控制器连接的模拟 codec 是本轮强制目标。
- v1 `RELIEFOS_DRIVER_ABI_VERSION` 和现有四个音频 ops 的布局保持不变。
- 单次 syscall 拷贝最多 4096 字节，调度前释放执行锁。
- 默认 48 kHz/stereo/S16 的交互播放采用 512-frame period、4-period buffer。
- 主仓 `AGENT.md` 的无擅自提交/push 规则优先于技能的频繁 commit 建议。
- 每个 kernel 函数声明/定义包含 Doxygen 注释；本计划所有 `kernel/reliefnt/...` 路径属于子仓。

---

## 文件结构与执行准备

| 创建或修改 | 职责 |
| --- | --- |
| 创建 `kernel/reliefnt/include/reliefos/driver_audio.h` | 带尺寸的模块内部 card/PCM/control 合同，不导出用户 SDK |
| 修改 `kernel/reliefnt/include/reliefos/driver.h`、`kernel/reliefnt/kernel/reliefnt/driver_manager.c` | 追加平台服务与 v2 card 注册，保留 v1 |
| 创建 `kernel/reliefnt/kernel/reliefnt/audio/core.c`、`kernel/reliefnt/kernel/reliefnt/include/reliefnt/audio.h` | 多卡注册、模块引用、事件和注销 |
| 创建 `kernel/reliefnt/arch/x86_64/pci_irq.c`、`kernel/reliefnt/kernel/reliefnt/include/reliefnt/pci_irq.h` | MSI 向量分配、使能、同步注销；不猜测 INTx GSI |
| 修改子仓 `arch/x86_64/{pci,irq,idt}.c`、`arch/x86_64/boot.S`、`mm/mm.c`、`kernel/reliefnt/include/reliefnt/{mm,pci}.h` | PCI 枚举、真实向量 stub、DMA mask 分配；已核查 `mm/mm.c` 包含 `mm_alloc_pages()` |
| 修改子仓 `kernel/reliefnt/time.c`、`kernel/reliefnt/include/reliefnt/time.h` 与 audio core service | 100 Hz tick 下的有界降级服务与待处理 codec/jack 工作；每次仅处理固定数量事件 |
| 创建子仓 `drivers/hda/{hda.c,hda.h,controller.c,codec.c,stream.c,controls.c}` | 模块生命周期、寄存器/命令、拓扑、DMA、mixer/jack |
| 修改子仓 `mk/boot.mk`、`Makefile`，主仓 `mk/{kernel,boot,run}.mk` | 模块 install/publish、QEMU 音频选择 |
| 创建 `tools/tests/{pci_audio_resources,hda_controller,hda_codec,hda_stream,hda_controls,audio_lifetime}_test.c` 与 `tools/test_hda.py` | 生产函数的 fake hardware 回归 |
| 创建 `tests/build/test-hda-products.sh`、`tools/test_hda_qemu.py` | 产物成员与实际 VM 音频证明 |

实施前读主仓 `AGENT.md`、规格和三个计划。检查 `git status --short` 与子仓状态，不自动提交、更新/回滚 gitlink 或重建现有镜像。在已附加隔离 worktree 中执行，子仓若未初始化，先复用已初始化的源树作只读参照，再按用户授权初始化其工作副本。使用独立 `O=out/audio-hda`。每任务验证后 `git diff --check`（两个仓分别执行）。

## 任务 H1：有所有权的音频卡与安全平台资源

**文件：** 创建 `driver_audio.h`、`audio/core.c`、`audio.h`、`pci_irq.c`、`pci_irq.h`、`tools/tests/pci_audio_resources_test.c`、`audio_lifetime_test.c`、`tools/test_hda.py`；修改上表的 driver API、PCI、IRQ/IDT/stubs、MM。

- [x] **步骤 1：写资源与 v1 兼容失败测试。** 在 host fixture 中包含实际 `pci_irq.c` 和 card core，stub 只替换 MMIO/端口/页分配/屏障；fixture 的 `read_cfg/write_cfg/map/unmap/alloc/free` 分别记录寄存器数组与资源账本。使用如下断言，并定义 `mm_fixture_init()` 给页分配器提供低/高两个空闲区：

```c
static void dma_mask_is_enforced(void) {
    mm_fixture_init(0x100000, 32, 0x100000000ULL, 32);
    uint64_t p = mm_alloc_pages_below(2, 0xffffffffULL);
    assert(p && p + 8191 <= 0xffffffffULL);
    mm_free_pages(p, 2);
}
static void module_v1_prefix_is_unchanged(void) {
    _Static_assert(offsetof(struct reliefos_driver_kernel_api,
                           register_audio) == 160, "v1 prefix changed");
    _Static_assert(offsetof(struct reliefos_driver_kernel_api,
                           pci_enumerate) == 168, "extension must follow v1");
    _Static_assert(sizeof(struct reliefos_driver_audio_ops) == 32,
                   "v1 ops changed");
}
```

测试含：PCI capability 环终止、64 位 MSI 地址、MSI disable 后 ISR 同步、不可分配低 DMA 页为 ENOMEM、注册失败无泄漏、活动流普通卸载为 EBUSY、forced disconnect 不进入已卸载回调。

- [x] **步骤 2：运行红测试。** `python3 tools/test_hda.py resources lifetime`；初始因新 API/生产文件缺失失败，记录失败测试名。runner 逐 suite 编译 C11、ASan/UBSan、10 秒 timeout，不能仅扫描文本。

- [x] **步骤 3：实现内部合同与资源服务。** 在 `driver_audio.h` 定义以下类型和所有 ops；只有模块 API 使用这些类型：

```c
enum audio_direction { AUDIO_PLAYBACK, AUDIO_CAPTURE };
enum audio_trigger { AUDIO_START, AUDIO_STOP, AUDIO_PAUSE, AUDIO_UNPAUSE };
struct audio_caps {
    uint64_t formats, rates;
    uint32_t channels_min, channels_max;
    uint32_t period_bytes_min, period_bytes_max, buffer_bytes_max;
};
struct audio_params {
    uint32_t rate, channels, sample_bits, frame_bytes;
    uint32_t period_frames, buffer_frames;
};
struct audio_dma { void *kernel; uint64_t bus; uint32_t bytes; };
struct audio_hw_stream { uint32_t id; void *driver; };
struct audio_control_info {
    uint32_t id, type, count, access;
    int64_t min, max, step;
    int32_t db_min, db_step;
    char name[44];
    uint32_t items;
    char item_names[16][32];
};
struct audio_control_value { int64_t values[16]; };
struct audio_card_ops {
    uint32_t version, size;
    int (*pcm_caps)(void *, uint32_t, enum audio_direction, struct audio_caps *);
    int (*open)(void *, uint32_t, enum audio_direction, struct audio_hw_stream *);
    int (*prepare)(void *, struct audio_hw_stream *, const struct audio_params *,
                   const struct audio_dma *);
    int (*trigger)(void *, struct audio_hw_stream *, enum audio_trigger);
    int (*pointer)(void *, struct audio_hw_stream *, uint64_t *frames);
    void (*close)(void *, struct audio_hw_stream *);
    uint32_t (*control_count)(void *);
    int (*control_info)(void *, uint32_t, struct audio_control_info *);
    int (*control_read)(void *, uint32_t, struct audio_control_value *);
    int (*control_write)(void *, uint32_t, const struct audio_control_value *);
};
struct audio_card_identity {
    uint8_t bus, slot, function, codec;
    uint16_t vendor, device;
    uint32_t codec_id, subsystem_id;
    char id[16], name[80];
};
```

新 API 的类型与签名：`pci_enumerate(index, out)`，`map_mmio(phys, bytes)`/`unmap_mmio(ptr, bytes)`，`alloc_dma(pages, mask)`/`free_dma(phys,pages)`，`request_pci_irq(dev, handler, opaque, out_handle)`/`free_pci_irq(handle)`，`audio_register_card(identity,ops,opaque,out_id)`/`audio_unregister_card(id,force)`，`audio_period_elapsed(card,stream,frames,error)`、`audio_control_changed(card,control)`。每个字段追加在旧 API 尾部；新模块检查 `struct_size >= offsetof(last)+sizeof(last)`。

卡注册复制 identity/ops 元数据，记录 loading module owner，使用递增 generation 防止旧回调命中新卡。MMIO 只映射 BAR 范围为 UC/NX；DMA 分配器按空闲页扫描，先验 `pages*4096` 溢出和最后地址 mask，再原子 claim 全区。MSI capability walk 最多 48 项，vector 使用 `0x50..0x5f`，新增逐向量 trap stub 和 IDT gate；handler 在 EOI 前读取/清 W1C 状态。MSI 不可用时返回 `ENOTSUP`，由 H2 决定降级模式；不构造未证明的 INTx 路由。注销先禁用设备 MSI、移除 registry、同步在途 handler，最后回收 vector。降级服务从 `time_on_tick()` 通知 `audio_service_tick()`，每 10 ms 处理至多 32 条 RIRB/period 状态，不等待新回应或持锁睡眠；jack 的多 verb 操作分步推进。1/4 CPU fixture 验证 service generation 与 unregister 不冲突。

- [x] **步骤 4：绿测试与模块回归。** `python3 tools/test_hda.py resources lifetime`；`python3 tools/test_e1000.py`；`python3 tools/test_linux_descriptors.py`；`make O=out/audio-hda -j8 kernel drivers`。预期所有测试 exit 0，已有五种模块能编译，新 API 只追加。v1 基线已由 host compiler 核查：`register_audio` offset=160、kernel API size=168、audio ops size=32；禁止为了测试改旧布局。

- [x] **步骤 5：审查并记录边界。** 审查资源账本归零、callback generation、MSI unregister 竞争。更新计划步骤。用户授权 commit 时，在子仓只 stage 本任务文件并执行 `git commit -m 'feat(audio): add versioned card and PCI resource services'`；未授权保持可 review 的 diff。

## 任务 H2：HDA controller 与可靠 codec 命令传输

**文件：** 创建 `drivers/hda/hda.h`、`controller.c`、`tools/tests/hda_controller_test.c`；扩展 `tools/test_hda.py controller`。

- [x] **步骤 1：写 fake MMIO controller 测试。** fixture 以 `uint8_t regs[0x4000]` 保存 MMIO，write hooks 实现 CRST/CORB reset 和 W1C，fake clock 每次等待递增，CORB 消费者按 verb 给 RIRB 写入回应。fixture 在本测试文件定义，不代替 controller 算法。最少断言：

```c
uint32_t hda_encode_verb(uint8_t cad, uint8_t nid,
                         uint16_t verb, uint16_t payload, bool short_verb);
static void verb_layout_is_correct(void) {
    assert(hda_encode_verb(2, 0x12, 0xf00, 9, false) == 0x212f0009);
    assert(hda_encode_verb(1, 3, 3, 0xb080, true) == 0x1033b080);
}
```

另验 ring size 2/16/256、CRST 卡住、RIRB wrap、不同 CAD、unsolicited 不占命令回应、overrun 后不误配、reset 所有失败点资源回收。`short_verb=true` 表示 4-bit verb + 16-bit payload。

- [x] **步骤 2：红测试。** `python3 tools/test_hda.py controller`；预期缺 controller/编码函数，随后注入卡住 reset 验证测试确实超时失败。

- [x] **步骤 3：实现 controller 状态机。** 定义 `struct hda_controller` 保存 card ID、BDF、MMIO、GCAP、CORB/RIRB DMA、rp/wp、IRQ handle、clock、command waiter 与 unsolicited 队列。公开函数 `hda_controller_init(c,api,dev)`、`hda_controller_destroy(c)`、`hda_exec_verb(c,cad,nid,verb,payload,short_verb,out)`、`hda_controller_service(c)`、`hda_encode_verb(...)`。编码实现：

```c
uint32_t hda_encode_verb(uint8_t cad, uint8_t nid,
                        uint16_t verb, uint16_t payload, bool short_verb) {
    uint32_t base = ((uint32_t)cad << 28) | ((uint32_t)nid << 20);
    return short_verb ? base | ((verb & 15u) << 16) | payload
                      : base | ((verb & 4095u) << 8) | (payload & 255u);
}
```

寄存器集中定义：GCAP `0x00`、GCTL `0x08`、STATESTS `0x0e`、INTCTL/INTSTS `0x20/0x24`、CORB `0x40..0x4e`、RIRB `0x50..0x5e`、DPLBASE/DPUBASE `0x70/0x74`、stream base `0x80`、stride `0x20`。按宽度访问，SDCTL 24 位不可用 32 位 RMW 覆盖 SDSTS。选择 ring size 时读能力位，写选择位。reset 100 ms 截止，退出 reset 后按规范至少等待 521 us；初始稳定 codec 检测再留 1 ms。命令 100 ms 截止；与同 controller 的其他命令串行，等待期间释放 spinlock，ISR 推送回应。即时命令仅是 ring 初始化失败时有日志的恢复路径，不能把它当作完整 unsolicited 能力。普通命令超时撤销 waiter、恢复 transport generation；连续失效将 card 断开。

- [x] **步骤 4：绿测试。** `python3 tools/test_hda.py controller resources lifetime`；预期所有失败点返回准确 errno、无 ASan/UBSan、无过期回应命中新请求。检查每条 busy wait 的截止时间。

- [x] **步骤 5：独立审查。** 校验 register width、bar mapping、memory barrier、CAD 解码、ring reset 顺序。授权后子仓提交 `feat(hda): implement controller and codec transport`；主仓测试文件另行提交。

初始化失败回收的安全例外（2026-10-01 实施裁决）：所有可证明 DMA 已停止的失败点必须回收全部资源；partial-init 已启动 ring 后若硬件拒绝 stop/reset，返回错误并保留可能仍在 DMA 的 leases，禁止未初始化 controller 的 service，供 retry/H6 manager retention 桥接。不能为了账本归零释放未知仍在 DMA 的页。这一例外须由真实生产 partial-init/destroy/retry 测试验证，与普通失败回收分别记录。

## 任务 H3：codec 拓扑与模拟播放/录音路由

**文件：** 创建 `drivers/hda/codec.c`；扩展 `hda.h`；创建 `tools/tests/hda_codec_test.c`。

- [ ] **步骤 1：写拓扑 fixture 测试。** 定义完整节点参数/连接数组作为 fake verb 响应，至少包含 `pin -> mixer -> selector -> DAC`、`ADC -> selector -> mic`、AFG 能力继承、2 codec、循环、NID 溢出、range 第一个成员非法：

```c
int hda_expand_connections(const uint16_t *raw, uint32_t count, bool long_form,
                           uint8_t first, uint16_t nodes,
                           uint8_t *out, uint32_t capacity);
static void connection_ranges_expand(void) {
    uint16_t raw[] = {2, 0x85}; uint8_t out[8];
    int n = hda_expand_connections(raw, 2, false, 1, 15, out, 8);
    assert(n == 4 && out[0] == 2 && out[3] == 5);
    assert(hda_expand_connections(raw, 2, false, 1, 3, out, 8) == -EINVAL);
}
```

连接展开和 graph search 测试调用生产函数。route fixture 用 verb log 验证 EAPD、pin control、amp mute/unmute、selector 和 ADC，不仅检查最终选择的 DAC 编号。

- [ ] **步骤 2：红测试。** `python3 tools/test_hda.py codec`；初始链接失败；存在图搜索后故意使用环图确保测试失败而不是无限递归。

- [ ] **步骤 3：实现 parser 与路由。** 定义 `hda_codec_probe(c,cad,codec)`、`hda_find_route(codec,pin,direction,route)`、`hda_apply_route(codec,route,enable)`；`hda_node` 保存 caps、pin default、连接列表、amp caps 和 rate/format。限制每 codec 256 NID、每节点 256 connection，分配失败返回 ENOMEM，读取数值先校验边界。搜索使用 visited bitmap，capture 从 ADC 沿输入连接追到 pin，playback 从 pin 沿输入连接追到 DAC。多输出 association/sequence 按真实映射组合。连接展开的核心：

```c
uint16_t range = long_form ? 0x8000u : 0x80u;
uint16_t mask = long_form ? 0x7fffu : 0x7fu;
uint16_t end = raw[i] & mask;
uint16_t begin = (raw[i] & range) ? previous + 1u : end;
if (((raw[i] & range) && !have_previous) || begin > end ||
    begin < first || end >= (uint32_t)first + nodes || end > 255)
    return -EINVAL;
if (end - begin + 1u > capacity - used) return -EOVERFLOW;
for (uint16_t nid = begin; nid <= end; ++nid) out[used++] = (uint8_t)nid;
```

路径配置先静音、D0、selector/mixer、pin/VREF/EAPD，再在 stream prepared 时写 converter format/tag；读取回验失败撤销已应用路径。未找到输出/输入分别不公布其 caps，不把没有 ADC 的 codec 暴露为 capture。

- [ ] **步骤 4：绿测试。** `python3 tools/test_hda.py codec controller`；要求图的选择与 verb trace 都正确，malformed codec 不越界、循环无 hang。

- [ ] **步骤 5：审查诊断。** codec dump 包含 vendor/revision/subsystem、NID/caps/default/连接，不包含内核地址。授权后提交 `feat(hda): discover generic analog codec routes`。

## 任务 H4：中断驱动的播放/录音 DMA 与可恢复 XRUN

**文件：** 创建 `drivers/hda/stream.c`、`tools/tests/hda_stream_test.c`；扩展 `hda.h`、controller ISR。

- [ ] **步骤 1：写实际 DMA 控制函数测试。** fake DMA buffer 含已知左右样本，fake position/IRQ 模拟四个 period、环绕、重复/过早中断、FIFO error 和 descriptor error。定义并测试生产 `hda_format_encode()`：

```c
int hda_format_encode(uint32_t rate, uint32_t bits, uint32_t channels,
                      uint16_t *out);
static void format_and_wrap(void) {
    uint16_t f;
    assert(!hda_format_encode(48000, 16, 2, &f) && f == 0x11);
    assert(!hda_format_encode(44100, 16, 2, &f) && f == 0x4011);
    assert(hda_format_encode(48000, 16, 0, &f) == -EINVAL);
}
```

另验 prepare 不启动 DMA、start 才写 RUN、pause 不重置 hw pointer、stop 不再 callback、capture 数据写入正确位置、64OK=0 的全部 BDL 地址 <4 GiB。

- [ ] **步骤 2：红测试。** `python3 tools/test_hda.py stream`；初始函数缺失，记录错误。

- [ ] **步骤 3：实现 stream ops。** `hda_pcm_caps/open/prepare/trigger/pointer/close` 与 H1 ops 同签名；stream descriptor index 依据 GCAP 的 input/output/bidirectional count，stream tag 在 controller 内唯一为 1..15。BDL entry 为 `{uint64_t addr; uint32_t length,flags;}`，16 字节，BDL 128-byte 对齐，period IOC。CBL/LVI/format 在 stream reset 后设置，converter 先绑定 tag/channel，再启动。format 用 48k/44.1k 基数、整数 multiplier/divisor 搜索，bits 对应 8/16/20/24/32，channels-1 在低 4 位；必须先与 caps 求交。

ISR 读取 INTSTS -> 每 stream SDSTS -> W1C 清已处理位 -> 更新单调 frame 计数 -> 发布 period/error。读 pointer 使用 position-buffer acquire，异常/不动且 LPIB 前进则切换 LPIB。不能从 modulo 差值猜测超过一圈的丢失时长；period count/clock 无法证明进度时进入 XRUN。ERROR 先停止 RUN，再报告 EPIPE。prepare 清 ring 并建立 generation；stop/destroy 同步 IRQ，DMA 页由核心最后释放。

- [ ] **步骤 4：绿测试。** `python3 tools/test_hda.py stream controller codec lifetime`；`make O=out/audio-hda -j8 kernel drivers`。验证 memory fence、BDL alignment、capture sample order、pause/resume 位。

- [ ] **步骤 5：审查中断工作量和错误恢复。** IRQ 不分配、不等待、不操作用户指针；录音不会读未完成 period；授权后提交 `feat(hda): add PCM DMA playback and capture`。

## 任务 H5：真实 mixer 控件、耳机插拔和原子路由切换

**文件：** 创建 `drivers/hda/controls.c`、`tools/tests/hda_controls_test.c`；扩展 codec 事件和 card control ops。

- [ ] **步骤 1：写 amp 与 jack 事件测试。** 为假 codec 定义 amp offset、steps、step-size、mute capability、speaker/headphone route；断言原始增益和 dB 换算，不把百分比直接写硬件：

```c
int32_t hda_amp_db(uint32_t caps, uint8_t gain);
static void amp_step_is_quarter_db_units(void) {
    uint32_t caps = 20u | (40u << 8) | (1u << 16) | (1u << 31);
    assert(hda_amp_db(caps, 20) == 0);
    assert(hda_amp_db(caps, 0) == -1000); /* hundredths of dB */
}
```

测试重复插拔只发布状态变化，auto-mute off 保持扬声器，route 在第 3 个 verb 失败后回滚，capture source 改变时保持独立播放增益，cap 没有 mute 位时不声称硬件 mute。

- [ ] **步骤 2：红测试。** `python3 tools/test_hda.py controls`；初始缺控件实现。

- [ ] **步骤 3：实现 control ops 与 jack worker。** numid 由注册顺序决定，name/type/count/min/max 与硬件一致；音量范围为整数 gain，TLV 返回真实 dB min/step。换算：

```c
int32_t hda_amp_db(uint32_t caps, uint8_t gain) {
    int32_t offset = caps & 0x7f;
    int32_t step = ((caps >> 16) & 0x7f) + 1;
    return ((int32_t)gain - offset) * step * 25;
}
```

unsolicited tags 分配到真实 pin，response 只入有界队列；worker 读取 pin sense、更新 jack 控件、执行 auto-mute，事件溢出触发重新采样而不假设最后状态。无 unsolicited 支持时 100 ms 检查真实 pin sense，标记模式。路由改变暂时静音、验证新路由全部 verbs、切换并恢复 volume；失败恢复原 route 和 control value，发布错误而不先显示成功。quirk 表键含 vendor/device/subsystem/codec/revision，仅支持有 fixture/硬件证据的条目。

- [ ] **步骤 4：绿测试。** `python3 tools/test_hda.py controls codec controller lifetime`；验证 dB 单位、mute、取消事件和路由回滚。

- [ ] **步骤 5：审查功能矩阵。** 给每种生成控件记录对应 NID/amp/策略来源，无模拟硬件能力。授权后提交 `feat(hda): expose mixer and jack routing controls`。

## 任务 H6：模块注册、完整构建发布与 VM 波形验收

**文件：** 创建 `drivers/hda/hda.c`、`tests/build/test-hda-products.sh`、`tools/test_hda_qemu.py`；修改子仓/主仓 Make 列表、run.mk、`docs/DRIVERS.md`；检查 `tools/build/rootfs-stage.sh` glob 与子仓 install 输出。本任务依赖 A1–A6；测试通过标准设备读取。

- [ ] **步骤 1：写缺失产物与失败初始化测试。** shell fixture 和真实 staged manifest 要求 `hda.drv` 与既有五模块并存；host lifetime suite 对第 N 次资源分配失败逐一注入，要求全部 handle/page/card 数归零。QEMU runner 接收 `--image/--output/--controller/--codec/--smp/--msi`，用独立 snapshot，不改用户磁盘；guest probe 由 A6 提供。

```sh
test -f "$audio_out/generated/drivers/hda.drv"
test -f "$audio_out/kernel-install/hda.drv"
test -f "$audio_root/usr/lib/reliefos/drivers/hda.drv"
for name in ac97 es1371 e1000 mouse serial; do
    test -f "$audio_root/usr/lib/reliefos/drivers/$name.drv"
done
```

- [ ] **步骤 2：红测试。** `sh tests/build/test-hda-products.sh`；`python3 tools/test_hda.py lifetime`；预期未列入 install/publish 的 hda.drv 失败。

- [ ] **步骤 3：实现模块 init/fini 与 staging。** init 扫描所有 HDA BDF，每个 controller 独立资源/codec/card；存在至少一个 usable card 才成功。失败清理已获得资源。fini 注销全部 card、停止 work/IRQ/DMA，再 unmap/free。主仓 `RELIEFNT_DRIVER_NAMES`、publish pairs、`DRIVER_NAMES` 和子仓 driver install 都加入 hda。`QEMU_SOUND_DEVICE ?= hda`，HDA 参数为 `-device intel-hda,msi=auto -device hda-duplex,audiodev=snd0`；选择 ac97/es1371 则使用原设备参数。既有驱动通过 v1 adapter 保留 OSS，只公布其已实现能力；任何未实现的 capture/reset/pause 不放进 ALSA 能力。新增内核故障诊断稳定前缀 `[hda]`。

- [ ] **步骤 4：构建、VM 与波形绿测试。** 执行：

```sh
python3 tools/test_hda.py resources lifetime controller codec stream controls
make O=out/audio-hda -j8 kernel drivers image-vmdk iso installer
sh tests/build/test-hda-products.sh
python3 tools/test_hda_qemu.py --image out/audio-hda/images/reliefos.vmdk \
  --output out/audio-hda/qa/hda --controller intel-hda --codec hda-duplex --smp 4 --msi on
```

已核查 `mk/images.mk` 的 `DISK_VMDK=$(O)/images/reliefos.vmdk`。WAV 后端生成 tone.wav，Python `wave/struct/math` 分析频率、声道、时长；capture 的输出样本比对使用实际输入音频后端，WAV/none 只能记录 capture syscall 路径的有限验证。再覆盖 pc/q35、1/4 CPU、ich9-intel-hda、hda-output、双 codec、no-device、MSI off（10 ms 服务降级）与 AC97/ES1371；QMP socket 在磁盘目录，不用 `/tmp`。

- [ ] **步骤 5：交付证据和授权提交。** `docs/DRIVERS.md` 只写实际已验证内容，真实机器未验证单列。授权后子仓提交 `feat(hda): register and publish the HDA module`；取得子仓可用提交后主仓更新 gitlink，禁止自动 push。H6 完成不代表 U 项目或真机门槛完成。

H2 实际销毁合同补充（2026-10-01）：`hda_controller_destroy()` 无法确认 DMA stop 时返回负错误并保留 leases。当前 manager 的 void fini 后会自动释放 owner 资源/模块，因此 H6 必须建立 teardown failure → manager retention 的明确桥接，覆盖 unload、init rollback 和 retry；不能忽略 destroy 返回或释放未知仍在 DMA 的 ring/模块。方案需保持旧 v1 prefix/旧五驱动行为（可用 append-only 私有失败报告入口或等价有验证机制）。fixture 必须调用实际 manager cleanup/unload 路径：注入无法 quiesce 后验证 IRQ 已停而 DMA/MMIO/image 仍持有、错误可诊断；恢复后重试才回收真实 provider，成功分支仍自动清理遗漏资源。此为 H6 集成要求，H2 standalone GREEN 不代表它已完成。
