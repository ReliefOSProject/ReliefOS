==============
编写与构建文档
==============

组织规则
========

现行手册只放在 Documentation/，使用 UTF-8 reStructuredText。
按读者任务组织 admin-guide、applications、kbuild、userspace-api、process，
每个目录提供 index.rst 和显式 toctree。保持段落可读，一页解决一个主题。
标题使用等号上下线，章用等号、节用横线、子节用波浪线；代码用字面块或 code-block。
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
构建启用 nitpicky 和 warnings-as-errors；缺失交叉引用、未收入目录、错误语法
均须修复，不能通过关闭警告或排除现行页面让检查变绿。
文档生成所需 Python/Sphinx 仅属于文档工具，不进入目标系统编译链。
