==================
系统工具与其他应用
==================

Fileman 与 Taskmgr
==================

Fileman 提供目录浏览、常规文件操作、文件关联和文件名搜索。
左侧设备列表支持可移动卷的发现、授权挂载及安全弹出，操作见
:doc:`../admin-guide/usb`。
文件复制与剪切使用 X11 系统剪贴板，可在独立 Fileman 实例间粘贴，历史窗口见
:doc:`reliefclipboard`。
当前目录使用 inotify 自动刷新，变动会短暂合并；刷新时保留仍存在的选中项，
并尽量保留滚动位置、键盘焦点和正在编辑的地址。监视不递归到子目录；
文件夹树中已展开的当前目录也会同步其直接子目录，其他分支保留原有展开状态。
若监视不可用、权限不足或当前目录被移动、删除，可使用 F5 手动刷新；
状态栏或刷新按钮提示监视不可用。F5 会尝试重新启用监视；监视器可用时，
重新导航也会监视新目录。
搜索默认从当前目录递归到子目录，使用相应开关控制范围；结果仍受当前身份权限限制。
Taskmgr 按其页面查看任务、资源和系统状态，操作进程前确认目标，不能据显示名称
随意终止用户任务。两者使用 C++/FLTK 和系统中文字体。

Taskmgr 自启动管理
==================

“启动应用程序”页合并显示当前用户和系统的 XDG Autostart 项目，列出名称、
状态、来源和命令。点击“添加”创建项目，双击或按 Enter 编辑名称、命令、
描述、工作文件夹、TryExec 条件以及终端选项。命令使用 Desktop Entry 的 Exec
语法，含空格的路径使用双引号；可通过浏览按钮选择程序。更改在下次登录时生效，
编辑操作不会立即启动或停止正在运行的程序。

个人配置位于 ``$XDG_CONFIG_HOME/autostart``，未设置有效绝对路径时使用
``~/.config/autostart``。系统目录依次来自 ``$XDG_CONFIG_DIRS``，默认
``/etc/xdg``。同名 ``.desktop`` 文件按目录优先级覆盖，个人目录优先。
禁用设置 ``Hidden=true``；个人禁用配置同时屏蔽同名系统项目。
系统项目的编辑与禁用写入个人覆盖文件，保留系统原文件。
个人独有项目可删除，存在系统默认值的个人项目显示“恢复默认”；恢复后可能
重新启用系统项目。以 root 运行时可通过用户选择器管理其他普通账户，操作进程
先切换到目标账户身份。其他账户使用其主目录中的默认 XDG 配置路径。

XDM 的 IceWM 登录会话设置 ``XDG_CURRENT_DESKTOP=ICEWM``，并运行
``taskmgr --run-autostart``。因此需要安装 Taskmgr 组件。
启动器按 freedesktop Autostart 与 Desktop Entry 规则处理 ``Hidden``、
``OnlyShowIn``、``NotShowIn``、``TryExec``、``Exec``、``Path`` 和 ``Terminal``，
使用 GIO 展开字段代码和执行参数。``NoDisplay=true`` 仍可自启动。
终端项目通过会话专用的 GIO 适配脚本调用 st。命令以登录账户身份运行，
继承显示和语言环境；会话结束时清理启动器直接创建的子进程，D-Bus 激活的
已有服务由 D-Bus 管理。每个用户、显示会话有独立锁，避免重复执行启动项。

配置扫描与写入由后台进程完成，保存使用临时文件、同步和原子重命名。
修改前检查源文件是否变化。无法解析或缺少程序的项目保留在列表中并标明状态，
可刷新后处理。每次最多读取 2048 项，单文件上限 1 MiB，配置原文总量上限
8 MiB，后台传输上限 16 MiB，超限显示错误。旧版 sessiond 的二进制启动数据库
保留原有 API，Taskmgr 的此页面使用 XDG 配置。

规范见 `Desktop Application Autostart Specification
<https://specifications.freedesktop.org/autostart/latest/>`_ 和
`Desktop Entry Specification <https://specifications.freedesktop.org/desktop-entry/latest/>`_。

其他程序
========

系统还提供 ReliefAPK、图像查看、绘画、扫雷、系统信息及游戏入口。
组件选择决定它们是否安装，菜单入口不表示程序永远存在。
ReliefOS Browser 应用的系统主页为 ``about:reliefos``；X11 默认浏览器为 NetSurf。
``driverinfo`` 是控制台驱动信息工具，默认显示已激活/Loaded，``-a`` 显示全部状态；
以实际设备和驱动状态为准，不把设备枚举等同于成功驱动。
控制台工具与外来软件不需要自研应用桌面图标。
