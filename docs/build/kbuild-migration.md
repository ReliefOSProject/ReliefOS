# ReliefOS / ReliefNT Kbuild 迁移记录

本文保留父仓基线 `b3ea0583` 上的初次迁移验收。后续同分支两个应用提交的
合并、Kbuild 适配和最新产物验证见 [同分支更新集成记录](kbuild-upstream-sync.md)。

用户确认的范围已经实现：两仓自研生产 C、C++、汇编及宿主工具使用固定的
Linux Kbuild；顶层 Make 集成第三方、SDK、APK、镜像、运行和测试。
第三方保留原生构建与既有移植边界。改动保留在附加工作树，尚未提交或推送。
桌面、I/O 及三类介质启动验收已通过。

## 来源与构建结构

采用 Linux 6.12 官方归档，SHA-256：
`b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb`。
两仓各自的 `scripts/kbuild-source.json` 记录来源和 137 个上游文件摘要；
原版引擎、Kconfig 与许可证逐文件核对一致，没有直接修改上游规则。

- 父仓 `Makefile` 检查输入、互斥和输出所有权，再在 `O` 中调用原版
  `scripts/Makefile.build`。目录 `Kbuild` 声明生产对象，宿主工具用 hostprogs。
- C++ 的独立项目扩展使用 `if_changed_dep` 与原版 fixdep；编译命令、参数及
  头文件依赖由 `.cmd` 跟踪。产品规则位于 `scripts/reliefos/Makefile.*`。
- 配置使用 `O/.config`、`include/config/auto.conf` 和
  `include/generated/autoconf.h`。普通系统和安装器的许可证派生策略独立生成。
- ReliefNT 独立构建，保留原有 157 个 C、3 个汇编及最后的 vDSO 嵌入对象顺序。
  父仓通过安装清单和 85 个导出头消费内核，保留独立内核配置边界。
- 第三方 Make 适配器移除继承的 `-rR` 和项目命令行覆盖，同时保留 jobserver。
  musl 栈保护辅助对象由 Kbuild 编译，其归档单独跟踪 AR 身份。

旧 `mk/` 编译入口、源码列表与 `.sig/candidate` 调度链、自定义配置解析器、
旧 Kconfig 前端子模块和源码目录中的生成配置头已经移除。当前开发文档、CI、
编辑器命令数据库和适用测试已更新。历史报告、第三方原生规则以及用户旧输出
单独分类保留，不属于活跃自研构建残留。

## 编译与打包证据

最终生产输出为 `out/kbuild-verified/`，日志位于 `out/kbuild-migration/`。
本地 APK 签名使用任务输出目录中的专用密钥。

| 项目 | 实际结果 | 主要证据 |
| --- | --- | --- |
| ReliefNT | debug/release、构建/安装、vDSO、85 个头导出及自包含检查通过 | `nt-reviewed-debug-build.log`、`nt-reviewed-debug-tests.log` |
| ReliefOS | 运行库、67 个配置内应用（含 21 个 FLTK 应用）、SQLite、图形库和 ReliefMath worker/probe 通过 | `task3-order-fix-build-rerun.log`、`verified-all-final.log` |
| SDK | 带空格路径重定位，实际静态/动态 C/API 链接通过；公开归档为 regular archive | `sdk-relocation-after-all.log` |
| SDK 恢复 | 删除 stdio、CRT、辅助归档和非主要许可证后，完整恢复，SDK 包字节不变 | `sdk-recovery-after-all.log` |
| APK 与镜像 | 完整 `all` 退出 0；APK、根文件系统、VMDK、Live ISO 和 Installer ISO 均生成 | `verified-all-final.log` |
| 二次完整构建 | 最终 4,070 个对象时间戳不变，无编译/归档/链接；10 个关键产物字节和时间戳不变 | `verified-all-noop-stable.log`、`verified-all-noop-stable-identities.json` |

SDK 静态 C/API 消费者沿用产品的 `-Wl,--gc-sections` 链接策略。
独立新输出的可重现性检查覆盖内核；上表的镜像字节比较针对同一输出树的二次构建。

迁移中修复了两项产物身份问题：规范运行库保留 `libreliefos.so.2`，兼容库保留
`libleonos.so.2`；GRUB 的预置 ELF `.mod` 文件明确作为输入，避免匹配原版
Kbuild 的模块列表输出规则。269 个 GRUB 模块均恢复并核对到原始 HEAD；实际
默认目录 ESP 构建及二次完整构建后，源码目录仍无改动。
生产构建前后还核对了 3,465 个父仓/子仓源码文件；验收脚本自身的有意修改另行记录。
最后的 Kbuild 空白整理触发了一次源码清单更新和重新打包；待输入稳定后，上表的
完整无操作重建通过。重新打包的 VMDK 也重新完成了下述运行验收。

## 主机测试

已通过真实原版引擎回归、普通/ASan/UBSan 宿主测试、原生配置菜单打开/退出、
配置同步/缺失头恢复/检查无写入、C/C++ 参数隔离、组件选择和裁剪、源码/头文件/
UAPI/链接脚本增量、同路径工具更换、对象/产品缺失修复、并发及中断恢复检查。

