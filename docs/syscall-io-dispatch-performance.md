# 读写后端提取与向量调用栈修复

上轮成果已推送：内核 `151cb6c`，父仓库 `93d0cb78`。本轮在其基础上提取读写后端并修复向量调用叠加大栈帧的问题。对照和最终 H 版本均为 5.1.1、debug 配置（`-O0 -g`），使用相同用户态；版本、编译优化级别和命令参数变化没有计入性能收益。

## 保留的实现

完整 READ/WRITE 分支提取为私有 `syscall_io_read()` / `syscall_io_write()`，legacy 中两个分支调用同一后端。提取只去掉一层缩进；回填原分支、删除 helper 并还原向量调用和注释后，源码与基线逐字节一致。最终标量 pinned 路径仍通过 legacy dispatcher，保留基线的 fd 和 syscall number 匹配条件。

向量循环原来为每个 iovec 再次进入整个 legacy dispatcher，现在直接调用同一标量后端。向量验证、较早处理的 signalfd/DRM/AF_UNIX 分支、部分成功、短 I/O、positional offset 恢复及外层 syscall_file_number/pin/retry 状态保持。缓冲区、访问模式、PTY、pipe、socket、设备、eventfd/timerfd、procfs 和常规文件后端语义没有改写；没有改变用户 ABI 或重生成 ABI golden。

标量直接进入 helper 的实验捷径已撤销：实验 G 的 VMware 四核默认小块在首次对照下降 12.6%，按事先固定的逆序复测、合并全部六个样本后仍下降 5.74%。其完整桌面运行也在双核 32/40、四核 8/40 次拖动后超时，未执行 VT 检查。全部失败与较慢样本保留在 `out/io-dispatch-20261020/`，没有用较有利的中间结果代替最终 H 的数据，也没有把标量探针中的 560/224 字节深度当作最终产品结果。

## 向量路径栈深度

本次 debug 基线 legacy 函数局部栈帧为 71,760 字节。仅提取标量、仍递归调用 legacy 的中间版本，guest 探针测得叠加深度已经超过架构入口分配的 128 KiB（131,072 字节）系统调用栈。该中间路径先在 96 KiB 诊断预算处失败，最终 H 再通过：

| 实际调用 | 保留递归的中间版本，字节 | 最终 H，字节 |
| --- | ---: | ---: |
| writev | 142,608 | 71,424 |
| readv | 142,944 | 71,760 |

这是从 dispatcher 入口到标量 backend 入口的实测，包含探针自身少量栈空间，不是完整系统调用及深层设备操作的最大值。96 KiB 是诊断预算，不是新 ABI；向量外层仍使用一个较大的 legacy frame。临时探针和标记已从最终产品源码及 ELF 移除。

## 编译与定向检查

canonical 内核编译成功；syscall.c 编译单元与基线具有相同的 15 条既有 warning。最终源码和 sys/debug/unstripped 三份产物分别冻结核对。最终内核 SHA-256 为 `3c73bc760328a8be4821f7b0d8511ebade0b46bb44bec4721ac696376e39507f`，基线为 `4a70d75cf6f92bb539ff5d86432d1d3bc3624b828da598afc0cd6b0bb1a74ea0`。

```sh
TMPDIR="$PWD/out/tmp" make O=out/debug PROFILE=debug -j8 kernel
TMPDIR="$PWD/out/tmp" make O=out/host-syscall-io test-syscall-io
TMPDIR="$PWD/out/tmp" python3 tools/test_linux_descriptors.py
TMPDIR="$PWD/out/tmp" make O=out/host-io-dispatch-return test-user-return
```

新增 host fixture 在 -O0/-O2 下用 ASan/UBSan 编译实际读写后端及生产 pipe/wait 代码，检查重定向标准流、EFAULT/EBADF、等待/重试、非阻塞、EOF、SIGPIPE、null 和 eventfd。硬件及常规存储是不可达边界，fixture 不执行完整 dispatcher 的 pin 生命周期。descriptor、user-return、权限和 socket batch 定向检查通过。

`tools/tests/syscall_io_runtime_probe.c` 通过实际 POSIX 调用检查 readv/writev、pread/pwrite、共享 offset、只读 fd、fork/dup2 管道、阻塞与非阻塞、EOF、EPIPE、AF_UNIX、PTY 和 null，共 52 项。Linux 参考及最终 H 的 QEMU 1/2/4 核运行均为 52 项、0 失败。静态 guest 探针由已安装的 host musl-gcc 构建，库、源码和二进制摘要有记录，没有冒充缺失的 pinned SDK。定向检查不构成完整 Linux/POSIX 一致性认证。

