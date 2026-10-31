==========
从源码构建
==========

宿主条件
========

使用 Linux/WSL、GNU Make 4.3+ 和仓库指定的 Clang/LLVM 工具链。
Clang 必须包含 x86_64 compiler-rt builtins。典型 Debian/Ubuntu 依赖为::

    sudo apt install build-essential clang lld llvm libclang-rt-dev \
        autoconf automake libtool libtool-bin pkg-config bison flex gperf gettext \
        texinfo libncurses-dev grub-common grub-efi-amd64-bin grub-pc-bin \
        xorriso mtools dosfstools e2fsprogs libext2fs-dev fakeroot fdisk \
        qemu-utils qemu-system-x86 ovmf zip curl xz-utils patch git

HTML 文档另需 Sphinx，见 :doc:`../process/documentation`。Sphinx 仅服务文档；
内核和用户态生产编译不因此改用 Python、Meson 或 Ninja。

构建顺序
========

在 ReliefOS 仓库根目录执行::

    git submodule update --init --recursive
    make help
    make doctor
    make fetch
    make -C kernel/reliefnt fetch
    make defconfig
    make -j8 all

根 Makefile 是受支持入口。fetch 是显式联网阶段，按锁文件校验摘要；
内核缓存独立。普通构建缺缓存应报错，不暗中下载。第三方保留自己的上游构建系统。
源码与公开头从各自所有者导出，不从历史 build 目录补齐。

配置内核
========

``make defconfig`` 有序重置 ReliefOS 与 ReliefNT 的默认配置。内核输出默认
在 ``O/reliefnt``；``RELIEFNT_O=`` 可指定另一输出目录，相对路径从调用
顶层 make 的目录解析。默认将生产部件内置；普通构建及 OS 的
``olddefconfig`` 不重置已有内核选择。

用以下目标编辑同一内核输出的配置::

    make kernel-menuconfig
    make kernel-olddefconfig
    make kernel-allmodconfig

前者打开 ReliefNT 原生菜单，第二个保留已有选择并解析新增选项，第三个将
支持独立模块的部件选为 ``m``。内核与 OS 分别持有配置，应用、镜像和
QEMU 选项继续在 OS 菜单中配置。仅已完成运行时边界的部件才提供 ``m``。

增量与排错
==========

用 ``V=1`` 查看命令，``make --trace`` 查看依赖，``make -n`` 预览。
首次预览产品前先执行 defconfig、config-sync。同一 O 互斥，不伪造内部锁 token；
不同 O 可并行。原版 Kbuild 的 .cmd 与 fixdep 跟踪命令、源码和头文件依赖。

clean 保留配置；distclean 同时清配置。它们只清有所有权标记的输出树，
不删除共享下载缓存或用户旧镜像。
