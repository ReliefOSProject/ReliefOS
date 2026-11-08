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

musl、BusyBox、SQLite、FLTK、Xorg/Mesa、GRUB、认证库等保留各自原生构建与
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
