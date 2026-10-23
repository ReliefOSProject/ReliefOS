==========
测试与验收
==========

执行入口
========

从主仓根目录执行；临时文件放在项目输出目录::

    mkdir -p out/tmp
    TMPDIR="$PWD/out/tmp" make test
    TMPDIR="$PWD/out/tmp" make test-long
    TMPDIR="$PWD/out/tmp" make test-legacy
    TMPDIR="$PWD/out/tmp" make test-smoke

test 为宿主/构建契约，test-long 包括长流程与恢复，test-legacy 显式运行
非生产 OS 测试，test-smoke 顺序检查三类介质的 BIOS/UEFI 启动。
test-build 包含 Pages 文档回归，宿主需要 Sphinx；普通内核和用户态编译不因此依赖它。
按风险选择定向检查；通过已有失败之前的步骤不能称为全套通过。

运行证据
========

QEMU 测试使用私有磁盘，保留源镜像与注入载荷摘要、完整串口、返回码、
截图和独立读回。界面出现不等于交互成功；编译成功不等于镜像含有新文件。
Installer 还须覆盖安装后的无 ISO 启动；VT、权限、错误路径分别检查。
QEMU 的 VMware 显卡型号不能替代 VMware 真实运行证明。
现有失败见 :doc:`../admin-guide/limitations`；不重生成 golden 掩盖差异。

Xorg 渲染回归
=============

``tools/tests/xorg_render_runtime_probe.c`` 是在隔离 X server 上运行的维护
探针，不安装到产品镜像。它使用 POSIX C 和 Xlib。可在具备 C 编译器、X11
开发头及库的 ReliefOS 来宾内编译并运行::

    cc -std=c11 -O2 -Wall -Wextra -Werror xorg_render_runtime_probe.c \
        -lX11 -o xorg-render-probe
    DISPLAY=:0 ./xorg-render-probe --wait-wm
    DISPLAY=:0 ./xorg-render-probe --repeat 3
    DISPLAY=:0 ./xorg-render-probe --offscreen --repeat 25

``--wait`` 只验证可连接 X server；``--wait-wm`` 等待窗口管理器发布
``_NET_SUPPORTING_WM_CHECK`` 并检查目标窗口回指自身，拒绝失效属性；两者都
不能替代绘制测试。普通模式检查同步
矩形绘制、批量绘制、重叠滚动、窗口移动和 Pixmap 创建/复制/销毁；关键
阶段以 XGetImage 验证像素。X 错误、错误像素和分配失败均使探针失败。
独立 heartbeat 进程每两秒输出时间，供外部超时监视器检查系统是否仍响应。

探针的窗口使用 override_redirect，窗口移动不等于验证 IceWM 鼠标拖动。
普通模式要求窗口无遮挡；XGetImage 不保证返回被遮挡且无 backing store 的
窗口区域内容，见 `Xlib 图像读回约定 <https://www.x.org/releases/X11R7.6/doc/libX11/specs/libX11/libX11.html>`_。
与其他桌面应用并发时，使用 ``--offscreen`` 在独占 Pixmap 上
绘制和校验，并复制首尾画面到窗口，避免把遮挡/Expose 当作绘制错误。
同步绘制包含 X11 往返开销；批量绘制包含末尾读回，不能将它们解释为纯
GPU 吞吐量。计时只用于诊断，返回零也不表示达到了某个性能门槛。

比较性能时使用相同探针、桌面配置、分辨率、vCPU 和内存，等待 IceWM
就绪后重复采样，顺序运行各台 VM。软件对照使用 modesetting 的
``AccelMethod none``，加速对照使用 ``AccelMethod glamor``；从 Xorg 日志
分别确认 ``glamor disabled`` 和 ``glamor X acceleration enabled on SVGA3D``，
并记录 VMware host renderer。记录内核和探针摘要、完整串口、返回码与每轮
耗时；不能把启动期间加载字体/库的耗时直接当作稳定渲染性能。

全量 syscall-trace 会向串口输出大量日志，显著扰动测试，应仅用于定性
定位；其结果不能进入性能对照。超时、探针失败、Xorg 退出和 Kernel Panic
要分别记录。稳定性测试应另行叠加桌面应用、CPU/内存压力与重复启动，
并保留 panic 的 Reason、RIP 和调用栈；短时无崩溃不构成永久稳定性保证。
