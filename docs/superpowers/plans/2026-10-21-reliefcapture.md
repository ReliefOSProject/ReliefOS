# ReliefCapture 实现计划

> 使用 subagent-driven-development 分配独立的集成工作；主要截图与编辑链在当前会话实现。
> 用户已授权直接执行；没有提交、推送、新建分支授权，所有变更留在当前工作区。

**目标：** 交付原生 FLTK 截图和标注工具，Win+Shift+S 可直接选择截图。

**架构：** 几何与文档模型独立于 X11；后台工作进程取得屏幕和窗口列表、编码和原子
保存 PNG；FLTK 覆盖层与编辑窗口负责输入、绘制及剪贴板。IceWM 通过命令入口启动。

**技术栈：** C++17、FLTK gtk+、Xlib、现有 libpng 和无异常容器。

**规格：** `docs/superpowers/specs/2026-10-21-reliefcapture-design.md`

## 全局约束

不修改内核、ABI golden、audio 归属；保留 result/；TMPDIR 使用 out/tmp。
生产入口仅根 Makefile。源码每次修改后检查 diff；不提交构建产物。

## 任务

- [x] 1. 模型与 PNG 保存：创建 `userland/apps/reliefcapture/model.{h,cpp}`、
  `png_io.{h,cpp}`，以及 `tests/host/test_reliefcapture.cpp` 和 shell 入口。
  先以反向拖动、边界裁剪、画笔点数限制、撤销重做、PNG 回读、冲突/链接保护为断言，
  执行 `TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefcapture.sh` 看到缺失功能失败，
  实现后执行普通和 ASan/UBSan 版本。
- [x] 2. 后台截图与会话：创建 `capture.{h,cpp}`、`job.{h,cpp}`、`instance.{h,cpp}`。
  约定屏幕 RGB32、最多 512 个带框窗口；后台通道按头部、窗口列表、像素顺序发送，
  父进程每轮最多读取 256 KiB，超时/错误/取消后回收子进程。
  以真实 Xvfb/Xorg 验证 RGB 内容、窗口边界、重复实例通知和取消。
- [x] 3. 原生 GUI：创建 `overlay.{h,cpp}`、`window.{h,cpp}`、`main.cpp`。
  覆盖层在冻结画面上显示原生选择条，处理拖选、窗口高亮、全屏、Esc；编辑区使用
  同一套标注绘制用于屏幕与导出，保留原始像素，限制内存和操作历史。
- [x] 4. 桌面/镜像集成（可并行）：组件 TOML/Kconfig、FLTK 清单、APK 归属、
  独立图标、`.desktop`、IceWM 快捷键和组件/归属测试。该任务不修改 GUI/模型/翻译。
- [x] 5. 翻译与文档：补中文翻译、`docs/apps/reliefcapture.md`，按真实实现记录快捷键、
  使用方式、文件与剪贴板语义。运行 NLS、组件、图标及 rootfs/installer 检查。
- [x] 6. 审查与验收：独立审查代码，修正有证据的问题；执行
  `TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug -j4 app-reliefcapture nls image-vmdk`。
  创建 `tools/test_reliefcapture_qemu.py`，在私有基线副本中验收真实快捷键、各截图模式、
  标注和 PNG；核对最终镜像文件，保存真实截图。总测试已有失败单独报告。

## 进度记录

2026-10-21：工作树开始时只有未跟踪的 result/，当前分支 rewrite/desktop-apps。
已读取现有应用、IceWM、构建与测试接入方式。按上述拆分直接执行。

已完成模型和 PNG 红/绿测试、ASan/UBSan、真实 Xvfb 截图/几何/异步传输/取消及错误
回归。组件、图标、APK 归属、翻译、userland 构建图、rootfs 和 installer 检查通过。
首次应用与 VMDK 构建通过；Xlib 直接调用要求补齐已有 Alpine libX11 的 build-only
staging，同时新增了开发库链接解析回归测试。没有新增下载身份或修改内核。

独立 GPT-6-Luna Max 审查发现：保存对话框期间转发截图可替换文档、后台读取屏幕时
取消入口不可达。已用显式对话框计数/排队及仅捕获 Esc 的被动 X11 键位抓取修正，
第二轮 QEMU 同时检查。针对已有 PNG 的跨进程覆盖，保留普通 POSIX 检查后原子替换
语义，并将规格和使用文档的并发保证表述精确化；不声称提供 inode 条件式原子提交。

