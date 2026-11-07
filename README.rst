========
ReliefOS
========

ReliefOS 是面向 x86_64 的操作系统项目，内核位于 kernel/reliefnt 子仓。
默认 Xorg/XDM/IceWM 桌面，支持中文原生应用、纯控制台安装及 BIOS/UEFI 启动。
现行手册从 `Documentation/index.rst <Documentation/index.rst>`_ 开始。

构建入口是根 GNU Make 4.3+，自研源码由固定的 Linux Kbuild/Kconfig 编译。
初始化子模块、显式获取锁定依赖后构建；完整步骤见
`构建手册 <Documentation/kbuild/building.rst>`_。HTML 手册使用::

    make O=out/docs htmldocs

编译、镜像和实际运行分别验证；已知问题见
`支持范围 <Documentation/admin-guide/limitations.rst>`_。VMware 验证未完成。
贡献前阅读 AGENT.md；开源许可见 LICENSE，上游许可与归属保留各自原文。
感谢 VasilyZa 对 Linux ABI、musl 等方面的贡献。

ReliefNT 内核手册独立维护在 kernel/reliefnt/Documentation/；完整 Pages/RPR 网站
在 docs/reliefos/ 与 docs/reliefnt/ 分别提供两份手册。
