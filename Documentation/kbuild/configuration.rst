==========
配置与版本
==========

输出树
======

默认 ``O=out/x86_64/release``；支持 ``ARCH=x86_64`` 和
``PROFILE=release|debug``。例如::

    make O=out/debug PROFILE=debug defconfig
    make O=out/debug PROFILE=debug menuconfig
    make O=out/debug PROFILE=debug -j4 kernel

当前配置是 ``O/.config``。olddefconfig 保留设置并补新项；defconfig 重置。
组件清单区分 BUILD、IMAGE、ENTRY、SDK、API；required 组件不能关闭。
不能把镜像策略混入内核私有配置。

版本身份
========

主仓与内核分别读取自己的 ``configs/build-version``。主仓
``system/rootfs/etc/os-release`` 的 VERSION_ID 必须与发布版本一致。
生成头只写 O；源码身份与数字发布版本分开。内核后缀例如::

    make EXTRAVERSION=-perf LOCALVERSION=-test kernel

完整内核版本最多 31 字节，后缀仅接受已允许字符。包元数据仍使用数字基础版本。
SOURCE_DATE_EPOCH 默认为提交时间；不得递增源码计数器或用当前时钟破坏可重现性。
旧输出的 ``config/.config`` 可复制为新输出的 ``.config``，再 olddefconfig。
