# ReliefOS / ReliefNT Kbuild 迁移实现计划

> **面向 AI 代理的工作者：** 使用 subagent-driven-development 执行，步骤使用复选框记录。用户已确认范围，按本会话持续执行；未经新指示不提交、推送或新建命名分支。

**目标：** 两仓全部自研生产源码使用真正的 Linux Kbuild，并删除旧自定义构建链的活跃入口与引用。

**架构：** 固定 Linux 6.12 的构建/配置引擎，目录级 Kbuild 描述源码与宿主工具。产品打包和第三方适配通过顶层 Make 集成；C++ 只扩展编译动作，沿用 if_changed_dep/fixdep。

**技术栈：** GNU Make、Linux Kbuild/Kconfig、宿主 C、Clang/LLVM、现有 musl/FLTK 与打包工具。

**规格：** `docs/superpowers/specs/2026-10-21-kbuild-migration-design.md`。

## 全局约束

- 使用附加工作树 `/home/xiaobai/Projects/Projects/.worktrees/kbuild-migration/ReliefOS_Xorg`；原工作区及其无关改动保持原样。
- Linux 6.12 归档 SHA-256 为 `b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb`；来源/许可证随上游构建文件记录。
- O=、ARCH=x86_64、PROFILE=debug|release、V=1、独立 HOSTCC 和显式工具覆盖保留；TMPDIR 在工作树 out/tmp。
- 自研生产编译使用原版 Kbuild；第三方维持其现有端口语义与原生构建；作为自研库输入的已移植 vendor 源不借机改写功能边界。
- 不改变内核/用户 ABI、音频 manifest 或 ABI golden；不将已有测试失败改写成成功。
- 工作记录放在本计划独占的 `.superpowers/sdd/2026-10-21-kbuild-migration/`；构建日志与镜像在 out/kbuild-migration/。
- 实现者不再分派 Subagent，所有 Subagent 固定 GPT-6-Luna / Max。实现任务按依赖顺序交接，独立静态审查可并行。

### 任务 1：固定引擎与真实 Kbuild 冒烟回归

**文件：** 两仓 `scripts/Makefile.*`、`scripts/Kbuild.include`、`scripts/basic/`、`scripts/kconfig/`、`LICENSES/`、`scripts/kbuild-source.json`；创建 `tests/build/test-kbuild.sh`。

- [x] 验证归档后只导入需要的上游文件及许可证，逐文件记录 SHA-256；项目扩展使用独立文件。
- [x] 回归先验证缺少 Kbuild 引擎时失败；fixture 使用原版规则而非重新实现编译器。
- [x] fixture Kbuild 使用 `obj-y := first.o second.o`、`hostprogs-always-y := helper`，实际编译 C、汇编及宿主工具，核对 built-in.a、fixdep 与 .cmd。
- [x] 验证 no-op、头文件、命令参数、对象列表删除、失败退出、并行 jobserver 及独立输出树行为。
- [x] 原版文件完整性和许可证检查通过后进行任务审查。

### 任务 2：ReliefNT 独立构建完整切换

**文件：** `kernel/reliefnt/Makefile`、各源码目录 `Kbuild`、`arch/x86_64/Makefile`、`scripts/Makefile.reliefnt*`、原 `mk/`、配置与工具消费处。

- [x] 归档旧源码/编译参数/产物清单，作为对照；不保留可执行旧构建入口。
- [x] 使用原生 conf/mconf、syncconfig、auto.conf 和 autoconf.h，保留独立内核配置边界与输出所有权/互斥。
- [x] 为原先所有内核、驱动、架构、mm/fs/net/firmware/block 对象编写 Kbuild，保留链接对象集合与必要顺序。
- [x] vDSO、嵌入对象、版本/字体资源、kerneldebug、导出头和 install 清单用 Kbuild 生成依赖表达；自研宿主工具用 hostprogs。
- [x] 删除旧 `mk/` 编译/签名/源列表与旧配置适配；更新子仓文档和适用测试。
- [x] 在新 O 验证 `defconfig`、`olddefconfig`、`menuconfig`、`all install headers_install test-vdso`，debug/release、后缀/工具覆盖、缺失产物修复和二次无编译；独立任务审查。

