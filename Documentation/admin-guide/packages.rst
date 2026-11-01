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

内核与模块更新
==============

``reliefos-kernel`` APK 同时拥有版本化的内核、initramfs、模块树和校验清单。
``reliefos-grub`` 提供静态引导维护工具和标准 APK commit hook；内核包依赖
同一包版本的引导工具，包脚本调用共同的完整载荷发布接口。
不要手工替换 ESP 上的单个 ``kernel.sys``，或把其他内核的模块复制到当前目录。

使用 RPR 时::

    reliefos-kernel-update --check
    sudo reliefos-kernel-update

检查依据是已安装 APK 的版本，发行数字版本相同时的包修订也可识别。
``--force`` 可重试或修复当前签名版本，不改变 world 中普通包名的请求形式。
客户端只接受格式 4：先冻结并验证签名仓库索引，解析依赖，验证全部归档，
在私有目录解包并核对内核、initramfs、模块 ABI 和完整清单，再执行 APK
事务；不从网络自动导入新的信任公钥，也不关闭 TLS 或签名验证。

日常升级通过预分配的 A/B 引导记录发布不可变的成对载荷，保留旧选择记录。
标准 pre-commit hook 在 APK 删除旧包文件前保留独立副本；早期静态恢复工具
可在中断升级后、正常共享库和模块服务运行前恢复当前启动版本的产品。
发布完成后重启才切换正在运行的内核。旧模块、旧入口和用户树外模块不会因
升级而被批量删除；本版不自动清理已保留的历史内核。

缺少新的稳定 GRUB 选择脚本或内核包记录的旧安装，需要先使用匹配的安装器
介质完成引导资源迁移；客户端会在 APK 修改根文件系统前拒绝半套更新。
仅有旧格式 1、2、3 的 RPR 也会被明确拒绝。
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
