================
第三方软件与归属
================

来源
====

依赖下载身份由 ``configs/dependencies.lock.json`` 给出，源码子模块由
``.gitmodules`` 和 gitlink 固定。fetch 校验摘要；原生生产目标只消费已验证缓存。
Linux 6.12 Kbuild/Kconfig 的来源与逐文件摘要见 ``scripts/kbuild-source.json``。
不能直接修改上游引擎来绕过项目边界。

``make fetch-check`` 检查锁定下载地址的可用性，不比较上游版本。
依赖升级需要另行核对对应 Alpine 仓库索引或上游正式发布，并同步版本、
下载地址、SHA256、补丁和构建适配中的版本信息。APK 同时验证官方签名；
运行库与开发包采用匹配版本，升级后重新检查构建与运行行为。

当前 GRUB 2.16 的 EFI 路径使用 GOP；上游已移除仅用于旧固件的 UGA 支持，
镜像模块清单不再请求 ``efi_uga``。Legacy BIOS 的引导路径保持独立。

musl、BusyBox、GRUB、认证库等保留各自原生构建与
移植边界。目标程序与开发 SDK 的 staging 不可混用。上游 README、许可证、
COPYING、归属和字体许可是来源资料，保留原文，不属于待清理的过程报告。

musl 的互斥锁属性探测补丁将内核缺少 PI futex 的 ``ENOSYS`` 转换为
POSIX 的 ``ENOTSUP``，以便 PulseAudio 等调用者回退到普通互斥锁。
该补丁不伪装成已实现优先级继承，也不改变其他 futex 错误。补丁及摘要由
依赖锁管理；已有镜像需要重新构建 musl 和系统包才会取得此修正。

ReliefPlayer 的 MP4 解码使用官方 FFmpeg 8.1.3 源码，以 LGPL 配置构建
``avformat``、``avcodec``、``avutil``、``swresample`` 和 ``swscale``。
源码 tag、commit、归档摘要和许可证由依赖锁固定；构建使用独立的 ``-reliefplayer``
库名和 SONAME，避免与用户安装的完整 FFmpeg 运行库发生文件及提供者冲突。
只将选中的播放器所需运行库与 LGPL 原文装入 ``reliefos-media-codecs`` 包，
不向镜像发布 FFmpeg 命令行程序或开发头文件。

移植与打包
==========

添加依赖时同步获取锁、配置/组件、Kbuild 适配、运行路径、包归属、
动态库依赖、桌面入口及许可证。不要仅为安装成功关闭上游验证。
标准控制台工具没有必要伪装成自研图形应用或分配桌面图标。
上游源码修订先在其仓库提交并推送，再更新父仓引用。

预编译基础依赖
==============

sl、ncurses、zlib、libpng、SQLite 和 mbedTLS 3 使用 Alpine v3.24 x86_64 的
原始签名 APK；不再初始化对应的六个源码子模块。准确版本与 SHA256 均在依赖锁中。
运行时 APK 经过 ``upstream-apk`` 校验；``runtime-development`` 仅从锁定且通过
官方签名校验的运行、dev 和 static 包暂存匹配的头文件与库，不将开发树覆盖到来宾。
生产构建离线消费缓存，缺包时先执行 ``make fetch``。

``libreliefos.so.2`` 和 ``libleonos.so.2`` 通过 ELF NEEDED 引用独立的 PNG、zlib、
mbedTLS 库。所有使用方采用 ``mbedtls3-dev`` 的配置头；不能继续定义旧的 ALT 配置，
也不能混用 main 仓库的 mbedTLS 4.x。时钟、熵和线程接口由官方库经 musl/内核获取，
证书链、域名、有效期验证继续生效。

ReliefMail 链接标准 ``libsqlite3.so.0``，不再单独编译 amalgamation 或旧的
``sqlite.so.3`` 自定义 VFS。数据库继续使用 DELETE journal、FULL synchronous、
foreign_keys，并显式设置 ``mmap_size=0``。扩展库的厂商私有符号不是稳定 ABI，
需要使用它们的外部应用应针对独立库及同版本头文件重新构建。

``component:sl``、``component:ncurses``、``component:sqlite`` 锁特性由镜像的
``etc/reliefos/apk-components`` 选择，保留 BUILD/IMAGE 的组件约束；运行库的
基础依赖在控制台和桌面配置中均可用。命令和库由官方包直接提供，不生成本地
包装包、注册表包装入口或迁移说明载荷。离线更新器迁移旧 world 请求并卸载旧包，
保留独立的许可证文本。许可证在 ``resources/licenses``，
SDK 另带原始 ``.PKGINFO`` 记录；源码和构建配方可从对应 Alpine aports 获取。

目标 ABI 使用实际 SDK 在 Linux/QEMU 验证。带 ASan 的 glibc 宿主测试不能链接 musl
静态归档，因此 ``host-runtime-development`` 从锁定的同版本 SQLite/mbedTLS 发布归档
构建原生测试库（使用官方配置）；该目标仅属于测试，不参与系统、SDK 或发行包构建。
可通过 ``RELIEFOS_TEST_DEP_ROOT`` 指向其他 O 的 ``host/runtime-development``。