QEMU 第一轮已真实验证 Win+Shift+S 调用、区域截图、剪贴板 BMP 和首张 PNG 保存；
自动化在后续保存对话框尚未完成绘制时定位失败，该轮不计为完整验收。
已将后续流程改成等待编辑器/对话框绘制和实际标注计数，并优化选区与预览缓存。

仓库总测试已执行，仍在既有 `component policy mismatch: ['soundctl']` 处失败；
该音频归属策略保持不变。VMware 验证未完成。

用户进一步要求功能性窗口不显示程序大标题：已移除编辑区大标题、介绍横幅与大字号
空白页提示，工具栏上移，画布扩大。QEMU 第三轮已实际操作四种标注并截图新布局。
原自动化使用 Home 重置 FLTK popup 选项并不成立，已根据实际选中项按差值导航。
第三轮又暴露文本对话框后的自定义全局快捷键分发不可靠，已改用仓库其他 Office
应用的窗口级 handle 路径。第四轮正在验收最终窗口与标注/复制/保存流程。

第四轮进一步复现了 IceWM 默认 Shift+Esc / Alt+Esc / Ctrl+Alt+Esc 与 AnyModifier
请求的冲突。已将 Esc 抓取拆到 keyboard 模块，只请求普通 Esc 和 Caps/Num Lock
等价组合；Xvfb 使用独立 X11 客户端预先抓取 IceWM 的组合键，红/绿回归已通过。
同时保留首轮截图错误的可见状态，不再显示错误后立即退出。第五轮验收当前版本。
窗口内容区不放程序大标题的用户偏好已补入 AGENT.md，供后续功能性应用遵循。

## 最终验收记录

最终应用 ELF SHA256：
`9789efcde661289e027f1d88e7ae45293824a0258cb19f3c6db0965c783f03f5`。
应用、中文翻译、raw/VMDK 构建通过，日志为
`out/reliefcapture-final-acceptance-build.log`。从最终文件系统镜像读回 ELF、MO、
desktop、BMP、六种 PNG 图标和 IceWM keys，与构建输入逐项一致；launcher、manifest
检查通过，raw 和 VMDK 的磁盘内容比较一致。结果为
`out/reliefcapture-final-image-readback/result.json`。

模型/PNG 保存、ASan/UBSan、真实 Xvfb 截图/异步 IPC/取消、组件/归属/图标、翻译、
userland 构建图、rootfs 和 installer 检查通过。增量修复包括次实例在退出前完成
XSync 转发、保存关闭期间的新截图请求撤销关闭意图，以及隐藏窗口前预留 Esc 并
接收已排队到旧 XID 的 Esc。后者新增 Xvfb 回归，先在旧实现断言失败，再在修复后通过；
日志为 `out/reliefcapture-hidden-escape-{red,green}.log`。

最终 QEMU 使用 `out/rc-q10/guest-private.raw`，注入并读回核对的程序与上述最终 ELF
相同。实际完成普通用户 Win+Shift+S、区域/窗口/全屏、四种标注、撤销重做、标注后
复制和保存、保存对话框期间排队、重复快捷键、延时出图/取消、隐藏瞬间立即 Esc、
正常退出及重新启动后取消后台读取。截图尺寸分别为 641×361、716×484、1280×800。
五张 PNG 均从来宾磁盘独立读回；区域图与稳定桌面参考的像素差异为 0，标注改变
4614 个像素，独立 X11 客户端接收的标注 BMP 与 PNG 像素完全一致。基线镜像哈希
保持不变。结果为 `out/rc-q10/result.json`，真实界面截图位于同目录。

本轮使用交互调试入口恢复了两项测试时序：启动前的小字 OCR 判定改为等待 fastfetch
退出及画面稳定；关闭后等待 IceWM 恢复焦点再调用新实例。修正后在同一私有虚拟机
从首个截图步骤重新执行，并补齐退出后的新实例检查，最后完成独立磁盘回读；不是
一次无中断的自动化运行。此前轮次的菜单定位、文件名预填及尚未绘制的桌面参考等
测试问题保留在对应日志，不作为通过记录。剪贴板探针现对相同图像去重，避免长时间
交互调试重复写入 BMP 占满私有测试盘；回读报告记录实际来宾文件名。

仓库总测试仍在既有 `component policy mismatch: ['soundctl']` 处失败，日志为
`out/reliefcapture-root-tests.log`；音频归属策略未修改。新默认 VMDK 已构建并核对内容，
未直接启动验收。VMware 验证未完成。内核子仓未修改，result/ 保留；没有提交、推送
或创建分支。
