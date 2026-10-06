# ReliefOS Agent Guide

本文件是 ReliefOS 仓库内所有自动化代理、维护者和贡献者的工作约定。它
描述当前架构、构建与验证边界，以及本项目维护者偏好的协作方式。除非用户的
明确指令与本文件冲突，否则应遵守本文件。

## 1. 总则：先检查、再修改、再证明

- 先检查真实工作区、相关源码、构建图和 `git diff`，再判断问题或实施修改。
  不要根据目录名、历史印象或“应该如此”推断已交付功能。
- 任何结论都要区分四个层次：**源码已检查**、**已修改**、**已编译/已打包**、
  **已在目标虚拟机运行验证**。不能把前一层说成后一层。
- 发现工作区已有未提交修改时，默认把它们视为用户工作：保留、绕开、不要
  重置、覆盖、批量格式化或还原。先用 `git status --short` 和定向
  `git diff` 确定自己的改动范围。
- 不要为了排查而随意结束用户的构建、QEMU、VMware、WSL、TUI 或其他进程。
  先检查进程、任务状态和日志；只有用户明确要求或确认目标后才停止进程。
- 变更应保持在请求范围内。发现相邻的明确缺陷可一并修复，但应说明其与
  当前问题的关系；不要未经授权引入大范围重构、替代工具链或可选功能。
- 涉及数据删除、镜像重建覆盖、磁盘格式化、清理用户文件、Git 强制操作或
  远程推送前，必须核实精确目标和用户授权。禁止 `git reset --hard`、
  `git checkout --` 这类会丢弃用户改动的操作，除非用户明确要求。

## 2. 项目定位与运行时架构

ReliefOS 是面向 x86_64、UEFI 启动的操作系统项目。正常系统使用 FAT32 ESP
和 ext2 根文件系统，应用运行在 Ring 3，内核和可加载驱动运行在 Ring 0。主要启动
与服务链如下：

```text
UEFI/GRUB
  -> boot/ loader.elf
  -> kernel/reliefnt 子仓产物 kernel.sys（Ring 0）
  -> userland init.elf
  -> desktop.elf（窗口服务器）
  -> 登录 / OOBE / 服务 / 普通桌面应用
```

安装器 ISO 是另一条启动路径：顶层 ISO 只包含启动所需的 loader、内核和
installer root；真正安装到磁盘的系统分为 `/install/esp`（FAT32
启动载荷）和 `/install/root`（ext2 运行时根载荷）。不要把“安装器运行时
镜像内容”和“安装后系统内容”混为一谈。

### 根目录职责

| 路径 | 职责 |
| --- | --- |
| `boot/` | GRUB 配置与 EFI 模块；loader 源码在内核子仓 `boot/loader/`。 |
| `kernel/reliefnt/` | ReliefNT 子仓（gitlink，URL 为 github.com/ReliefOSProject/ReliefNT）：内核核心（调度、内存、ELF 进程、syscall、GUI IPC、网络、驱动管理、权限判定与 `lib/` 内部工具）、`drivers/`、`boot/loader/`、`include/uapi` 与内核侧 `include/reliefnt/`。首次使用执行 `git submodule update --init --recursive`，并在子仓内 `make fetch`（其缓存不入库）。 |
| `userland/runtime/` | ReliefOS libc、syscall 包装、UI/字体、网络/HTTP/TLS、PTY 等公共实现。 |
| `userland/apps/` | Ring-3 系统与桌面应用；`desktop/` 是窗口服务器，其他应用为它的客户端。 |
| `userland/{busybox,stardustui}/` | 第三方软件的 ReliefOS 端口、适配层与构建输入。 |
| `include/reliefos/` | 公共 C ABI 头文件；旧 `include/leonos/` 转发头保持兼容。 |
| `system/` | 被 staging 的系统配置、字体、壁纸、证书、图标、应用资源和默认内容。 |
| `configs/` | 动态组件清单、可提交 build profile 与默认配置。 |
| `tools/` | 构建、Kconfig 同步、镜像、安装器、SDK、资源生成和验证脚本。 |
| `mk/`、`tools/host/`、`tools/build/` | Make 依赖规则、C 工具及上游适配器。 |
| `docs/` | 架构、ABI、构建、文件系统、驱动、安全等项目文档。 |
| `third_party/` | 通过 Git submodule 引入的上游源码；见 `.gitmodules`。 |
| `build/` | 可再生产物与 staging 输出；不可作为手写源码的唯一来源。 |

