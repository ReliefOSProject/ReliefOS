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

增量与排错
==========

用 ``V=1`` 查看命令，``make --trace`` 查看依赖，``make -n`` 预览。
首次预览产品前先执行 defconfig、config-sync。同一 O 互斥，不伪造内部锁 token；
不同 O 可并行。原版 Kbuild 的 .cmd 与 fixdep 跟踪命令、源码和头文件依赖。

clean 保留配置；distclean 同时清配置。它们只清有所有权标记的输出树，
不删除共享下载缓存或用户旧镜像。