公开 `make test` 仍在 `test-apk-ownership.sh` 失败：`component policy mismatch: ['soundctl']`。相关测试、实现、ownership/components/layout 配置均与已推送 HEAD 一致，未在本轮修改；公开套件在此停止，不能写成全套通过。既有音频、ABI golden 和其他 fixture 的失败或用户决策项未静默修改。

GPT-6-Luna（Max）独立复核最终 H 的后端提取和向量语义，无 Critical/Important/Minor 问题。源码重建证明见 `final-h-extraction-proof.json`；最终检查见 `final-h-audit.json`。

## 最终 H 的吞吐对照

正式测速不含诊断探针。每端、每种块大小保留三次样本，取中位数；吞吐为十进制 MB/s。私有盘使用相同根分区，ESP 内 `reliefos` 和 `leonos` 两个兼容内核路径均回读 SHA-256。源盘只读，测试结束另核对未改变。

```sh
# 默认 bs=512，每次 256 MiB
yes | dd of=/dev/null count=524288 iflag=fullblock status=progress
# bs=65536，每次 1 GiB；QEMU 单独测试
yes | dd of=/dev/null bs=65536 count=16384 iflag=fullblock status=progress
```

| 环境 / 块大小 | 对照 MB/s | 最终 H MB/s | 变化 |
| --- | ---: | ---: | ---: |
| QEMU 2 vCPU，bs=512 | 262.03 | 266.93 | +1.87% |
| QEMU 2 vCPU，bs=65536 | 2005.23 | 2051.00 | +2.28% |
| VMware 2 vCPU，bs=512 | 175.32 | 184.38 | +5.17% |
| VMware 4 vCPU，bs=512 | 203.89 | 219.72 | +7.77% |

QEMU 使用 4 GiB 内存，VMware 使用 2 GiB 并启用 SVGA3D；两类虚拟机的数据分别比较。宿主负载没有完全隔离，三次短样本不足以证明固定收益或解释全部波动。没有达到 5 GB/s，也不能把大块吞吐当成默认小块的结果。

## VMware 桌面负载

最终 H 的四核正式运行中，真实 GCC `make -j4` 完成 2 轮 zlib 静态库构建及示例运行；归档 SHA-256 为 `b57fb8ea48bf73a841aeb6662272359af676160de71c9030ade1d1dc54dd3354`。SVGA3D/glamor/GLX 路径及 18 项绘图和像素检查通过。root 实际查看了 benchmark、Fileman 和失败截图，中文标签及目录内容完整，没有明显画面损坏。Fileman 冷启动至内容可见耗时 20.12 秒；该值包含宿主输入及画面采集，不是纯内核耗时。

桌面压力阶段**失败**：连续拖动完成 10/40 次后，第 11 次要求标题条到 y=84，12 秒内观察到 y=70，未满足同方向至少 5px 的判定。所有成功位移及失败画面均保留，未放宽阈值、丢弃失败或重跑直到通过。编译循环的协作停止/回收及 VT2/VT1 检查没有执行；finally 关闭该测试虚拟机，stop 状态为 0，宿主 VMware 配置恢复。完整运行 `completed=false`，不能把已完成的吞吐、绘图阶段写成完整桌面通过。

最终 H 的双核正式 VMware 运行仅测试吞吐，未测试双核编译压力桌面；G 的旧桌面结果不能替代 H。当前没有证据宣称编译期间 IceWM 的拖动问题已修复。

宿主旧 QA RFB 模块和 VMX 模板被外部清理，首次 G 原生运行在导入工具时失败，尚未启动 VM；失败记录保留。任务目录恢复传输模块与私有模板，按 [RFB 协议说明](https://www.rfc-editor.org/rfc/rfc6143.html)核对协议并通过本地握手、认证向量、输入事件和完整画面拼接回归；VT 输入工具也在正式 VT 阶段前恢复。正式两端使用相同控制器，测量轨迹与判定阈值未改。工具恢复没有进入产品。

**VMware 验证未完成**；物理硬件未验证。安装升级、挂起恢复、长期稳定性及完整一致性测试未覆盖。