### 特权边界

- Ring-3 程序经 Linux 编号的 x86_64 syscall ABI 进入内核，入口目前为
  `syscall`；参数使用 `rax/rdi/rsi/rdx/r10/r8/r9`，负返回值为
  `-errno`。已实现接口才可视为可用，未知 syscall 返回 `-ENOSYS`。
- 内核负责用户指针与长度验证、页表/进程资源、硬件和最终授权。
- 内核内部能力按职责直接落在内核子仓的 `kernel/reliefnt/kernel/reliefnt/`（含 `lib/` 工具）和
  `kernel/reliefnt/drivers/bootstrap/storage/`（文件系统与 `LEONACL.SYS` 权限元数据），全部编译进
  kernel.sys；不存在跨模块 callback 表或第二个启动镜像。用户态与内核之间只有
  syscall/ioctl ABI 和 GUI IPC。职责归属与旧数据格式的兼容策略见
  `docs/KERNEL_USERSPACE_BOUNDARIES.md`。
- GUI 客户端与 `desktop.elf` 通过 GUI IPC/ioctl 通信，而不是共享窗口服务器
  的私有像素内存。应用提交自己的缓冲内容；不要把窗口服务器内部 buffer
  当作公共 ABI。
- 来宾运行路径使用 Unix 根目录格式，例如 `/usr/lib/reliefos/apps/desktop/desktop.elf`；
  仓库源码路径（`system/`、`kernel/reliefnt/drivers/`、`docs/`）是构建输入，不等于来宾路径。
  现行 rootfs 契约见 `docs/ROOTFS_LAYOUT_AND_MIGRATION.md`。
  相对路径依赖任务当前目录；路径统一使用 Unix 根目录语义。

## 3. 公共 ABI、库和 SDK 的联动规则

任何新 syscall、ioctl、公共结构、窗口/鼠标/网络/输入法 API 或 UI 库接口，
都要逐项检查以下闭环，不能只修改一处：

1. `include/reliefos/*.h`：公共定义、常量、结构布局、权限语义和返回值。
2. `kernel/reliefnt/kernel/reliefnt/`（内核子仓）：编号、用户范围检查、权限检查、实现和错误路径。
3. `userland/runtime/include/` 与 `userland/runtime/src/`：声明、包装和实现。
4. 使用该 API 的系统应用、窗口服务器及相关测试程序。
5. `configs/header-export.list` 白名单与 `headers_install` 导出：SDK/用户态只消费
   导出结果，由 `tools/test_header_export.py` 校验；不维护手工 ABI 镜像副本。
6. `packages/reliefos-musl-sdk.tar.gz` 装配规则与归属/许可证文本。
7. `docs/ABI.md`、`docs/SYSCALLS.md` 或对应专题文档。

公开结构应采用定宽类型，校验用户提供的指针、容量、长度、枚举值和版本。
不要在 ABI 中泄漏内核指针、窗口服务器私有地址或未经界定的可变对象。扩展
现有结构时，要考虑旧调用方的大小与兼容行为。

### libc 与第三方移植

- musl、mimalloc 与 ReliefOS 扩展库构成用户态 C 环境；不要把“成功链接”误称为
  “已完整移植”。每个移植软件都需要确认其真实源码、适配层、启动代码、
  musl/mimalloc 依赖、ELF 输出、镜像 staging 和运行路径。
