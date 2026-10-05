# U6 音频提交延迟修复：内联审查检查点

工作区为 `/home/xiaobai/Projects/Projects/ReliefOS-intel-hda-audio-plan`。用户要求后续全部内联执行；本记录不是独立 Agent 审查。旧源码、失败日志和诊断快照全部保留；没有提交、推送或归档。

`ds31` 的临时诊断显示四次 PCM 播放应用指针落后于硬件指针，没有 HDA 整圈服务间隔或 FIFO/descriptor 错误。`ds33` 将补帧耗时分开测量后，慢调用的大部分周期耗在 `doom_audio_submit()`。生产 `write()` 设备分支原先把请求截成 4 KiB，因此每个 20 KiB Doom 块至少经过五次 syscall 返回和调度。该行为符合短写语义，但会放大当前内核的提交延迟。

修复只为 PCM/OSS 字节写入选择最多 32 KiB 的请求上限；其他设备仍用原来的文件系统 slice。用户范围检查、PCM/OSS 内部小块复制、队列容量、短写、可中断等待和 EPIPE 恢复均保留。Doom 混音块上限调整为 8,192 帧，使每块最多对应一个 32 KiB 写入；总 PCM 环仍为 512 KiB。全局执行锁、IRQ 屏蔽规则和调度策略没有修改。

`audio-syscall-batch-red-20261004.log` 的真实断言在原 4 KiB 请求策略下失败。`audio-syscall-batch-green-20261004-r3.log` 通过实际 OSS/core PCM 的整块提交、逐字节样本、超大请求上限、队列满时短写/EAGAIN、非音频设备上限以及原有 OFD/SCM_RIGHTS 生命周期检查。该 runner 同时链接真实 ALSA timer 清理实现。

`doom-write-batch-alignment-red-20261004.log` 在旧混音上限下未达到一次提交一块的要求；`doom-write-batch-alignment-green-20261004.log` 的 output、mixer、music 和真实游戏睡眠/补帧路径均通过 ASan/UBSan。`audio-syscall-batch-related-host-20261004.log` 的 PCM、OSS、mmap 均通过。

第一份批量写入镜像 `be8acafb7c748cd99fd571987ee075ae1b9d5a9a63e2cfd7136e8148377a4d91` 在 `ds34` 完成 90 秒 Doom、60 秒 CPU/I/O 重叠、全部标准工具和已知信号波形/采集，但仍有一次 Doom XRUN。不能把此结果称为零欠载验收。块对齐后的 `ds35` 使用镜像 `d8c7a584dbf252bb02a02973ba7d9966cdc9323df862804bd88a5cbfbb5eeb67`：90 秒写入 27,746,580 字节，startup/XRUN/suspend 恢复均为 0，60 秒 CPU/I/O 完整重叠，I/O 为 10,608,640 字节/40 圈。后续 lifetime capture 有两条 overrun，整轮没有获得零 XRUN 接受。600 秒关卡在 `ds37` 运行中。

生产源码已经移除本轮 `audio-debug` 和 RDTSC 诊断。诊断源码、镜像身份及失败结果保留在 `out/audio-hda-u6/qa` 和 `out/ds31..ds34`。本检查点不改变 18/19 账本：U6 仍开放；GUI 按用户指示留给后续 Xorg，物理 codec 未原生启动或透传验证。

默认 2,048 帧核心探针在 `ds36` 完成全部调用与 4,096 帧已知信号采集。其原始整轮失败记录保留：旧检查器把初始 2 秒 tone 与 timer 测试额外的 1 秒 tone 合并。`native-probe-wave-window-red/green-20261004.log` 对该两段信号执行真实 RED/GREEN；新范围固定为初始 96,000 帧，频率、连续性和时长阈值不变。`ds36/wave-window-recheck.json` 确认原始和规范化容器的 PCM 字节相同，95,999 个 active frames 连续、最大周期误差 1。这次复检没有覆盖原始失败结果。

完整读串口又发现旧 `aplay -l`/`arecord -l` 仅输出表头，之前的 shell gate 只检查退出码。因此早期“enumeration PASS”不能证明列出 PCM。`PCM_NEXT_DEVICE` 的 Linux `_IOR` 编码仍消费输入游标；公共 usercopy 先前没有读入它，初始 -1 被当成 0，跳过唯一 device 0。`control-pcm-cursor-usercopy-red-20261004.log` 在实际分发器逐设备断言失败；修复按该命令显式 copy-in，`...green...` 的 control/device/ABI 全部通过。新增标准探针要求两个列表实际包含 card 0/device 0，并增加真实 amixer/soundctl 双向写入、静音及 OpenRC/alsactl 状态恢复。该新验证尚未在包含修复的客体运行；`ds37` 的镜像与探针保留修复前版本，不能据它宣布枚举与持久化接受。
