# VMware wavplay 播放失败交接

2026-10-04。仅在 `/home/xiaobai/Projects/Projects/ReliefOS-intel-hda-audio-plan` 操作。用户已暂停 U6；本轮仅处理其 VMware 无声及 `wavplay` 退出报告。没有子代理、提交、推送、归档、reset，未修改或停止用户 VMware，保留所有原 dirty/untracked 源码与证据。`/home/xiaobai/log.txt` 和 VMX/宿主日志仅作为数据读取。

## 已证实的故障

用户 VMware 使用 ES1371 ABI-v1。串口完整日志显示第一次 pid 382 在约 307 秒成功配置、写入非零 PCM，随后队列满并退出 1；第二次 pid 393 在约 340 秒报告设备不可用。`first nonzero DMA range consumed` 属于旧队列推进，不代表第二次应用成功播放或扬声器有声。

1. `wavplay` 把 musl `write()` 的 `-1/errno=EAGAIN` 当作原始 syscall 的 `-EAGAIN`，暂时队列满即退出。真实播放器源码 host fixture 的 RED 为 SIGABRT，修复后严格编译、ASan/UBSan 通过，覆盖 EAGAIN、EINTR、短写和终止错误。
2. 旧 `/dev/dsp` 关闭后 ABI-v1 硬件队列仍可能有 PCM。即使新描述请求相同的 48000 Hz，OSS SPEED 也返回 EBUSY。新增旧队列关闭/重开测试 RED 为 SIGABRT；同速请求成为不重置硬件的成功协商，不同速仍拒绝正在播放的队列。严格 OSS fixture 和 17 项真实等待回归通过。
3. 每块 PCM 后额外按毫秒休眠，将 1024/48000 秒取整为 22 ms，再由 10 ms 调度 tick 取整为 30 ms。旧 QEMU AC97 两次 30 秒 PCM 分别约播放 42 秒，波形含 571 个超过 10 ms 的内部静音间隙。改为依据真实队列背压写入，最终 SYNC 排空并显式关闭描述符。
4. 仅修复以上问题仍不足：audio-fix-r2 在 QEMU 第一次播放成功，第二次在约 36 秒立即因 EAGAIN 失败。原 `nanosleep` 用原始 `time_ticks()` 建立相对截止时间，却用经过 NTP 校时的 CLOCK_MONOTONIC 判断。实际内核函数的正/负时钟偏移回归证实等待消失或变长；统一截止时间与判断的时钟域，只把剩余等待换算到调度 tick。信号中断的剩余时间也采用保留的时钟域，对应真实信号投递 RED/GREEN、严格编译和 ASan/UBSan 通过。

## 最终修复与证据

本轮生产变更仅为 `userland/apps/wavplay/main.c`、内核 `audio/oss.c`、`syscall_time.c`、`signal.c`；保留先前 mouse/IRQ 输入修复及所有既有 dirty 修改。没有改变 ES1371、HDA、Doom 或候选 HDA 几何。

证据根目录为 `out/audio-hda-u6/qa/vmware-audio-20261004/`：

- `wavplay-red.log`、`oss-red.log`、`wavplay-queue-pacing-red.log`、`nanosleep-clock-red-r2.log`、`signal-clock-red.log` 保留有效反例。错误的 `--suite` 调用、旧时间测试缺少链接 GC、QA service 依赖/路径及 ENOSPC 失败也保留，未当作有效回归结果。
- `production-build-r3.log`：正常根 Make `kernel app-wavplay` 退出 0。既有 loader 未使用变量警告保留；先前 wavplay 构建的 ioctl 宏重定义警告也保留，不宣称生产整体无警告。
- `final-audio-host.log`、`final-clock-host.log`、`signal-clock-strict.log`、`oss-green.log`、`oss-regressions-r2.log`：相关 host 回归及严格/ASan/UBSan 通过。定向 diff whitespace 检查通过。
- `qemu-before-r3/`：原播放器两次退出 0，但波形节奏错误。`qemu-after-r2/`：未修计时的候选第一次 0、第二次 1，不能接受。`qemu-clock-fix/`：最终修复两次均退出 0，完整串口约 66.8 秒、host 约 74 秒；两段波形跨度分别 30.000023 和 30.000000 秒，立体声一致、频率约 960 Hz。
- `qemu-clock-fix-wave.json` 和 `final-validation.json` 保存断言。QEMU WAV 后端为 44100 Hz，输出头的 RIFF 长度未收尾；只在独立 reflink 副本修正两个长度字段，原音频和所有 PCM 字节保留。
- `r3-source-image-sha.json` 验证真实 raw root 的全部字节、两个 ESP kernel 路径及 wavplay/mouse/ES1371/HDA/Doom 对应正常构建产品；记录源文件与镜像 SHA。

宿主数据空间一度耗尽，部分 VMDK 写入失败并导致 QA 无法完成。没有删除源码或证据；仅对已证明完全相同的完成产物做块去重，记录操作前后相同 SHA。失败候选文件保留。最终 VMDK `check` 无错误，raw/VMDK `compare` 相同。

## 交付镜像

`/home/xiaobai/Projects/Projects/ReliefOS-intel-hda-audio-plan/out/audio-hda-u6/images/reliefos-vmware-audio-fix-r3.vmdk`

VMDK SHA256：`d2641c787eb5a2635e90d2c75ff2960c82e8b21d46a787cf934253a2da2ca169`

raw SHA256：`ffa09ab64bfb9099e78878e4faaa224d72dc4ad9882116c34a01f5c240d6ea85`

kernel.sys SHA256：`9672e9aeba686f4a722f61da184923069c2bb91cb7013d81b551f0f5dd5665e8`

wavplay.elf SHA256：`d31fd98a2d15916cc455637ab8ab9822e861b8d4f0673648766808443ba815aa`

这是独立诊断镜像：冻结 R10 加先前输入修复、正常构建的新内核及 wavplay；没有全量发行包/SDK 重建。交付镜像没有 QA 开机脚本，QA service 仅在单独模型镜像中。用户正在使用的 input-fix-r2 VMDK 未被覆盖；audio-fix-r1/r2 不是复测目标。

## 平台边界与下一步

QEMU 使用 AC97 ABI-v1，验证相同 OSS/用户态/计时生命周期，不能替代 VMware ES1371 的实际出声验证。ES1371 真正驱动的寄存器 host fixture 通过，但其硬件模型和物理扬声器没有在本轮验收。

用户关机后切换到 audio-fix-r3 VMDK，运行 `wavplay`，等正常结束后立即再运行一次。如仍失败，新输出会给出具体 open/ioctl/write/nanosleep/SYNC 操作及 errno，应结合新的完整 VMware 串口继续定位；不能仅凭 DMA consumed 日志宣称有声。

- VMware 最终镜像实际出声：**待用户复测**；修复前失败已有真实用户日志。
- 整体 GUI/Xorg gate：**仍 deferred**；本轮未重跑先前完整输入/GUI 验收。
- 物理 codec gate：**未执行（not_run）**。
- U6：**仍 paused/incomplete**，未接受 8192/65536；本轮没有恢复标准 HDA 矩阵、Doom/controls 或 600 秒长测。