- 新增第三方软件时，除上游源码外还要处理：构建脚本、组件清单、镜像路径、
  launcher/桌面入口（若需要）、许可证和归属、SDK/API 包（若公开）、以及
  对应文档。
- `third_party/` 有 submodule 时，遵守 submodule 工作流：上游或分叉仓库的
  源码变更先在正确仓库提交，再在主仓库更新 gitlink；不要把未初始化或宿主机
  目录误当成可提交的第三方源码。
- 任何 API 安装包都必须同时检查包元数据、归档内容、目标安装路径、部分写入
  和失败清理、启动/自启动授权以及安装后的可运行性。

## 4. UI、桌面、主题、字体和输入原则

UI 修改必须横向检查，而不是只改一个应用。典型关联范围包括：

- `userland/apps/desktop/`：窗口管理、桌面、任务栏、开始菜单、状态栏、
  覆盖层、主题广播、图标和壁纸。
- `userland/runtime/src/ui*.c` 及公开 UI 头文件：控件、布局、绘制、文字输入、
  文件选择、窗口协议、主题状态与字体。
- 所有受影响内置应用：窗口尺寸、焦点、键鼠、文本编辑、主题变化事件、
  图标/快捷方式和高 DPI/分辨率边界。
- `system/` 中的字体、壁纸、BMP/PNG/图标等 staging 资产，以及
  `tools/prepare_ui_font.py`、`tools/make_app_icons.py` 等生成步骤。
- 登录、OOBE、Installer 与早期 framebuffer：它们可能使用不同的启动阶段和
  配置读取时机，不能仅以登录后桌面正常就宣布完成。

### 主题与个性化

- 用户个性化数据属于 `/home/<name>/appearance.conf`；Metro 与 Win95
  的基础色配置相互独立，不能相互覆盖。
- `/etc/reliefos/display.conf` 是尚无用户会话时的启动/默认外观，用于早期
  framebuffer、bugcheck、登录、OOBE 和安装器等场景。它不能替代每用户配置。
- 修改个性化设置后应立即经 Desktop 发布状态并让已打开应用收到主题变化；
  不要只写文件、等下次启动才生效。
- 现有壁纸 BMP 处理有安全上限：最大 1280 x 720，仅接受受支持的未压缩
  24-bit/32-bit BMP。任何格式、尺寸或缩放策略扩展都需检查解码器、内存占用、
  绘制与错误显示。
- TTF/字体加载、壁纸加载、目录扫描和图标读取不得长期阻塞 Desktop 主循环。
  需要分段、异步或有界处理时，保持 UI 可响应并保留明确的失败状态。

### 输入、窗口与可访问性

- 新输入、键盘快捷键、鼠标模式或终端行为要同时验证：事件消费顺序、焦点、
  修饰键释放、文本提交、预编辑文本、应用快捷键与窗口服务器全局快捷键。
- 密码或其他安全输入字段必须明确禁止第三方输入法；不得为了“兼容”把敏感
  文本交给扩展提供者。
- 控件应依赖真实 UI 控件/状态，而不是仅通过绘制模拟的伪控件来承载交互。
  新控件需处理尺寸不足、键盘导航、鼠标命中和主题变化。
- 窗口控制按钮、任务栏、状态栏、候选框和上下文菜单必须在默认窗口尺寸和
  边缘尺寸下不重叠。对不支持缩放的窗口，应按窗口能力隐藏而非仅灰化放大按钮。

## 5. 存储、网络与长期操作

### 存储与响应性

- 任何文件系统写入、API 解包、词库下载、游戏资源安装或大文件复制必须正确
  处理短读、短写、临时失败、重试边界、最终错误和资源关闭。`ret == 0` 不等同
  于完整写入成功；以实际传输字节数判断。
- 不能在 Desktop、窗口绘制、输入事件或持锁的热路径中执行长时间同步磁盘
  操作。将重操作放入 worker/服务路径，并持续让出调度机会；UI 只显示进度和
  可恢复的完成/失败状态。
