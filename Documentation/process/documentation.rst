==============
编写与构建文档
==============

组织规则
========

现行手册只放在 Documentation/，使用 UTF-8 reStructuredText。
按读者任务组织 admin-guide、applications、kbuild、userspace-api、process，
每个目录提供 index.rst 和显式 toctree。保持段落可读，一页解决一个主题。
标题使用等号上下线，章用等号、节用横线、子节用波浪线；代码用字面块或 code-block。
标题装饰线按显示宽度计算，汉字通常占两列；混排标题的上下线均须覆盖整行标题。
交叉引用使用 doc/ref 角色，避免绝对宿主路径、陈旧构建编号和失效截图链接。

先写目的、前提与可操作步骤，再写接口约束、错误和限制。不把计划、聊天交接、
日志流水账或审查回合当使用手册。历史事实由 Git 与已发布 Release 保存。
AGENT、许可证、上游说明和机器输入按其职责保留，不批量转换。
ReliefNT 的内核手册只在 ReliefNT 仓库维护；主仓手册记录用户态与系统集成。
完整 Pages/RPR 网站从两仓读取文档，发布到独立入口，见 :doc:`../kbuild/products`。

格式依据 `Linux 文档指南 <https://docs.kernel.org/doc-guide/sphinx.html>`_，
只采用适合本项目的 Sphinx/RST 组织，不声称继承 Linux 的全部专用扩展。

构建与检查
==========

宿主安装 Sphinx 5.3 至 9.x（依赖范围在 Documentation/requirements.txt）。
无需交叉编译工具链或配置产品即可构建::

    make O=out/docs htmldocs
    make -C kernel/reliefnt O="$PWD/out/nt-docs" htmldocs

输出分别在所选 O/Documentation/html/。默认采用 Alabaster 导航；页面可离线
阅读并放在任意 URL 前缀下，不加载 JavaScript，不提供依赖 JS 的搜索框。
简体中文是源语言及默认入口，英文位于 en/，日文位于 ja/；各页的语言导航
切换到另一语言的同名页面。网站文档中心同样提供中文、英文和日文入口，
两套手册的链接保持所选语言。既有中文页面地址保持不变。
构建启用 nitpicky 和 warnings-as-errors；缺失交叉引用、未收入目录、错误语法
均须修复，不能通过关闭警告或排除现行页面让检查变绿。
文档生成所需 Python/Sphinx 仅属于文档工具，不进入目标系统编译链。

翻译维护
========

只修改中文 RST 源文档。英文与日文使用 gettext PO，按完整文档路径保存为
``Documentation/locale/{en,ja}/LC_MESSAGES/<docname>.po``，例如
``Documentation/locale/en/LC_MESSAGES/process/documentation.po``。
``gettext_compact = False`` 保证每页独立生成目录，不合并同章消息。
HTML 构建先将 locale 复制到临时输出中再编译，MO 不写回源码目录。

两仓均提供独立的提取和检查目标，无需编译产品::

    make O=out/docs gettext
    make O=out/docs gettext-check
    make -C kernel/reliefnt O="$PWD/out/nt-docs" gettext
    make -C kernel/reliefnt O="$PWD/out/nt-docs" gettext-check

POT 输出到所选 ``O/Documentation/gettext/``，每次提取替换旧模板，避免遗留
已删除页面。维护脚本使用 Sphinx 的 Babel 依赖，不需要另装翻译工具。
也可在各自仓库直接运行::

    python3 scripts/update-doc-translations.py --output out/docs-gettext
    python3 scripts/update-doc-translations.py --update --output out/docs-gettext
    python3 scripts/update-doc-translations.py --check --output out/docs-gettext

默认只提取 POT；显式 ``--update`` 才会初始化或合并英文、日文 PO，保留现有
译文与译者注释，可能的近似匹配标记为 fuzzy，必须人工复核。
``--check`` 按当前源码重新提取模板，只读检查全部 PO；缺页、缺消息、空译文、
fuzzy、过期消息和含中文原文整段照抄均导致失败，不将中文回退视为翻译完成。
纯英文命令和路径允许与原文相同。需要保留原文的中文技术词或日文同形词，
在本仓 ``Documentation/locale/identical.json`` 按语言登记完整 msgid，例如::

    {"en": [], "ja": ["注意", "内容"]}

白名单必须恰好包含 ``en``、``ja`` 两个键，值为不重复的非空字符串数组。
按完整 msgid 精确匹配，不支持模糊匹配或通配；日文例外不用于英文。
``gettext-check`` 与 ``htmldocs`` 预检默认读取同一文件，删除例外后同形译文
会再次检查失败。``--allow-identical '术语'`` 仅为本次 CLI 检查临时添加两种语言
共用的例外，不修改持久白名单。行号与提取时间变化不算译文过期。

``htmldocs``、``gettext``、``gettext-check`` 通过 ``scripts/docs-python.sh``
共用解释器选择：显式非空 ``DOCS_PYTHON`` 优先，否则依次选择 ``SPHINXBUILD``
所在目录的 ``python``、``python3``，均不存在时才使用 PATH 中的 ``python3``。
只设置 ``SPHINXBUILD=/path/to/venv/bin/sphinx-build`` 即可使用该虚拟环境的
Babel；可用 ``DOCS_PYTHON=/path/to/python`` 显式覆盖。直接运行 Python 维护
脚本时，仍需激活相同环境或显式选择该环境的 Python。

完整文档检查无需编译内核或应用，但须初始化所选 ReliefNT 子模块::

    git submodule update --init kernel/reliefnt
    python3 tools/test_documentation.py

此检查通过两仓的 ``make htmldocs`` 入口和实际 Pages 文档入口构建全部页面，
核对每个 RST 页面、站内链接、HTML 锚点和资源，拒绝依赖网站根路径的链接、
JavaScript 及源文档泄漏。只检查现有文档入口可传 ``--site 路径``。
外部网站链接不在此离线检查范围内。

``Check documentation`` 工作流在推送和 PR 时独立检查文档，覆盖 Sphinx 5.3、
Pages 当前使用的 7.2.6 和 9.x，并检查两套手册的完整翻译。Pages 发布工作流也会先运行文档检查，再构建
系统镜像；两处均保留严格警告检查，不以关闭警告绕过失败。
