==================
Xorg、XDM 与 IceWM
==================

会话
====

默认图形链为内核、PID 1、Xorg、XDM、IceWM。XDM 完成 PAM 登录后，
IceWM 和应用使用普通用户身份运行；不要用 root 桌面代替正常授权。
终端为 st，浏览器为 Dillo。图形载荷使用锁文件中 ``feature=xorg`` 的依赖；
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

中文与故障排查
==============

原生 C++ 应用使用 FLTK gtk+ 控件、系统中文字体和 UTF-8。
显示字体、键盘布局和输入法是不同配置，见 :doc:`../userspace-api/locale`。
密码输入不能交给第三方输入法。

记录启动固件、CPU/内存、Xorg 日志、用户身份、主题和复现步骤。
通过 VT 切换或串口确认系统是否仍可调度，再区分 Xorg、窗口管理器和应用故障。
编译压力下的拖动与硬关机后的锁文件问题见 :doc:`limitations`。

上游登录管理器的程序名为 ``xdm``；它与 IceWM 的会话和主题配置分别维护。