- 普通和消毒器宿主测试各包含 74 项 common、66 项 JSON 及 locale 用例。
- 完整生产增量回归：`incremental-final-green.log`，包括 SONAME 与准确对象顺序。
- 并发 17 项、内核可重现性 9 项、长任务与中断恢复 19 项通过。
- 第三方契约 29 项、子模块契约 11 项、历史子模块回滚构建 7 项通过。
- GRUB 输入保护回归覆盖真实 ELF 输入摘要、重复构建及必要输入缺失错误。
- musl/Linux UAPI 静态断言和宿主静态/动态 musl+mimalloc 探针通过。

公开 `make test` 最终仍退出 2：宿主测试通过后，首个 APK ownership 检查报告
`component policy mismatch: ['soundctl']`；迁移前已有同一失败。
没有据此宣称整个公开测试套件通过。

## 实际运行

QEMU 使用本次 VMDK 的私有副本，在加入测试账号和探针前，先核对镜像内的
内核、规范运行库、Fileman 和 Taskmgr 与当前 Kbuild 产物摘要相同。

静态和动态 `syscall_io_runtime_probe.c` 在来宾内均完成 52 项检查、0 失败，
覆盖普通文件、偏移、重复描述符、pipe/SIGPIPE、UNIX socket、PTY 和 `/dev/null`。
XDM 登录、IceWM 会话，以及 Fileman/Taskmgr 的中文控件可视检查均通过；
两个应用通过原生窗口关闭按钮正常退出，状态均为 0。截图由代理实际查看，位于
`out/kq6/tty1-greeter-detail.png`、`desktop-fileman.png` 和 `desktop-taskmgr.png`。
`out/kq6/results.json` 记录产物摘要、52 项计数、应用 PID/退出状态及原 VMDK 未变；
最终 VMDK SHA-256 为 `677a117c63c1bf649a9bec8fbb45105cfd48a82c69122e140d3a1a1cbeed94ea`。
公共 `make test-smoke` 退出 0：VMDK、Live ISO 与 Installer ISO 均到达内核
boot complete 和 PID 1，三份完整串口日志中未发现 panic、bugcheck 或 fatal。
两个 ISO 中回读的内核摘要也与本次原生构建相同。该检查证明介质启动；本次
没有重新执行安装器格式化、安装及安装后启动的完整交互流程。
证据为 `media-smoke-after-audit.log`、`iso-kernel-hashes-after-audit.log`，以及
`out/kbuild-verified/logs/smoke-{disk,live,installer}.log`。

额外运行了更广的 musl 62 项探针：`signalfd_abi_test.c:78` 的 `preadv2` 检查失败，
随后整组运行未完成。这不是上述 52 项 I/O 套件。
对完全相同的 ReliefNT 基线 HEAD `10b489986baeece8f89520e975beeb88d47b29b4`
使用旧构建系统重新构建，并运行同一静态和动态 signalfd 探针；迁移前后均在同一
行得到 ESPIPE，Linux 宿主上的静态探针通过。该既有内核兼容问题单独保留，未改
内核行为或探针断言。证据为 `out/signalfd-baseline/{current,old}/serial.log` 和各自
`result.json`；两个内核的启动载荷回读摘要匹配，原 VMDK 未被修改。

## 其余既有失败与边界

Pages 保留迁移前已有的 5 个失效引用。ABI 布局、运行库导出 golden 和音频
manifest 的既有差异单独记录；本次没有重生成 golden 或改变音频验证策略。

## 最终残留检查

检查同时包含两仓受跟踪文件和本轮未跟踪源码；父仓 366 项变更、子仓 170 项
变更均已纳入，忽略的输出和第三方子模块维持原有边界。两仓旧 `mk/` 和旧
Kconfig 前端目录均不存在；活跃生产入口、构建测试、CI 和当前开发说明中，
旧签名宏、旧源码枚举、旧配置输出及旧前端调用均无命中。

全源码检索的剩余文字命中逐项归类为历史记录、本次删除范围说明，以及
ReliefMath 的候选渲染状态成员；没有把活跃规则排除后宣称零残留。
原版引擎与许可证各 137 个文件的摘要再次一致。两仓受跟踪 diff 和新文件的
空白检查、修改脚本的语法检查均通过；没有功能 C/C++/汇编或公共 ABI 改动。
证据为 `source-audit-final.log`、`legacy-term-hits-final.json` 和
`source-status-final.json`；独立审查与完整分类保存在本次 SDD 记录中。

原工作区的 6 项既有变更保持原样。构建产物均位于输出目录，没有加入暂存区。

VMware 验证未完成。物理硬件未验证。QEMU 的 VMware 显卡设备验证不等于
实际 VMware/SVGA3D 验证。两仓改动均保持未提交；未来提交/推送仍按子仓先行、
父仓随后更新 gitlink 的顺序执行。
