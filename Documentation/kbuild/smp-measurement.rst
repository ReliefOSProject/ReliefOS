=======================
SMP 整机测量与输入身份
=======================

``tools/test_smp_latency_qemu.py`` 在单独复制的系统镜像中运行有限次工作负载，
不构建内核或获取软件包。开始前需准备独立、不可变的 root ``kernel-payload``
目录和与之匹配的 ESP ``boot-payload`` 目录，以及已安装官方 GCC、make、musl
开发头文件、binutils、FFmpeg（含 libx264）、Xorg 和 IceWM 的来宾磁盘。宿主机
需要 QEMU、e2fsprogs、dosfstools、mtools、``grub-editenv``、目标 musl sysroot、正式 SDK 和
目标 X11 开发文件。源镜像可以是 raw 或单文件 VMDK；外部 extent 和 backing
链无法由单个文件哈希标识，因此会被拒绝。
源盘如果已含所选 release 的内核或模块目录，工具也会拒绝注入，避免保留
清单之外的旧文件；请使用不含该 release 的干净用户态镜像。

从仓库根目录执行，以下变量由测量者指向已冻结的产物：

.. code-block:: sh

   python3 tools/test_smp_latency_qemu.py \
     --image "$SOURCE_IMAGE" --source-sha256 "$SOURCE_SHA256" \
     --kernel "$COHORT_ROOT/usr/lib/reliefos/kernel/$RELEASE/kernel.sys" \
     --kernel-payload "$COHORT_ROOT" --boot-payload "$COHORT_ESP" \
     --sysroot "$SYSROOT" --sdk "$SDK" --x11-root "$X11_DEVELOPMENT_ROOT" \
     --out "$NEW_OUTPUT_DIRECTORY" --expect-profiler both \
     --cpus 4 --repeat 3 --jobs 4 --timeout 1800 --prepare-only

``--prepare-only`` 只生成镜像，不启动虚拟机；删除该参数才由工具启动拥有的
QEMU 子进程。它发现已有 QEMU 或 VMware 时拒绝启动，使用短路径 QMP socket，
超时或失败时只停止并回收自己的子进程，每次等待均有限时。启动前、负载和
清理失败均写入 ``result.json``，保留失败阶段与清理状态。可用准备好的 raw 镜像交由操作者在
VMware 中运行；VMware 的超时控制、串口采集与关闭由操作者负责。离线串口
校验可使用 ``--validate-serial LOG --nonce NONCE --release RELEASE
--expect-profiler MODE --repeat N``，身份信息位于 ``prepared.json``。

``--expect-profiler both`` 在已配置诊断节点的同一内核上分别运行 ``off`` 和
``on``；``on`` 或 ``off`` 只运行其中一种。``absent`` 明确要求
``CONFIG_LOCK_STAT=n`` 的节点不存在，不能作为节点缺失时的自动回退。
``--jobs 0`` 在固定 32 个翻译单元上使用无参数 ``make -j``；其余取值是作业数。
输出记录配置、镜像和源文件身份，保留准备日志、完整 root 和 ESP 回读、
串口、QEMU 日志以及失败结果。内核、initramfs、模块数据库、两个启动槽、
版本文件和清单在启动前核对；不重写 GRUB 或固件。私有副本中禁用原来的
``smp-setup``、DHCP、NTP 和图形会话启动链接，并离线删除旧的 :0 socket、
锁文件和 ``Xorg.0.log``；源盘不变。来宾将 gcc、make、as、ld、ffmpeg 和
ffprobe 的完整版本输出与退出状态写入串口并保存在解析结果中，缺失或失败
的版本记录会使测量失败。串口原始日志完整保留；解析遥测块时只跳过符合
内核格式的完整 DEBUG/INFO 行，其他异常内容仍使该块失败。