- 概率性“权限拒绝、资源缺失、图标/字体/壁纸偶尔不加载”等问题，应优先从
  共享的存储、目录枚举、缓存、并发和状态提交路径排查，而不是分别添加 fallback。
- 对写入关键状态（账户创建完成标记、安装状态、配置）使用可诊断的提交顺序，
  保证重启后不会产生“主体成功、完成标记失败”的矛盾状态。

### 网络与 TLS

- 网络栈当前主要服务 e1000；QEMU/VMware 的网卡、DHCP、DNS、TCP 和显示/
  驱动行为并不完全相同。报告时必须说明实际运行平台。
- TLS 故障必须区分 DNS、TCP 连接、握手、证书链、主机名、时间、发送与读取。
  日志可输出主机名、阶段、返回码、网络状态、证书校验标志和时钟状态；绝不
  输出密码、Cookie、HTTP 请求体、令牌或私密证书内容。
- 不得通过关闭证书、主机名或时钟验证来掩盖问题。`MBEDTLS_SSL_VERIFY_REQUIRED`
  是安全基线；CA 解析的非负“部分成功”结果应结合已解析根数量处理。
- 网络错误 UI 不应把所有 TLS/传输错误都称为“证书验证失败”。保留底层状态，
  使诊断和用户提示能反映实际失败阶段。

## 6. 构建系统和动态 Menuconfig

根 `Makefile` 是唯一受支持的生产入口。GNU Make 负责依赖图、增量判断及 jobserver；
`mk/*.mk` 声明规则，`tools/host/` 的 C 程序只转换数据，`tools/build/` 的短 Shell 脚本
适配上游 configure/Make 和镜像格式工具。不得另建调度器、调用旧 Python 引擎，或让生产链
执行 Python、Meson、Ninja。生产链只编译 C 与汇编，不要求 Rust 工具链。

```sh
make help
make doctor
make fetch
make defconfig
make -j8 all
make menuconfig
make olddefconfig
make O=out/debug PROFILE=debug -j8 kernel
make V=1 image-vmdk
make --trace userland
make -n installer
make test
make test-long
make test-legacy
make test-smoke
```

- 默认 O 为 `out/<arch>/<profile>`，支持 `ARCH=x86_64`、`PROFILE=release|debug`。
  修改输出目录用 O，修改目标工具用显式 CC/CXX/LD/AR；HOSTCC 独立。
- `O/config/.config` 是当前配置，Kconfig、Kconfig.components 与 configs/components.toml
  是受跟踪输入。组件 BUILD、IMAGE、ENTRY、SDK、API 选择不得混用；required 组件不可关闭。
  修改组件元数据时保持 Kconfig.components 同步，运行组件 fixture。
- 生成的 C 配置和版本头写入 O。生产构建不得改写源码目录、递增版本计数，或使用
  当前时钟/随机 UUID 破坏可重现性；SOURCE_DATE_EPOCH 默认来自提交时间。
- 下载只能出现在显式 fetch；生产消费已校验缓存，不允许隐式联网。依赖锁是唯一下载身份来源。
- 新源文件、删除源文件、编译/链接参数变化、多输出成员缺失必须正确触发重建。
  对目录 stage 使用完整成员清单，临时树完成并校验后再发布，不能以 stamp 代替存在性检查。
- 同一 O 的真实构建互斥，不手工伪造 RELIEFOS_BUILD_OWNER。不同 O 可并行。
  `-n/-q` 不重建被包含的配置，避免干扰持锁构建；首次准确预览前先 defconfig。
- `all` 包括 kernel、userland、runtime、sdk、apk-repo、image-vmdk、iso、installer；
  `release` 再加本地 rpr-pages。run/run-debug/run-iso/run-installer 只负责运行对应产物。
- SDK 必须用随包 C 编译器驱动实际编译/链接，覆盖路径含空格的重定位和静态/动态程序。
  APK 必须保留真实上游数据库、签名、依赖和文件所有权；本地 world 请求不可精确锁版本。
