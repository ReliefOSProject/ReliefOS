==============
显示与图形接口
==============

设备能力
========

ReliefNT 提供 framebuffer、VT 与有限的 Linux DRM/vmwgfx 路线。
硬件和权限允许时提供 card/render 节点；必须处理缺设备、不支持的 ioctl、
超时和中断。用户属于 video 组并不保证所有 GPU 功能可用。
SVGA-II/VGPU9、同设备 PRIME 与通用跨设备 DMA-BUF 是不同范围。

应用集成
========

X11 应用设置标准 desktop、MIME、图标与 WM_CLASS/_NET_WM_ICON 等适用信息。
FLTK 应用保留系统控件、主题和中文字体；功能内容区不放重复的大程序标题。
窗口、输入与状态更新不得被磁盘操作或网络连接长期阻塞，长操作走 worker。
XDM 与 IceWM 的独立主题策略见 :doc:`../admin-guide/desktop`。

内核显示驱动源码位于 ``kernel/reliefnt/drivers/``，不消费旧中间层实现。
