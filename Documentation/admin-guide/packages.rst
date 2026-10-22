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
