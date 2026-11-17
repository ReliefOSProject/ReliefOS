============
软件包与证书
============

命令行
======

系统使用真实的上游 apk 命令、数据库、签名和依赖关系。
只读查询无需把桌面作为 root 启动；修改系统包通常需要管理员权限。
在 ReliefOS 终端中可使用::

    apk info
    apk info --who-owns /usr/bin/fastfetch
    sudo apk update
    sudo apk add gcc make
    sudo apk del package-name

软件源位于 ``/etc/apk/repositories``，world 和公钥按 apk 的标准路径管理。
不要为解决包冲突删除仍在使用的 libc、认证或基础包，也不要关闭签名验证。
外部软件存在 Linux ABI 依赖，安装成功不代表全部功能已验证。

基础命令由完整的 ``reliefos-coreutils`` GNU 包与 BusyBox 按路径分工提供。
同名外部命令使用 GNU 入口，旧路径由 APK 管理并转发；详情与验收方法见
:doc:`coreutils`。

基础依赖
========

sl、ncurses、zlib、libpng、SQLite、mbedTLS 3 由签名 Alpine APK 提供。
系统直接请求官方命令、终端工具/数据和 SQLite 命令/运行库，不再生成本地包装包。
文件所有权可通过
``apk info --who-owns /usr/bin/sl``、``/usr/bin/tput``、``/usr/lib/libsqlite3.so.0`` 查询。
离线更新器在预检时将旧包装包的 world 请求转换为官方包，保留其他包和配置；
正式事务自动卸载旧包，完成后清除离线缓存中的旧包装包归档。
新的自有包使用第 7 代版本前缀，保证从第 2、3、4、5、6 代包按正常版本顺序升级。
旧的 ``sqlite.so.3`` ABI 已移除。
依赖旧自定义 SQLite VFS 或厂商内部符号的外部程序需要重新编译。

libmd 1.3.0、libbsd 运行库，以及 dosfstools、e2fsprogs 工具及其配套运行库和文档，
均直接使用锁定的官方 Alpine APK。
ext2/3/4 和 FAT 工具保留官方 ``/sbin`` 路径，旧 ``/usr/sbin`` 入口由兼容链接转发。
libmd 保留官方 ``/usr/lib`` 路径，旧 ``/lib`` 入口由本地认证支持包中的单向链接转发。
本地文件系统包继续提供 exfatprogs；兼容包通过真实 APK 依赖安装官方库和工具，
升级时同时转移旧文件的所有权，不强制覆盖。
shadow 构建使用独立提取的 libbsd 和 libmd 开发文件，运行镜像不包含 libmd 的头文件、
静态库和 pkg-config 文件，也不把 Alpine 的 Linux 头文件混入 SDK。

util-linux 2.42.4 的命令、运行库和手册由锁定的官方 Alpine edge APK 提供。
分区和文件系统工具保留官方 ``/sbin`` 路径，mount、umount、lsblk 保留 ``/bin`` 路径；
旧入口通过单向兼容链接转发。exfatprogs 的开发依赖从官方 APK 单独提取，运行镜像不包含
这份开发根。现有 shadow login、newgrp、nologin 和 PAM 策略由本地认证包明确接管重叠文件，
官方包保留 su、runuser 的文件归属和权限。

liblastlog2 依赖 SQLite 运行库，因此桌面和控制台基础系统均保留 sqlite-libs；
SQLite 命令仍由组件开关控制。

官方 lsblk 2.42.4-r3 的直接探测路径未请求文件系统版本，因此 ``FSVER`` 列可能为空。
本版保留该原版行为；读取真实版本可使用::

    sudo blkid -p -s VERSION -o value /dev/sda2

RPR 本地发布树继续包含完整的签名索引、原始上游 APK 和自有包，构建
``rpr-pages`` 不等于部署到线上。运行库因新增 ELF NEEDED 而得到真实 APK
依赖，不能只复制新的 ``libreliefos.so.2`` 而遗漏配套库。

内核与模块更新
==============

``reliefos-kernel`` APK 同时拥有版本化的内核、initramfs、模块树和校验清单。
``reliefos-grub`` 提供静态引导维护工具和标准 APK commit hook；内核包依赖
同一包版本的引导工具，包脚本调用共同的完整载荷发布接口。
不要手工替换 ESP 上的单个 ``kernel.sys``，或把其他内核的模块复制到当前目录。

在线整机更新使用原生客户端::

    sudo reliefos-update prepare
    sudo reliefos-update apply --plan <计划 ID>
    sudo reliefos-update status

命令行和 ReliefAPK 的“升级系统”共享相同的持久计划、签名索引、依赖解析、
缓存和提交检查。更新包括已安装的系统包、上游依赖与应用；保留 world 中的
版本和仓库标签约束，提交阶段不重新从移动的软件源选择版本。

内核更新必须同时校验内核、initramfs、模块 ABI 和完整清单，再发布 A/B
引导记录。客户端使用已安装的信任密钥，不从网络自动导入新的公钥。
旧安装缺少匹配的稳定 GRUB 和启动确认资源时，应先通过安装介质完成迁移。

更新中断后使用 ``sudo reliefos-update status`` 检查阶段，并通过 ``resume`` 或
``repair`` 显式恢复。用户态包原地更新；内核回退不能代替整个系统快照。
完整操作、缓存与空间要求、启动确认和失败处理见 :doc:`system-updates`。
模块配置与日常管理见 :doc:`kernel-modules`。

ReliefAPK
=========

ReliefAPK 是 apk 的原生 C++/FLTK 前端，提供包查询、安装、删除、升级、
修复及仓库配置等入口。应用修改前先审阅计划和输出；长任务在子进程执行。
普通用户可在程序内通过已有 sudo/root 认证流程执行特权事务。
取消、认证失败或 apk 失败必须保留明确状态，不能把退出码或空输出当成成功。

信任与更新
==========

自建系统的签名私钥保存在宿主用户目录，不属于发行内容。升级要沿用目标
系统已信任的公钥；更换密钥需要独立、经认证的轮换流程，不能从更新介质自动信任。

ReliefMail 的证书修复通过 ReliefAPK 预览并更新 ``ca-certificates`` 和
``ca-certificates-bundle``。损坏的安全常规文件可先备份并恢复出厂 CA；
修复仍须检查签名、TLS、权限和最终证书内容。不要关闭域名、有效期或证书链验证。
生成目录 ``/etc/ssl/certs`` 与管理员自定义源、apk 保护规则需人工协调。
细节见 :doc:`../applications/reliefmail`。
