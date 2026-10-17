# ReliefWord 实现计划

> 必需子技能：使用 subagent-driven-development；用户要求所有子代理 GPT-6-Luna、Max。禁止自行提交或推送。

**目标：** 内置 C++/FLTK 的中文分页文字处理器 ReliefWord。

**架构：** 共享接口固定，编辑模型、文件读写、分页算法各自独立测试，主代理实现 GUI 和系统集成。

**技术栈：** C++17、现有 FLTK 1.3、POSIX 文件接口，禁用异常/RTTI；不增加运行库。

**规格：** `docs/superpowers/specs/2026-10-17-reliefword.md`。

## 全局约束

500,000 字符、50,000 段落、512 样式；外来文件最多 16 MiB。所有编辑失败保持原状态。只改 ReliefWord 及必要系统接入，不改内核、主题、ABI golden、音频语义。工作在现有 rewrite/desktop-apps 分支，不提交推送。临时文件使用 build/reliefword-dev 和 TMPDIR=out/tmp。

## 文件与任务

- [x] 任务 1：`userland/apps/reliefword/model.cpp` 与 `tests/host/test_reliefword_model.cpp`。按 model.h 实现 Unicode 标量、样式、段落、原子编辑、撤销/重做、查找替换、富文本片段；先写失败测试，记录红/绿命令。测试混合中文、非法 UTF-8、跨段编辑、组合删除、样式与剪贴板、撤销选择和失败上限。
- [x] 任务 2：`document.cpp` 与 `tests/host/test_reliefword_document.cpp`。按 document.h 实现版本化原生格式、TXT、HTML 子集、文件戳检测、原子保存和独立恢复文件锁。测试完整往返、实体/转义、脚本忽略、畸形/截断/过限输入、冲突不覆盖、符号链接和恢复并发。
- [x] 任务 3：`layout.cpp` 与 `tests/host/test_reliefword_layout.cpp`。按 layout.h 实现分页、段落和编号布局、字符坐标和命中；测量回调使主机测试不依赖 X11。测试中文标点、拉丁单词、长词、混合字号、组合、分页符、缩进、对齐、页面边界与性能。
- [x] 任务 4：`view.h/.cpp`、`window.h/.cpp`、`main.cpp`，根代理实现所有可见控件、编辑事件、剪贴板、页面与查找对话框、保存和恢复流程。接入 components/build/ownership/menu/icon/l10n/Fileman；添加 `tests/build/test-reliefword.sh` 和集成断言。
- [x] 任务 5：运行主机测试和目标构建；用私有 QEMU 镜像注入最终 ELF/资源，实际编辑中文、格式与页面设置、保存并独立回读、恢复、截图。最后进行覆盖规格和质量的审查，修复后仅重跑相关验证，记录限制。

## 共享接口与审查边界

根代理拥有 model.h、document.h、layout.h、storage.h；实现者修改接口前先协调。各任务只编辑列出的 cpp/测试，不能覆盖其他代理的文件。根代理核对每个任务的规格覆盖和测试，再派独立最终审查。所有实现和审查报告落在 build/reliefword-dev；不依赖上下文记忆。