固定策略是先不采样地编译并编码/解码一次，然后每次清理目标文件重新编译，
每次编码并完整解码固定 640×360、24 fps、5 秒的软件视频；不清空系统缓存。
这些是预热策略下的重复运行，不应标记为冷启动。编译夹具必须输出精确值
``660e5ee6e1d4b501``；视频必须有 120 帧、正确宽高并完整解码。另有空闲控制
和原生进程/线程内存、COW、暖缓存文件映射与 ``pread`` 负载。任何退出码、
结果检查、样本缺失、超时或来宾崩溃都会使本次结果失败。

每次负载期间运行真实 Xorg、IceWM 和专用窗口。测试服务使用现有 tty wrapper，
仅在隔离镜像的单个 VT 上启动 Xorg；``-nolisten tcp -ac`` 关闭远程 TCP 并
允许隔离系统内的本地客户端连接。X11 数值是客户端定时绘制及 ``XSync`` 服务端
确认的时长，不是物理鼠标、键盘或屏幕扫描输出延迟。程序核对窗口管理器和
实际可见窗口的像素并保存每一个样本；样本使用 ``CLOCK_MONOTONIC`` 纳秒。原生探针也使用
该时钟。X11 探针有独立的有限超时，即使连接或服务端确认卡住也会失败退出。
按样本排序后以 nearest-rank ``ceil(N*p)-1`` （从 0 开始）计算 p50、
p95、p99 和最大值。页故障样本包含探针自己的内容检查和读写开销。

纳秒是存储单位，不表示时钟具有纳秒分辨率。当前多核来宾不假定 TSC
跨 CPU 同步；时钟回退及延迟的 timer IRQ 会影响测量。比较编译吞吐时应
另记录编译区间的宿主单调墙钟和串口观察误差，不能用整个启动/清理耗时
代替编译耗时。X11 round-trip 也不能代替真实拖动、按键和应用操作验收。

交互验收先确认 IceWM 和终端已经可用，再启动编译。检查标题栏和 Alt
拖动、缩放、最大化/最小化与恢复、释放后的指针移动、终端命令及 Fileman
导航。``tools/test_icewm_interactions_qemu.py`` 的截图等待包含宿主图像分析：
1920×1080 的连通区域识别每帧可能超过 2 秒，需允许连续两帧确认；该等待
时间不是来宾输入延迟。冷启动 Xorg/XDM 的等待也与已经运行的桌面分开记录。
最大化的黑色客户区可能与桌面背景连通；应同时用 X11 报告的位置、尺寸和
``map_state`` 核对移动、缩放及最小化。原生窗口状态与实际截图分别留证，
不能以截图识别器没有检测到边界判定窗口管理器未执行操作。

桌面与编译优先级
================

正常图形启动由 root 所有的 ``reliefos-xdm`` 使用现有 BusyBox ``renice``
将自身设为 nice=-10，XDM、Xorg 的线程及 IceWM 继承这个普通调度优先级。
不切换到实时调度。``Xsession`` 在启动 IceWM 后、启动初始终端前将自身
恢复为 nice=0；IceWM 的菜单与快捷键终端入口同样先恢复为 nice=0。
因此终端中执行的 shell、``make -j`` 及其编译器子进程使用正常优先级。
无法调整优先级时仍继续启动会话，不因权限或工具不可用而中断登录。

该策略优先处理繁忙时的显示与窗口事件，交互繁忙时可能让出部分编译吞吐。
验收应在正常 XDM 登录链中只读取 ``getpriority`` 的实际值，覆盖 Xorg 的
输入线程、IceWM 和终端；由测试脚本临时修改优先级的实验不能代替此检查。
Fileman 导航应核对应用提交后的窗口标题和实际截图，避免仅依赖 OCR 对
中文、路径或小字体的识别结果。

诊断节点使用独立的原始 ``tsc_cycles``，各 CPU 的 TSC 不假定同步，不换算成
用户态延迟。每次先停止采样再重新打开快照；因停止可能截断已取得锁的样本，
持锁直方图总数不必等于取得锁总数。任何 ``clock_errors`` 都会使该测量失败。
使用一致的来宾用户态、源文件、作业数、缓存策略、重复次数和检查结果进行
前后比较；失败的行不能用于宣称性能改善。
