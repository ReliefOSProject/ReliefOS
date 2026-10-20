# ReliefDiagram 实现计划

> 在当前已核实干净的开发工作区内联执行；用户已要求直接实施，不设置批准关卡，不自动提交。

**目标：** 交付可在 ReliefOS 运行的原生思维导图与流程图编辑器及真实截图。

**架构：** 独立模型、版本化文档、共用矢量场景、FLTK 画布和 Office 布局；复用
ReliefTodo 的有界容器和异步原子文件操作。PNG 编码交由子进程。

**技术栈：** C++17、FLTK 1.3、libpng、GNU Make。

## 1. 模型和可编辑文档

- [x] 创建 `userland/apps/reliefdiagram/{storage.h,model.h,model.cpp,document.h,document.cpp}`。
  `Document` 管理节点、连线、父子关系和视图；`History` 管理快照。
- [x] 先编写 `tests/host/test_reliefdiagram.cpp` 与 `tests/build/test-reliefdiagram.sh`：
  子主题布局不重叠、折叠不丢失节点、父子循环拒绝、删除清理引用、撤销恢复文本、
  文档双向读写、CRC/截断/无效 UTF-8 拒绝且保留原文档。
- [x] 运行测试观察缺失行为，再实现并运行至通过。命令：
  `TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefdiagram.sh`。

## 2. 几何与导出

- [x] 创建 `scene.h/scene.cpp`；节点边界、连线端点、箭头与文本换行共用几何。
- [x] 添加 SVG XML 转义、隐藏分支、图形命中、完整内容边界和 Markdown 缩进测试，
  运行失败后实现；不使用被测函数生成预期结果。
- [x] 创建 `export.h/export.cpp`：FLTK 场景绘制和无网格 PNG，编码子进程有界轮询。

## 3. 原生编辑界面

- [x] 创建 `main.cpp,window.h,window.cpp,actions.cpp,canvas.h,canvas.cpp,files.cpp`。
- [x] 实现菜单、快捷工具、分组工具页、图形库、对象清单、属性和画布尺寸布局。
- [x] 实现单击/框选/多选、拖动、缩放、平移、连接、双击编辑与尺寸手柄；
  将每次拖动作为一个快照，快捷键不拦截输入框文本编辑。
- [x] 实现新建/模板/未保存提示/异步打开保存/冲突提示/导出、错误恢复和关闭清理。

## 4. 系统集成

- [x] 添加 desktop/MIME；更新 `configs/components.toml`、FLTK 清单、APK ownership、
  图标清单与生成器、fileman 关联及相关 fixture。生成 `Kconfig.components`。
- [x] 加入 Office 分组，更新中文目录和使用文档 `docs/apps/reliefdiagram.md`。
- [x] 运行模型、组件、元数据、文件关联、图标、desktop 和翻译检查；
  `git diff --check`。运行根测试并单独记录已有失败。

## 5. 构建与实际验收

- [x] `TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug -j4 app-reliefdiagram nls image-vmdk`。
- [x] 只读检查最终 root 镜像中的 ELF、desktop、MIME 和图标，比较 RAW/VMDK。
- [x] 使用基线私有磁盘副本注入最终程序、目录与内核；QEMU 真实验证：
  思维导图/流程图模板、编辑/拖动/连接、撤销重做、保存重开、SVG/PNG/大纲、正常关闭。
- [x] 独立读取来宾保存文件并验证内容，检查日志，查看真实截图。
- [x] 更新本清单实际状态，交付截图和验证边界；明确 VMware 验证未完成。

## 验收记录（2026-10-20）

- 核心测试、ASan/UBSan、组件/元数据、文件关联、图标、desktop、翻译与应用归属检查通过。
- 目标程序、NLS 和 VMDK 构建成功；最终根分区与 ext4 匹配，RAW 与 VMDK 内容一致；程序、MIME、desktop、图标、Office manifest 和启动链接已只读核对。
- 最终 ELF SHA256：`d29c9d7e01db9559573288f9a3259ddc01e34fafadf94be8d66cc4e446fbd696`。
- QEMU 使用基线私有副本，注入同一最终 ELF 和内核；已验证 Ctrl+Enter 应用、分支折叠、连接标签、移动、菱形缩放、复制粘贴、撤销重做、保存重开、损坏文档拒绝、三种导出和三次正常关闭。六个来宾文件已独立读回核对，原始基线未修改。
- 本轮首次 Xorg 启动在应用启动前黑屏；保留失败启动日志，重启该私有来宾后完成验收。证据：`out/rdgm-qemu5/result.json`、同目录截图、`out/reliefdiagram-final-image-readback/result.json`。
- 根 `make test` 仍在既有 `component policy mismatch: ['soundctl']` 处失败；未改变该策略。
- 默认生成的 VMDK 未直接启动验收。VMware 验证未完成；硬件未验证。
- 保留当前分支和所有未提交改动，不提交、不推送。