### 任务 3：ReliefOS 自研运行库、C/C++ 应用和工具

**文件：** 父仓 `Makefile`、`Kbuild`、`tools/host/Kbuild`、`userland/**/Kbuild`、`scripts/Makefile.cxx`、自研音频/图形/ReliefMath 适配的 Kbuild。

- [x] 宿主工具及聚合 C 对象注册 hostprogs；配置由原版 Kconfig 生成，单独生成许可证/installer policy 别名。
- [x] 运行库保留相同导出符号、SONAME、静态/动态链接和依赖，编译动作改用 Kbuild。
- [x] 各应用用独立对象目录和 Kbuild，覆盖 FLTK、共享源、Doom、glxgears、motd、静态 dynlinkerror、gptinit、音频库及 ReliefMath worker；共享源码无复制且不同编译上下文不共用对象。
- [x] C++ 规则使用 `$(call if_changed_dep,cxx_o_cpp)` 与原版 fixdep，C/C++/ASM flags 分离。
- [x] 保留组件 BUILD/IMAGE/ENTRY/SDK/API 语义；更新组件生成工具消费 native CONFIG_*，取消旧 autoconf.mk。
- [x] 新 O 验证 tools、runtime、userland、全部 FLTK app、worker 与专用宿主回归；修改 C++ 参数不重编无关 C；独立任务审查。

### 任务 4：打包、依赖、镜像与测试入口

**文件：** 原 `mk/*.mk` 产品集成规则、`scripts/Makefile.*` 产品集成层、`tools/build/` 适配器、`tests/build/`、`.github/workflows/`。

- [x] 迁移全部 public goals，包括第三方 fetch、导出头、SDK、APK、rootfs、VMDK/ISO/installer、release/pages、run 和 clean。
- [x] 去掉自定义 .sig/candidate 规则，命令依赖采用 Kbuild；保留完整成员清单和原子发布，不能用 stamp 掩盖丢失成员。
- [x] 删除不再需要的 kconfig-frontends 子模块声明/依赖、脚本和 config 解析器；保留通用内容稳定发布等仍有真实消费者的工具。
- [x] 活跃构建测试断言行为而非旧 Make 宏；测试中的 C fixture 编译是验证过程，不冒充产品编译入口。
- [x] 运行构建契约、并发/增量/重现/SDK/安装器 staging 回归和公开 make test，完整记录失败；独立任务审查。

### 任务 5：全新产物与实际启动验收

**文件：** 输出目录内的日志/产物/摘要和 `docs/build/kbuild-migration.md`。

- [x] 从新 O 实际完成内核、用户态、SDK、APK、根文件系统、VMDK、普通 ISO、Installer ISO，检查缺失成员自动恢复与稳定重建。
- [x] 同 epoch 的独立输出核对可复现范围；参数变更、头变更、对象删除回归均成功。
- [x] 仅运行任务拥有的 VM，从新产物启动 QEMU，运行实际 POSIX 52 项探针及桌面可视检查，核对启动内核与测试产物摘要。
- [x] 若运行 VMware，分层记录实际检查与未验证项；始终明确 VMware 验证未完成的边界以及物理硬件状态。

### 任务 6：残留审计与最终复核

**文件：** README、AGENT.md、当前构建文档/CI/测试与 `docs/build/kbuild-migration.md`。

- [x] 扫描两仓 tracked 与本轮新建的自研生产文件，旧 mk/ 入口、RELIEFOS_SIGNATURE_RULE、RELIEFOS_SIG_、.candidate、sources.mk、autoconf.mk 和旧 frontend 调用应归零；逐项复核命中。
- [x] 文档、测试、CI 对应新构建图；历史记录标注范围，第三方和用户缓存/旧镜像单列。
- [x] 检查最终 diff、许可证、上游文件 hash、未提交文件清单与生成物边界；进行一次完整独立审查，修复实质问题后定向复核。
- [x] 写出编译/打包/测试/运行分层报告；保持改动未提交，工作树保留供用户审阅。
