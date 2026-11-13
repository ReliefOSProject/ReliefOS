================
语言、区域与资源
================

文本
====

图形应用使用 UTF-8，中文和日语显示依赖随系统 staging 的 CJK 字体，
输入法由会话提供。
剪贴板、路径、文档格式和 GPT 名称分别处理编码，不能逐字节截断 Unicode。
不要把可显示中文等同于完整 Unicode 分词、双向排版或 grapheme-break 支持。

配置与翻译
==========

区域配置位于 ``/etc/reliefos/locale.conf``。应用通过 gettext 加载各自的翻译域，
域名与应用源码目录名一致。``configs/nls/DOMAINS`` 登记翻译域，
``configs/nls/LINGUAS`` 登记随系统构建的语言。

随系统提供 ``zh_CN`` 和 ``ja_JP`` 翻译。用户可在安装器选择日本語，
或在 ReliefSettings 中保存 ``ja_JP.UTF-8`` 并重新登录。
日语翻译包括界面、公共对话框和各应用的错误提示。
第三方应用使用各自的上游翻译资源。

例如，Fileman 的模板位于 ``configs/nls/po/fileman/fileman.pot``，中文译文位于
``configs/nls/po/fileman/zh_CN.po``。``make nls`` 将其编译为
``O/generated/nls/zh_CN/LC_MESSAGES/fileman.mo``，镜像安装到
``/usr/share/locale/zh_CN/LC_MESSAGES/fileman.mo``。Taskmgr 使用独立的
``taskmgr`` 翻译域和对应目录。FLTK 公共按钮与文件选择器文案收录在各自应用的目录中。

修改 Fileman 文案后执行 ``make nls-update-fileman``，可更新该应用的模板和译文。
``make nls-update`` 更新已登记的全部应用。提取范围包含应用的子目录、
公共 FLTK 头文件、Kbuild 中复用的实现及该应用的
``configs/nls/messages/<应用名>.c`` 动态文案标记。
新增应用时登记翻译域，并为 ``LINGUAS`` 中的每种语言创建对应的 PO 文件。
常规构建只生成所选输出树中的资源，翻译源文件由显式维护目标更新。
系统更新时，应用二进制与对应翻译资源一起更新。

日语目录位于各应用的 ``ja_JP.po``，生成和安装目录中的语言名为
``ja_JP``。区域数据位于 ``configs/locale/ja_JP.po``，由
``make musl-locales`` 生成 ``ja_JP.UTF-8`` 并安装到
``/usr/share/musl/locales/``，供 musl 的日期、星期和上午／下午格式使用。
``MUSL_LOCPATH`` 指向该目录，应用仍使用标准的 ``setlocale`` 与 gettext。
显示语言不会自动改变键盘布局，布局由 ReliefSettings 的键盘页单独设置。

自研控制台工具也使用各自的翻译域，包括 driverinfo、soundctl、wavplay、
rprfetch、登录与 Shell 包装程序、rcctl、Snake、Matrix 和登录系统信息。
IceWM 的窗口标题、任务栏和应用菜单通过原生 Xft 字体设置使用 CJK 字体。
应用桌面入口通过标准 ``Name[ja]``、``GenericName[ja]`` 与 ``Comment[ja]``
字段提供日语名称和说明。

原生文件格式的容量、恢复与字符边界见各应用手册。
