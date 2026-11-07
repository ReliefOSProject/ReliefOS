===================
ReliefMath 数学渲染
===================

本文件记录实现选择与实际证据；目标 GUI 及虚拟机运行仍待验收。

选择依据
========

已确认规格将 MicroTeX 固定候选设为 ``0e3707f6dafebb121d98b53c64364d16fefe481d``。实际下载该提交并隔离检查，默认主机 CMake 因缺少 ``gtksourceviewmm-3.0`` 失败；其 GNU 构建采用 C++20，并依赖标准 C++ 库、异常、filesystem 和 GTK C++ 图形实现。使用本项目现行 C++17、``-nostdinc++ -fno-exceptions -fno-rtti`` 标志交叉编译 ``src/core/core.cpp``，实际在 ``cstring`` 头处失败。证据保留于 ``out/tmp/reliefmath-backend/cmake-host.log`` 和 ``microtex-target.log``。这不是声称上游不能移植，而是本版本没有为公式工具引入完整 GTK/C++ 开发栈。

因此采用规格允许的本地受限递归下降解析器、数学盒模型和扁平字形显示列表，使用 FreeType 与 Cairo 的 C 接口。GUI 不链接数学后端；独立 ``reliefmath-render.elf`` 只接受有界源码协议，不运行 shell、网络请求或 TeX 命令。支持范围见 兼容清单。

字体与依赖
==========

数学字体为 STIX Two Math，来源 ``stipub/stixfonts`` 的 ``v2.13b171``，原文件路径 ``fonts/static_otf/STIXTwoMath-Regular.otf``。仓库字体 SHA256 为 ``3a5f3f26f40d5698b3c62dd085d48d6663696a3f80825aab8b553d5097518e8c``，完整 SIL OFL 随资源保留。字形缺失时回退到系统现有 SimSun；中文也通过 FreeType 提取轮廓。两者仍无字形则明确报错，不生成缺字符号冒充成功。

只使用 STIX 字体，不分发 MicroTeX 的代码或资源。目标路径为 ``/usr/share/reliefmath/STIXTwoMath-Regular.otf``，中文使用 ``/usr/share/fonts/reliefos/simsun.ttc``；许可证装入 ``/usr/share/licenses/reliefmath/``。

Cairo ``1.18.4-r1`` 与 FreeType ``2.14.3-r0`` 的开发 APK 使用 ``configs/dependencies.lock.json`` 固定 URL 与 SHA256，通过显式 ``make fetch`` 获取。构建脚本使用系统信任密钥验证 APK 签名并校验摘要，再以临时完整树发布到专有开发目录；不覆盖共享 X11 开发树。运行时使用系统已有的相同版本 APK。没有改变全局 C++ 编译标志。

渲染与输出
==========

公式布局生成字形、伸缩和横线的同一条路径。SVG 只写出 ``svg/g/path``，所有字形（包含中文）都是真实曲线；没有位图、``text``、脚本或外部链接。PNG 由同一条 Cairo 路径填充到透明图层，预览由其直通 RGBA 数据生成。SVG 与 PNG 尺寸、边距和基线相同。

解析前检查 UTF-8、64 KiB 源码与 128 层嵌套限制；输出前检查 16384 单边和 16777216 总像素限制。未知命令、环境、宏定义、文件访问或字体缺字均返回错误和源码字节范围。
