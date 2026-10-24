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

Dillo 使用标准 ``~/.dillo/dillorc`` 和 ``~/.dillo/style.css``。新账户从
``/etc/skel/.dillo/`` 获得默认文件；已有账户登录 XDM 时只补缺失文件，已有
个人配置、空文件与符号链接均不覆盖。也可在自己的会话中运行
``/usr/lib/reliefos/dillo-config-init`` 补齐缺失文件，然后重新启动 Dillo。

默认正文与界面字体为宋体（SimSun），代码区为新宋体（NSimSun）。配置使用
FLTK 1.3 稳定的内置字体名，避免中英文字体族名随 locale 改变而未被识别。
``/etc/fonts/conf.d/09-reliefos-dillo-cjk.conf`` 只将 Dillo 请求的通用字体映射到
这些中文字库，不改变其他应用或 Dillo 明确指定的自定义字体。
由于现有 FLTK 1.3 绘制路径不提供逐字缺字回退，用户样式以 ``!important``
固定字体族，覆盖网页指定的西文字体；网页的字号、粗斜体、颜色和布局仍保留。
Dillo 只从用户目录读取该样式，不能改放到 ``/etc/dillo/style.css``。
已有自定义配置的用户可自行合并模板中的字体规则；要恢复网页原有字体，
编辑自己的样式文件删除或调整这两条规则。清空该文件也会保留用户选择。

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
构建清单、安装包或启动菜单中。终端使用 st，浏览器使用 Dillo。旧的设置、
设备管理及协议演示程序需迁移到 X11 后才能重新加入；保留源码不表示它们
仍可编译或运行。第三方上游源码及其许可证保留，旧窗口平台适配层已移除。

这是旧图形 API 的破坏性移除。现有应用需迁移至 FLTK/X11；进程快照、
CPU 亲和性和单调时钟声明位于 ``<reliefos/system.h>``，独立 framebuffer
辅助接口位于 ``<reliefos/fb.h>``。重新生成 SDK 并重新链接应用，不要混用旧库。
