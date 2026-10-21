# ReliefOS / ReliefNT Kbuild 迁移规格

状态：用户已确认本规格的范围；实现和验收以此为准。本文描述迁移要求，不把计划中的功能写成已完成。

## 范围与选择

用户确认：两个仓库全部自研 C、C++、汇编源码由 Linux Kbuild 规则构建；镜像、打包、运行及测试由顶层 Make 集成；第三方项目使用其原生构建方式。选择固定版本的上游 Kbuild 与项目 Kbuild 描述。仅更名现有 Make 片段会保留自定义编译引擎，不满足本需求；重写第三方构建不属于用户确认范围。

上游固定为 Linux 6.12，使用现有依赖锁中的官方归档：
`https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.tar.xz`，
SHA-256 `b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb`。
两个独立仓库均包含所需上游构建/配置文件及其许可证、来源清单；正常构建无需下载 Kbuild。项目扩展与原版文件分开，来源清单检查上游文件完整性。

## 架构

- 顶层 Makefile 负责入口、O/ARCH/PROFILE/工具链选择、配置、准备阶段及产品目标。
- 目录级 Kbuild 声明对象、静态库、宿主工具、子目录和生成物；原版 scripts/Makefile.build / Makefile.host 实际执行 C、汇编和宿主编译，原版 Kbuild.include/fixdep 记录命令及头文件依赖。
- C++ 采用单独的项目扩展规则，调用原版 if_changed_dep/fixdep；不会另建对象扫描器、签名数据库或构建调度器。
- 根文件系统、第三方适配、SDK、APK、VMDK/ISO、安装器和运行目标作为产品集成层，依赖实际成员/清单，继续共享 GNU Make jobserver；命令变更使用 Kbuild 机制。
- ReliefNT 能独立构建，不读取父仓库的产品配置；父仓只通过显式参数、导出头和安装产物调用它。

## 配置与输出

采用原生 .config、include/config/auto.conf、include/generated/autoconf.h 及 syncconfig。保留全部现有配置选择、组件 BUILD/IMAGE/ENTRY/SDK/API 区别、required 约束和安装器策略。项目特有派生策略独立生成，不能用第二套解析器覆盖原生 Kconfig 结果。

保留 O=、ARCH=x86_64、PROFILE=debug|release、V=1、显式 CC/CXX/LD/AR/HOSTCC、EXTRAVERSION/LOCALVERSION/SOURCE_DATE_EPOCH 的职责。Kbuild 生成物只写入 O；不同 O 隔离，同一 O 互斥，干净的 help 和 -n/-q 不隐式生成配置。重命名输出时同步更新生产消费者、测试、文档和 CI。

工具链、C/C++/ASM 参数、PIE/动态加载器、CRT、musl/mimalloc、FLTK、共享源码和静态故障提示程序的语义保持；完整保留字体、vDSO、版本头、kerneldebug、导出头、应用资源、安装器和 SDK 生成关系。新增/删除对象由 Kbuild 列表表达并触发正确重建。

## 删除与残留边界

删除失效的自定义编译规则、.sig/candidate 引擎、sources.mk 源枚举、旧配置前端适配及不再使用的元数据/依赖项。构建入口、活跃测试、CI、README、开发约定均更新。检索结果按自研生产文件、测试、活跃文档、第三方源码、历史验收记录和用户输出目录分列，不能通过忽略活跃文件得到零残留。

第三方自身的 Makefile/CMake/Meson 文件属于已确认范围内的输入，不视为旧自研构建残留。历史性能/验证记录保留事实并标明对应旧版本；用户已有镜像、缓存和无关文件不因源码迁移而删除。主工作区现有 BY_AUTHOR.md 删除及其他无关未跟踪文件保持原样。

## 验收

1. 两仓独立的 defconfig/olddefconfig/menuconfig、host 工具、debug/release 内核及自研用户态实际编译；原版 .cmd 和 fixdep 参与依赖判断。
2. 从新 O 构建，第二次无需编译；修改头、C/C++/ASM 参数、链接参数及 Kbuild 成员会重建正确目标。删除多输出中的任一成员会恢复完整产物；两个 O 可并行，同一 O 拒绝并发。
3. SDK 实际编译/链接、APK/根文件系统和 VMDK/ISO/Installer 的生产链可运行，生成物内容/路径和清单校验正确。
4. QEMU 从新镜像启动，运行 I/O 52 项探针并检查桌面；VMware 如运行则单列实际结果，未完成时明确标记。物理硬件状态单列。
5. 新增构建语义回归、现有适用构建测试及 git diff --check；已知 soundctl ownership、音频和 ABI golden 问题单独记录，不擅自修正语义或重生成 golden。
6. 生成可复核的残留审计与构建/打包/运行分层报告。

## Git 和执行约束

迁移在附加的隔离工作树进行。用户本次未要求提交、推送或新建命名分支，保持改动未提交；未来发布仍先推子仓再推父仓。构建产物不纳入源码变更。参与的 Subagent 使用 GPT-6-Luna / Max，职责和文件范围互不重叠。
