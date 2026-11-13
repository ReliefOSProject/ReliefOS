==================
Xorg、XDM 与 IceWM
==================

会话
====

默认图形链为内核、PID 1、Xorg、XDM、IceWM。XDM 完成 PAM 登录后，
IceWM 和应用使用普通用户身份运行；不要用 root 桌面代替正常授权。
终端为 st，默认浏览器为 NetSurf。图形载荷使用锁文件中 ``feature=xorg`` 的依赖；
当前 Alpine 基线为 ``v3.24``。从源码准备依赖使用 ``make fetch``。
首次配置及真实运行测试必须区分 QEMU 与 VMware。

Xorg 自动探测设备。在具备已实现 DRM 能力时使用 modesetting/glamor，
否则保留 framebuffer 回退；默认软件光标与关闭 page flip 的策略不能被误读为
关闭全部 DRI3/Present。检查实际 Xorg/Mesa 日志与绘制结果，不能只看显卡名称。

主题与菜单
==========

XDM 使用 ``/etc/X11/xdm/`` 下的标准配置和 Xresources；IceWM 的背景、
颜色和装饰由 IceWM 主题决定。两者分别配置，不再强制使用同一张背景图片。
系统 IceWM 配置由 ``/etc/icewm`` 及 ``/etc/reliefos/icewm`` 提供，
用户的 ``~/.icewm/`` 可覆盖系统配置。主题不能替代菜单、工具栏与键位文件。

修改键位或会话配置后需要重新加载 IceWM 或重新登录。自定义菜单应保留按功能
分组的应用入口。Win+Shift+S 启动 ReliefCapture；用户自定义 keys 覆盖默认键位时
需自行合并该绑定。

用户目录与快捷方式
==================

登录会话运行上游 ``xdg-user-dirs-update``，按用户语言初始化 Desktop、Documents、
Downloads 等目录。安装器和账户种子只准备主目录与配置模板，不再预建另一套目录。
Fileman 及下载路径读取 ``$XDG_CONFIG_HOME/user-dirs.dirs``；未设置有效的
``XDG_CONFIG_HOME`` 时使用 ``~/.config/user-dirs.dirs``。桌面由
``XDG_DESKTOP_DIR`` 决定，下载位置由 ``XDG_DOWNLOAD_DIR`` 决定。未配置或
禁用的特殊目录回退到主目录。配置只按路径数据读取，不执行其中的 shell 代码。

Fileman 的“创建快捷方式”可选择桌面或当前目录：应用使用标准 ``.desktop``，
已有应用启动项保留名称、图标、文件参数和终端设置；普通文件与文件夹使用 POSIX
符号链接。创建操作不覆盖同名文件，自动选择新的名称。打开应用启动项时复用系统
GLib ``gio launch``，由上游处理 Exec 转义、字段码、工作目录和终端启动规则。
IceWM 的应用菜单继续通过 ``icewm-menu-fdo`` 读取标准应用入口。

``~/.icewm`` 是窗口管理器配置目录，桌面图标的显示仍需独立图标管理器；创建
快捷方式不会自动增加桌面图标服务。更新不会删除或移动用户已有文件或个人目录。
旧版私有快捷方式不再解析；已有的此类快捷方式需重新创建为标准启动项或符号链接。

中文与故障排查
==============

原生 C++ 应用使用 FLTK gtk+ 控件、系统中文字体和 UTF-8。
显示字体、键盘布局和输入法是不同配置，见 :doc:`../userspace-api/locale`。
密码输入不能交给第三方输入法。

记录启动固件、CPU/内存、Xorg 日志、用户身份、主题和复现步骤。
通过 VT 切换或串口确认系统是否仍可调度，再区分 Xorg、窗口管理器和应用故障。
编译压力下的拖动与硬关机后的锁文件问题见 :doc:`limitations`。

上游登录管理器的程序名为 ``xdm``；它与 IceWM 的会话和主题配置分别维护。

源码与兼容边界
==============

系统只交付 Xorg、XDM、IceWM 和已迁移的图形应用。旧自研桌面、私有窗口
传输、对应 SDK 窗口头文件和 ELF 窗口接口均已删除，不提供兼容服务或空实现。
内核保留标准 framebuffer、DRM 和 evdev 设备接口，不再包含内置桌面绘制。

尚未迁移的图形客户端源码继续保留在 ``userland/apps/``，但不在组件菜单、
构建清单、安装包或启动菜单中。终端使用 st，浏览器使用 NetSurf。旧的设置、
设备管理及协议演示程序需迁移到 X11 后才能重新加入；保留源码不表示它们
仍可编译或运行。第三方上游源码及其许可证保留，旧窗口平台适配层已移除。

这是旧图形 API 的破坏性移除。现有应用需迁移至 FLTK/X11；进程快照、
CPU 亲和性和单调时钟声明位于 ``<reliefos/system.h>``，独立 framebuffer
辅助接口位于 ``<reliefos/fb.h>``。重新生成 SDK 并重新链接应用，不要混用旧库。
