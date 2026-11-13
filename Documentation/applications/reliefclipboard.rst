==========================
ReliefClipboard 剪贴板管理
==========================

桌面会话自动启动剪贴板管理器。通过应用菜单、托盘图标或 ``Super+V`` 打开
历史窗口，输入关键字搜索，选择条目后点击“复制到剪贴板”，再切回目标程序粘贴。
关闭历史窗口后，后台服务继续运行。托盘右键菜单可以显示窗口、暂停记录、清空或退出。

支持范围
========

剪贴板使用 X11 ``CLIPBOARD`` selection，鼠标选区 ``PRIMARY`` 独立保留。
支持 UTF-8 文本、图片及文件 URI，同时保留来源提供的其他字节格式。
来源存活时保留其所有权，来源退出后接管已获取的数据。
内置 FLTK 应用在正常退出时使用 ``CLIPBOARD_MANAGER/SAVE_TARGETS`` 交接。
ICCCM ``TARGETS``、``TIMESTAMP``、``MULTIPLE`` 和 ``INCR`` 用于格式协商及大数据传输。

Fileman 使用 ``text/uri-list``、GNOME 文件剪切/复制格式和 KDE 剪切标记，支持
独立实例之间的复制粘贴。URI 按文件路径的实际字节编码，中文、空格、换行与百分号
均可保留。远程 URI 不作为本地路径执行。剪切成功的条目从原剪贴板列表移除，
部分失败保留未移动条目，且仅在剪贴板内容仍匹配原操作时更新。

Paint 支持图片复制和粘贴，现有画布有修改时先询问是否替换。图片粘贴仍遵循
Paint 本身的画布容量。历史窗口提供文本和可解码图片预览，预览容量独立于传输容量。

历史与配置
==========

默认跨登录保存历史，设置窗口可关闭“退出登录后保留历史”。暂停记录会停止添加
历史条目，当前剪贴板的来源退出接管仍可使用。清空历史同时清空当前剪贴板。

* 历史位于 ``$XDG_STATE_HOME/reliefclipboard/history``，默认
  ``~/.local/state/reliefclipboard/history``。
* 配置位于 ``$XDG_CONFIG_HOME/reliefclipboard/settings.conf``，默认
  ``~/.config/reliefclipboard/settings.conf``。``PersistHistory=1`` 启用保存，
  ``PersistHistory=0`` 关闭保存并移除磁盘历史。
* 私有目录权限为 ``0700``，文件权限为 ``0600``。写入通过子进程完成，以临时文件、
  ``fsync`` 和原子重命名提交。解析失败不会把部分读取结果作为完整历史使用。
* 历史最多保存 100 条、128 MiB 数据，单条最多 64 MiB、64 种字节格式，容量满时
  淘汰较旧条目。窗口预览文本最多显示前 4096 字节，搜索检查完整文本。
* X11 资源句柄格式如 PIXMAP 不跨登录保存，图片通过字节格式传输。

命令行
======

``reliefclipboard --daemon`` 启动后台服务，``reliefclipboard --show`` 打开窗口。
每个 X11 display 由标准 manager selection 保证一个管理器。

协议参考
========

* `ICCCM selection 协议 <https://www.x.org/releases/current/doc/xorg-docs/icccm/icccm.html>`_
* `freedesktop 剪贴板约定 <https://specifications.freedesktop.org/clipboard/latest/>`_
* `Clipboard Manager 协议 <https://wiki.freedesktop.org/www/Specifications/clipboard-manager-spec/>`_