- 更新已安装系统必须保留 Alpine 包与本地配置；正式发布包版本必须递增。
  新 APK 的版本代际为 1，兼容旧 0.time_ns 版本升级；脏工作区内容哈希本身不保证时序。
- clean/distclean 仅清有所有权标记的 O，拒绝 symlink/源码根/不明目录，保留下载缓存。
  不删除用户旧 build/、buildsystem/cache/、buildsystem/deps/、ISO 或 VMDK。
- Python 仅允许现存非生产 OS 测试、维护工具及上游参考代码。详见
  `docs/build/legacy-removal.md`；不以保留的参考实现冒充新链验收。

## 7. 验证要求

按风险选择最小但充分的验证，并在交付时逐项报告实际执行过的命令与结果。

| 修改类型 | 至少应验证 | 完成标准 |
| --- | --- | --- |
| 文档/单文件说明 | 定向检查 + `git diff --check` | 内容正确、无格式/空白问题。 |
| 构建脚本/Kconfig/组件清单 | `make test`、相关 Make 目标 | 菜单、同步和目标选择真实生效。 |
| 内核/存储/ABI/libc | 受影响目标编译 + 相关用户态重建 | 编译链接、头文件与调用链一致。 |
| 应用/UI/主题/字体 | 受影响应用、VMDK/ESP 构建 + QEMU | GUI 可见行为、交互和日志被证明。 |
| VMware 专属显示/驱动 | VMware 启动与可见操作 | 不以 QEMU 成功替代 VMware 证明。 |
| Installer 变更 | Installer ISO + 实际安装/启动路径 | installer runtime 与安装后系统均正确。 |
| 网络/TLS | 相关构建 + VM 真实网络请求 | DNS、TCP、握手、验证与 UI 状态可区分。 |
| 安全修复 | 定向回归检查 + 合适的运行证明 | 触发条件被阻断，未用降级掩盖。 |

额外规则：

- 每次源码修改完成后运行 `git diff --check`。
- 编译通过不等于镜像已含文件；镜像已生成不等于虚拟机能启动；日志出现不等于
  UI 可见。分别证明。
- QEMU 的 QMP Unix socket 不要放在 `/mnt/d/...`（WSL DrvFs 不支持绑定）；
  使用支持 Unix socket 的数据盘目录，例如
  `/home/xiaobai/Projects/Projects/.reliefos-qmp-<id>.sock`；不要使用 `/tmp` 或 tmpfs。
- 遇到概率性失败，记录平台、镜像、网络、操作步骤和关键日志，重复定向验证；
  不要以单次成功或失败就宣布根因。
- 新增安全审计报告时，使用简体中文，归档为 `docs/security/YYYY-MM-DD.md`，
  明确静态覆盖范围和未做的 PoC/QEMU 验证，不能虚构修复版本或利用结论。

## 8. 代码注释规范

`kernel/reliefnt/kernel/reliefnt/`（内核子仓）中的每个函数定义和公共函数声明必须紧贴
Doxygen 风格块注释。C 与汇编预处理源统一使用 `/** ... */`，格式如下：

```c
/**
 * @brief 说明函数实际负责的行为、边界与副作用。
 * @param request 输入；说明可空性、所有权、长度或容量。
 * @param out_result 输出；调用方提供的可写存储。
 * @return 成功值和所有重要错误/特殊返回值。
 */
int subsystem_handle(const struct request *request, struct result *out_result);
```

- `@brief` 必须表达职责，不能只重复函数名。涉及用户指针、权限、锁、中断上下文、
  内存映射、引用计数或硬件 I/O 时，要记录适用的关键前置条件或副作用。
- 每个参数都用 `@param` 说明输入/输出方向、可空性、所有权与字节长度/数组容量；
  无参数函数不写占位 `@param`。不返回的函数应说明不返回的原因。
