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
空输出目录首次运行 olddefconfig 或 menuconfig 时，也先载入相同的项目种子。
``configs/default.conf`` 是默认配置种子；完整结果由原版 Kconfig 生成。
不能把镜像策略混入内核私有配置。ReliefNT 的配置只在其独立仓库维护，
不会 source 主仓的应用、镜像、授权或 QEMU 配置。

菜单与实际作用
==============

主菜单按系统镜像、授权策略、软件仓库、可选用户态组件和 QEMU 运行参数分组。
镜像尺寸是构建工具的最低容量请求，工具仍会按文件大小和分区开销扩大输出。
QEMU 的 CPU、内存、显示和网络参数只控制来宾启动命令，不编译内核功能。

编译策略使用 ``PROFILE=release`` 或 ``PROFILE=debug``，显示命令用 ``V=1``。
旧 BUILD 预设、优化/LTO/符号/剥离开关已经不被生产链消费，现已移除；
测试按明确的 Make 目标运行，校验和与归属文件由相应产品规则保证，
不提供能跳过这些规则的虚设开关。

Legacy framebuffer 外观仅控制保留程序读取的 display.conf；
XDM 使用自己的 Xresources，IceWM 使用自己的主题，均不由该选项改变。

组件选择
========

``configs/components.toml`` 是组件身份、依赖和默认策略的唯一清单。
维护后运行 ``make regen-component-kconfig``，检查生成的 Kconfig.components。
每个可选组件有独立子菜单；required 组件固定启用，不生成可关闭的选项。

* BUILD：启用组件。依赖可自动启用它；第三方库也可能因其他产物需要而被构建。
* IMAGE：把启用的组件纳入系统载荷。静态库可用 image_supported=false 明确声明
  没有独立运行时载荷，这类组件不生成 IMAGE 开关。
* ENTRY：控制旧应用注册表的入口，依赖 IMAGE。它不会改写 X11 程序提供的
  desktop 文件；原生 X11 菜单和 MIME 行为仍由这些文件定义。库没有应用入口开关。

当前 musl SDK 是产品规则定义的固定 sysroot；组件 SDK 开关没有控制它，已移除。
API 包也没有对应的可选生产目标，不生成 API 开关。组件元数据保留原格式的
SDK/API 字段供兼容读取，不能由这些字段推断 SDK 实际文件；以安装清单为准。

旧配置与检查
============

现有 O/.config 先备份，再执行 olddefconfig 与 config-sync，原版 Kconfig 会去掉
已删除及不属于本仓的符号。更早的 LEON_COMPONENT 名称先用 migrate-config 显式迁移，
该步骤保留备份并拒绝冲突；不要直接用默认配置覆盖用户选择。
未被构建链使用的重复默认模板已删除。

``tests/build/test-kconfig-contract.sh`` 检查原生依赖、独立镜像/入口选择、
无效控制项和配置稳定性；config-groups 与 config-first-build 检查并发、增量、
缺失产物修复及首次构建。它们使用真实原版 conf，不以文本替换代替 Kconfig 求值。

版本身份
========

主仓与内核分别读取自己的 ``configs/build-version``。主仓
``system/rootfs/etc/os-release`` 的 VERSION_ID 必须与发布版本一致。
生成头只写 O；源码身份与数字发布版本分开。内核后缀例如::

    make EXTRAVERSION=-perf LOCALVERSION=-test kernel

完整内核版本最多 31 字节，后缀仅接受已允许字符。包元数据仍使用数字基础版本。
SOURCE_DATE_EPOCH 默认为提交时间；不得递增源码计数器或用当前时钟破坏可重现性。
旧输出的 ``config/.config`` 可复制为新输出的 ``.config``，再 olddefconfig。
