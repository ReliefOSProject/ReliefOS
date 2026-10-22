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