- 每个非 `void` 函数都用 `@return` 描述成功结果、负 errno 或其他特殊值。异步函数还要
  说明完成、回调或状态变化的时机。
- 静态私有函数至少在定义处注释；公共函数的头文件声明和实现都必须有一致描述。函数
  注释不替代结构、宏、ABI、状态机或复杂算法所需的独立说明。
- 修改已有函数签名、返回约定、所有权、锁定规则或副作用时，必须同步修改其 Doxygen
  注释；新函数不带合格注释不得合入。

## 9. 文档、日志和交付质量

- 新功能、公开 API、构建开关、镜像布局或第三方移植发生变化时，更新相关
  `docs/`、SDK 说明、示例和归属/许可证文本。文档只能描述
  已确认存在的接口；计划中的接口必须明确标为计划。
- 日志应使用稳定前缀（例如 `[reliefnt]`，兼容期 `[ntclks]`、`[desktop.elf]`、`[tls]`），包含足够的
  阶段、返回码和状态来定位问题，但不能泄露令牌、密码、Cookie、私钥或请求体。
- 对用户的最终交付应优先给出结果，然后列出：修改了什么、关键路径、构建/
  打包/运行验证分别是否完成、已知限制和下一步。默认使用简体中文。
- 当用户要求“继续”“开始改”“赶紧改代码”时，在确认工作区与范围后直接实施；
  不要重复给方案代替执行。只有缺少会实质改变范围或安全性的授权时才提问。
- 当用户问“为什么”或要求诊断时，先给源码和日志证据，再给结论；除非用户
  同时要求修复，否则不要擅自修改。

## 10. Git、CI 与发布

- 提交前检查 `git status --short`、`git diff --check`，并只 stage 本任务的文件。
  没有用户明确要求时，不要自行创建分支、提交、push 或创建 PR。
- CI 配置位于 `.github/workflows/`。凡是改变依赖、Kconfig、组件清单、构建
  目标、SDK、Installer、第三方移植或产物布局，都要检查 CI 是否仍能从干净
  checkout 和递归 submodule 状态构建。
- CI 的构建通过仅证明 Linux 自动化路径；不要把它等同于 VMware/QEMU 的图形、
  音频、鼠标、网络或安装交互已经验证。
- 发布任务应同时考虑 VMDK、普通 ISO、Installer ISO、SDK、API 包、校验和与
  第三方归属文件；任何一项是否包含某个组件由当前 profile 与组件清单决定。
- ReliefNT 子仓（`kernel/reliefnt`，`https://github.com/ReliefOSProject/ReliefNT`）日常开发在子仓内进行：在子仓里开分支或
  游离提交（detached HEAD），验证通过后推送其工作分支或 main；主仓只提交
  更新后的 gitlink（`git add kernel/reliefnt`），不把子仓改动拆进主仓提交。
  子仓 checkout 处于 detached HEAD 是正常状态（gitlink 检出即游离），不要
  在主仓 `git submodule update` 后顺手帮子仓建分支。发布目标（rpr-pages/
  release）要求子仓 clean 且 HEAD 与 gitlink 一致；开发构建（all/kernel）
  允许子仓 dirty。

## 11. 常用排查顺序

1. `git status --short`，查看是否有用户进行中的工作。
2. `git diff -- <相关路径>`，确定当前改动与请求关系。
3. `rg` 或 `git grep` 找到从 UI/API/构建到 staging 的完整调用链。
4. 用 `make --trace`、`make -n` 和 `V=1` 证明实际构建路径。
5. 以最小相关目标编译；必要时构建 VMDK/Installer。
6. 在对应的 QEMU 或 VMware 平台进行运行/可视验证。
7. `git diff --check`，并在交付中区分已验证与未验证项。

不要以删除 fallback、增加 sleep、关闭校验、无限增大超时、吞掉错误或硬编码
成功状态来“修复”问题。应找到真实边界、状态机、权限、I/O 或构建/staging
断点，并保留可观察的诊断。
