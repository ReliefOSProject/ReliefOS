# Kbuild 迁移后的同分支更新集成计划

> 使用现有 Kbuild 迁移规格和完成前验证流程执行；用户已授权合并同分支更新并迁移新增规则。复用现有隔离工作树，不提交、推送或创建命名分支。

**目标：** 在保留两仓未提交 Kbuild 迁移的同时，集成 `rewrite/desktop-apps` 的 ReliefPlayer 和 ReliefCapture 提交。

**架构：** 父仓基线快进到 `00d2c680bc9c96070d116c9df1f925c0bcb66910`，自研生产编译继续使用 Linux 6.12 原版 Kbuild。两个应用增加目录 Kbuild；X11 库成员和 minimp3 参数迁入产品适配层。上游程序源码、翻译、组件选择、资源与测试语义保留。

**技术栈：** GNU Make、固定 Kbuild/Kconfig、Clang/LLVM、musl、FLTK、Xlib、libpng、现有音频接口。

**规格：** `docs/superpowers/specs/2026-10-21-kbuild-migration-design.md`，范围仍为自研生产源码和宿主工具；独立宿主测试夹具维持现有测试边界。

## 约束

- 原工作区的六项既有状态保持不变；ReliefNT HEAD 仍为 `10b489986baeece8f89520e975beeb88d47b29b4`。
- 同分支两个新增提交：`e67a0a0d`、`00d2c680`；只将原有迁移重新叠加到它们之上。
- 修改前已备份父仓 366 项、子仓 170 项迁移状态、补丁、当前文件归档和源码摘要，位于 `out/kbuild-upstream-sync/`。
- TMPDIR 和 QMP 位于工作树 out；一次只启动一个 4 GiB VM。构建日志重定向保存，不过滤构建管道。
- 不重生成 ABI golden，不修改音频 manifest 策略，不把 soundctl、Pages、signalfd 等既有失败改写成通过。
- 编译、打包、主机测试、来宾运行分别报告；VMware 验证未完成，物理硬件未验证。
- 如需 Subagent，固定 GPT-6-Luna / Max，禁止嵌套分派。

## 任务 1：安全合并已有提交

涉及 `AGENT.md`、原 `mk/userland.mk`、同分支新增的 86 个变更路径。

- [x] 确认远端是本地基线的后代，核对 gitlink 未变化；归档 tracked/new 文件和补丁。
- [x] 对唯一两个重叠迁移路径预先求合并结果；临时还原这两项输入后进行 `git merge --ff-only`，再恢复原迁移语义。
- [x] `AGENT.md` 同时保留新的 UI 约定和 Kbuild 约定；删除旧 `mk/userland.mk`，将其中新参数留作后续显式迁移。
- [x] 比较合并前源码摘要，所有非远端变更路径必须保持原字节；原工作区和子仓 HEAD 不变，暂存区为空。

## 任务 2：两个应用的原生对象图

创建 `userland/apps/reliefplayer/Kbuild`、`userland/apps/reliefcapture/Kbuild`，修改 `scripts/reliefos/Makefile.userland`，按需要更新新测试对旧规则路径的引用。

- [x] 在添加描述前，实际运行构建，记录缺失新 Kbuild 的失败。
- [x] ReliefPlayer 按旧规则的字典顺序声明 decoder/main/pcm/session/window/worker 六个 C++ 对象；保留 minimp3 头目录。
- [x] ReliefCapture 声明 capture/instance/job/keyboard/main/model/overlay/png_io/render/window 十个 C++ 对象；保留 libX11 链接依赖。
- [x] X11 开发阶段同时声明 libX11.so、libX11.so.6 为完整性成员，保持新上游阶段脚本语义。
- [x] 运行 `test-reliefplayer{,-session,-ownership}.sh`、`test-reliefcapture{,-x11,-ownership}.sh`、组件元数据、FLTK、X11 开发阶段与 Fileman 边界相关回归；测试夹具编译保留独立测试边界。
- [x] 实际构建两个应用并检查 native .cmd、对象顺序和动态依赖；验证重复构建稳定，以及删除 libX11 成员后的自动恢复。

## 任务 3：最终产品和来宾验收

更新 `docs/build/kbuild-migration.md` 的追踪说明，并新增 `docs/build/kbuild-upstream-sync.md` 分层记录。

- [x] 在已验收输出树运行 olddefconfig 和完整 all，确保新组件进入 APK、根文件系统、VMDK 和两种 ISO。
- [x] 运行完整生产增量回归和公开 make test；区分既有失败，不隐藏异常。
- [x] 检查新镜像内两应用 ELF、桌面入口、图标、翻译与快捷键，运行现有 ReliefPlayer 和 ReliefCapture QEMU 验收脚本并查看真实截图；必要的 QA 路径适配单列。
- [x] 再跑稳定 all 和三介质启动，核对源码构建前后未被改写、两仓上游各137文件一致、旧活跃规则无残留，检查新文件空白和脚本语法。
- [x] 完成独立审查和分层报告，记录新的父仓基线、保持两仓迁移未提交；保留原工作区和所有备份。
