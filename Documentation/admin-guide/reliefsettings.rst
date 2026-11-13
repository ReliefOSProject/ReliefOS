==============
ReliefSettings
==============

ReliefSettings 是 C++/FLTK 系统设置中心，可从 IceWM 的设置菜单或
``reliefsettings`` 命令启动。应用使用独立的 ``reliefsettings`` gettext
翻译域，随系统提供中文资源。

语言、显示与键盘
================

用户设置保存在 ``$XDG_CONFIG_HOME/reliefos/``。变量为空或使用相对路径时，
采用 ``$HOME/.config/reliefos/``。新建目录模式为 0700，配置文件模式为
0600。保存使用 POSIX 文件接口、同目录临时文件和原子重命名，并检查打开
设置后是否已有其他程序修改文件。桌面操作使用独立的 POSIX 建议锁，防止
多个设置窗口同时预览和恢复显示模式或键盘布局。

``locale.conf`` 使用 ``LANG=zh_CN.UTF-8`` 等 locale.conf 赋值语法。
选择“系统默认”移除用户的 LANG 覆盖，现有其他区域类别赋值保留。登录
Shell 与 XDM 会话导入经过验证的 LANG 和 LC_* 值，系统默认值仍来自
``/etc/reliefos/locale.conf``。语言修改在下次登录生效。

``desktop.conf`` 是 ReliefSettings 的键值配置文件。XKB_LAYOUT 和
XKB_VARIANT 记录 XKB 规则中的布局、变体标识。DISPLAY 行由空格分隔
输出名称、模式名称和刷新率。同一输出只能出现一次。下面给出配置示例。

.. code-block:: text

   XKB_LAYOUT=us
   XKB_VARIANT=intl
   DISPLAY=Virtual-1 1280x720 60.00

键盘布局和变体从系统安装的 XKB ``evdev.lst`` 读取，通过原版
``setxkbmap`` 应用。选择系统默认清除下次登录的覆盖。中文输入由会话的
输入法提供。

显示模式通过原版 ``xrandr`` 读取和应用，只提供 X server 实际报告的模式，
仅调整已启用输出的分辨率与刷新率。独立子进程执行预览、15 秒确认和
恢复，关闭应用或预览超时都会恢复原模式，确认后才保存。恢复失败会显示
诊断。登录时只恢复仍连接、已启用且仍提供该模式的输出。

配置文件作为数据解析，登录脚本通过验证后的赋值导入环境。
若原子重命名已经完成、随后目录同步失败，界面保留已应用的设置并报告同步
错误。诊断窗口支持滚动查看命令输出。重启前应处理存储错误并重新保存。

系统时区与管理入口
==================

时区列表来自 IANA tzdata 的 ``/usr/share/zoneinfo/zone.tab``，可输入城市或
区域名称筛选。应用通过临时符号链接及原子重命名更新 ``/etc/localtime``，
指向已安装的 TZif 文件。系统时钟本身不受影响。新的进程使用新时区，
已有进程的缓存更新时间由其 C 库与应用决定。

普通用户修改时区时由上游 sudo 与 ReliefAskpass 完成认证，设置中心执行
固定的 ``--set-timezone`` 辅助入口。该入口要求有效 UID 为 root，只接受
时区标识，目标目录固定为 ``/etc`` 与 ``/usr/share/zoneinfo``。

网络和服务页启动已安装的 Network Controller 与 OpenRC Controller，
各管理器负责自己的认证和操作。相应组件未安装时入口禁用。

构建配置
========

``APP_RELIEFSETTINGS`` 组件默认启用，可分别控制构建、镜像成员和入口。
原版 xrandr、setxkbmap 通过 Xorg APK 集合安装，tzdata 通过基础 APK 集合
安装。其下载身份固定在依赖锁中。

``make app-reliefsettings nls`` 构建应用与翻译。定向配置测试为
``sh tests/build/test-reliefsettings.sh``。
