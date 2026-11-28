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
        xorriso mtools dosfstools e2fsprogs squashfs-tools libext2fs-dev fakeroot fdisk \
        qemu-utils qemu-system-x86 ovmf zip curl xz-utils patch git

HTML 文档另需 Sphinx，见 :doc:`../process/documentation`。Sphinx 仅服务文档；
内核和用户态生产编译不因此改用 Python、Meson 或 Ninja。

``make test-build`` 中的播放器宿主检查另需 ``ffmpeg``、``libavformat-dev``、
``libavcodec-dev``、``libavutil-dev``、``libswresample-dev`` 和 ``libswscale-dev``。
这些是宿主检查依赖；生产播放器使用锁定源码构建的 musl 库。
自启动的宿主检查另需 ``libglib2.0-dev``；生产程序使用锁定 APK 提供的目标 GIO 库和头文件。
可通过 ``PLAYER_TARGET_BUILD=/绝对路径/out/x86_64/release sh tests/build/test-reliefplayer-mp4.sh``
在 Linux 宿主上同时运行实际精简库和 musl 版本的解码/控制检查；这不等于来宾播放验证。

迁移分支：隔离的 Arch 构建根
============================

在 x86_64 Linux 上可以通过本地 Docker 构建，无需在宿主安装 pacman、Arch 包或
编译器。宿主需要 Bash、Git 2.31+、GNU Make 4.3+、coreutils、awk、sed 和
util-linux（``flock``），以及可通过 Unix socket 访问的 Docker Engine 25+。
例如 Debian 13 可安装 ``docker.io git make``；先按宿主发行版的说明配置
Docker。以普通用户执行，不要使用 ``sudo make``。Docker daemon 对其使用者授予高权限，
仅使用受信任的本地 daemon；目前不支持远端、rootless 或 userns-remap 模式。
源码和 ``O`` 路径沿用 Kbuild 的字符限制，不支持空格。

在仓库根目录依次运行::

    git submodule update --init --recursive
    make arch-doctor
    make arch-fetch
    make arch-build ARCH_JOBS=8

``arch-doctor`` 只检查宿主条件，不下载、不创建输出；``make -n`` 可预览入口。
``arch-fetch`` 使用摘要固定的官方 Arch 基础镜像和 pacman 签名验证准备工具链，
并下载校验 ReliefNT 锁定的构建输入。Arch 软件仓库滚动更新；固定的是基础镜像，
不是每次初次获取时的软件仓库快照。同一配方复用本地镜像。
``arch-build`` 不拉镜像、不联网，沿用根 Makefile 的 ``kernel`` 目标。容器内用
调用者的 UID/GID 编译，源码、Git 元数据和容器根是只读的；下载缓存保存在
``O/downloads/``。默认 ``O=out/arch-root/x86_64/release``，也可为这三个命令
指定同一个新目录。不要与原生 Kbuild 共享 ``O``。

验证范围为 ``O/generated/system/kernel.sys``、``kernel.debug`` 和
``O/kernel-install/install.bundle.json``。这是迁移用构建入口，尚不构建 glibc
来宾用户态或 ISO；以下原生入口仍负责现有产品和启动验证。

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

``make installer`` 使用 ``mksquashfs`` 生成 ``O/images/installer-payload.squashfs``，
以 gzip 压缩并与可写的 ``installer-root.ext4`` 一起打入原路径的
``O/images/reliefos-installer.iso``。此目标要求 ReliefNT 配置中保留默认的
``CONFIG_SQUASHFS=y``。``CONFIG_INSTALLER_ROOT_SIZE_MIB`` 仅设置运行时 ext4 的容量
下限，默认 32 MiB；构建仍根据运行时文件及预留空间自动增大该镜像。

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
