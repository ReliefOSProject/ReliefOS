# Make 全量迁移：2026-09-19 历史接口

本文记录 2026-09-19 的迁移结果，包含当时的路径和组件，不代表当前生产接口。
当前 Linux Kbuild 接口见 [BUILDSYSTEM](../BUILDSYSTEM.md)，本次验收见 [Kbuild 迁移](kbuild-migration.md)。
历史验收证据保存在 verification.md。

| 功能 | 当前目标 / 实现 | 产物 |
| --- | --- | --- |
| 配置、版本、依赖 | defconfig / olddefconfig / menuconfig / fetch；C host helpers | O/config、O/include/generated、cache/downloads |
| 内核、驱动、loader、Rust middlelayer | kernel / drivers / loader / middlelayer；scripts/reliefos/Makefile.kernel、boot.mk | O/generated/system、drivers、loader |
| 运行库、认证、应用 | runtime / leonos-pam / userland / leonos-upstream | O/system、O/userland、独立上游安装树 |
| SDK | sdk / musl-sdk；C 编译驱动，按 SDK 选择导出 | O/packages/LeonOS4-Developer-SDK.zip、leonos-musl-sdk.tar.gz |
| 根文件系统 | rootfs-raw / rootfs；C stage 清单与签名 APK 安装 | O/rootfs/{raw,managed}、manifest.json |
| 签名 APK 与升级 | apk-repo；真实上游 apk mkpkg/mkndx/add | O/packages/apk/repository；真实 installed DB |
| 磁盘、live、安装器 | image-vmdk / iso / installer | O/images/leonos4.{raw,vmdk}、leonos4-live.iso、leonos4-installer.iso |
| RPR | rpr-apps / rpr-pages；本地生成，不发布 | O/rpr-pages |
| 聚合与运行 | all / release / run / run-debug / run-iso / run-installer | all 含 SDK 和三类镜像；release 再含 RPR |
| 验收 | test / test-long / test-legacy / test-smoke | 明确区分主机测试、生产 exec 跟踪、旧 Python 回归、来宾启动 |

生产规则不回退到旧 Python 引擎；已删除清单与保留测试参考的边界见 legacy-removal.md。

## 历史 P0 盘点（仅供对照，不代表当前状态）

# LeonOS 4 构建系统迁移台账（P0）

本文是 原构建重建计划（已清理，历史版本可从 Git 查看） 第 12 节 P0 阶段的交付物之一：
把旧 Python 构建系统的实际职责、输入输出与消费者盘清，并给出新 GNU Make 系统的归属与验收方式。

本轮只完成 **P0 与 P1 的一部分**。未做项在第 6 节明确列出，不以"脚手架存在"充当完成。

- 基线提交：`6be4c69`（`xiaobai/dev/buildsystem` 的创建点，内容为 `main` 的超集，含 EEVDF、
  Clang 用户窗口、menuconfig 修复等 13 个提交）
- 盘点时间：2026-09-19
- 盘点方法：直接读 `build.py`、`buildsystem/`、`tools/`，用 `python3 build.py -v run <task>` 观察真实
  执行链；数字来自本仓库实测，不是估算

## 1. 实测规模

| 项目 | 实测值 | 取证方式 |
| --- | --- | --- |
| `build.py` | 4958 行 | `wc -l` |
| `buildsystem/` Python | 43 个文件 / 7150 行 | `find` + `wc -l` |
| `tools/` Python | 生产 71 个、测试 129 个（合计 24497 行） | 目录清点 |
| `graph.add(...)` 目标注册点 | **157** | `grep -c "graph.add("` |
| `name="..."` 字面量 | **146** | `grep -c 'name="'` |
| 顶层任务（`build.py help`） | 21 个 | 见第 2 节 |
| 上游 submodule | 22 个 | `git submodule status` |
| 生产链 Python 调用点 | 约 72 处（调研稿统计，含子进程与进程内逻辑两类） | 第 5 节说明其可靠性 |
| 可归入 `test-legacy` 的既有测试 | 约 37 个 | 同上 |

`build.py help` 的 21 个顶层任务是：`all, config-sync, build-info, loader, kernel, drivers,
middlelayer, userland, sdk, esp, rpr-pages, image-vmdk, image-iso, installer, release, run,
run-debug, run-iso, menuconfig, defconfig, clean`。其余 100 多个目标由组件清单与循环展开生成。

## 2. 顶层任务 → 新 Make 目标映射

新目标语义见计划第 4 节。"阶段"指计划第 12 节的 P0–P5。

| 旧任务 | 新目标 | 阶段 | 验收方式 |
| --- | --- | --- | --- |
| `menuconfig` / `defconfig` | `make menuconfig` / `make defconfig` | P1 | `test-bootstrap.sh` + 干净 O 下生成 `$(O)/config/.config`，与旧 `buildsystem/config/leonos.conf` 逐项比对 |
| `config-sync` | `make defconfig` 的产物转换（`leonos-config`） | P1 | C 工具契约测试：`y/n/字符串/数字`、重复冲突赋值、非法行、内容不变则 mtime 不变 |
| `build-info` | `make kernel` 依赖 `$(O)/generated/include/generated/build_info.h` | P1 | A02：重复构建不重编译 `version.c`；A17：构建前后受跟踪文件零差异 |
| `boot-logo` | `make tools` 的 C 资源生成器 | P1 | 与旧 `boot_logo.h` 逐字节比对 |
| `loader` | `make kernel`（启动链并入） | P1 | 干净 O 构建 + QEMU 启动 |
| `kernel` | `make kernel` | P1 | A01–A07、A14、A17 |
| `drivers` | `make kernel` | P1 | `kerneldebug-module` 等产物清单比对 |
| `middlelayer` | `make userland` 前置 | P2 | Rust 目标产物与 `runtime.c` 链接顺序一致 |
| `userland` | `make userland` | P2 | A06（删源不残留）、来宾加载 |
| `esp` / `staging` | `make rootfs` | P3 | rootfs 增删改名/权限/软链接 fixture |
| `sdk` | `make sdk` | P2 | 最小程序编译链接；`leonos-musl-cc` 包装器为 C |
| `image-vmdk` | `make image-vmdk` | P3 | 分区与文件系统校验 + QEMU 启动 |
| `image-iso` | `make iso` | P3 | 同上 |
| `installer` | `make installer` | P3 | 安装 + 升级（含已装 Alpine build-base/xorriso 的旧系统） |
| `release` | `make all` + 发布脚本 | P3 | 产物哈希与清单 |
| `run` / `run-debug` / `run-iso` | `make run` / `run-iso` | P3 | 来宾成功标记 + 退出码 |
| `clean` | `make clean` / `make distclean` | P1 | A12：拒绝空值、`/`、源码根、无所有权标记的 O |
| `test component-config` 等 | `make test-legacy` | P2/P3 | 显式列出 Python 依赖 |
| `musl-*`、`busybox`、`lua`、`file`、`sqlite`、`nano`、`sl`、`cmd`、`less`、`tcc`、`portablegl`、`fastfetch`、`python`、`storage-upstream`、`auth-upstream` | `make fetch` + `scripts/reliefos/Makefile.third-party` | P2/P3 | 锁文件摘要校验、离线全量构建 |
| `apk-*`、`openrc`、`rpr-*`、`app-manifests`、`grub-*`、`gbk-table`、`image-*` 资源 | `make apk-repo` / `make rootfs` / 资源 C 生成器 | P3 | 包归属/签名、镜像内容比对 |
| `staging-prune`（`build.py:2553-2660`） | 无等价物：改为"空 staging 起步 + 成员清单 + 原子发布" | P3 | A06、A12 |

### 2.1 P2-a 之后的实际进度（2026-09-19）

上表中 `musl-*` 行的 **musl + mimalloc sysroot** 与 **`make fetch`/锁文件** 两半已落地：
`configs/dependencies.lock.json`（41 条，`schema_version=1`）+ `tools/host/manifest/{json.c,leonos-deps.c}`
+ `tools/build/fetch.sh` + `scripts/reliefos/Makefile.third-party` + `tools/build/musl-sysroot.sh`。
产物等价性与逐字节比对结果记在 `verification.md` 第 6 节。

同一次改动把 `configs/auth-upstream.json`、`configs/storage-upstream.json`、
`configs/openrc-packages.json` 的职责并入锁文件；三个旧文件**暂不删除**，因为
`auth-upstream`、`storage-upstream`、`apk-root` 三个消费者仍在旧 `build.py` 里。
它们迁移到新目标时改读锁文件，删除时机记入 `legacy-removal.md`（P4）。

`busybox/lua/sqlite/...` 等其余上游组件、以及 `userland/runtime/sdk` 仍待迁移。

## 3. 已实证的结构性障碍

这些不是"改造一下就行"的差异，而是新系统必须显式解决、否则验收矩阵会直接挂掉的点。每条都给出可复核位置。

1. **`build-info` 被塞进每一次运行。** `build.py:4213-4217` 的 `build_roots()` 对除
   `BUILD_NUMBER_EXEMPT_TARGETS` 之外的每个目标前置 `build-info`。实测重复 `run kernel` 会把受跟踪的
   `buildsystem/state/build_number.txt` 从 3833 递增到 3836，重写受跟踪的
   `include/generated/build_info.h`，进而重编译 `kernel/ntclks/version.c` 并重链接全部 87 个内核对象。
   → 违反 A02 与 A17；新系统按 P1 直接消除（版本头移到 `$(O)/generated`，不再自增计数）。
2. **Linux-PAM 只有 Meson/Ninja 路径。** 实证：`build/auth-upstream/src/Linux-PAM-1.7.2/` 内只有
   `meson.build` 与 `meson_options.txt`，无 `configure`/`configure.ac`/`autogen.sh`；
   `tools/build_auth_upstream.py:157-164` 调用 `meson setup/compile/install`；`build.py:794` 的
   `auth-upstream` 被 `storage-upstream`、rootfs、SDK 依赖。
   → **用户裁定：手写 Makefile 移植**，不降级上游、不开 Ninja 例外、不删认证。细节见第 5 节。
3. **SDK 编译器包装器是 Python。** `tools/package_musl_sdk.py:49-50` 把
   `tools/leonos_musl_cc.py`（40 行）安装为 SDK 的 `bin/leonos-musl-cc`，`build.py:1546` 更在生产
   链接步骤直接执行它。`tools/package_devtools.py` 的 SDK zip 是第二处。
   → 违反计划第 2 节"执行链不运行 Python，含生成的包装器"。裁定在 P2 用 C 移植（纯 argv 改写）。
4. **追加式静态库。** `build.py:1426,1436` 用 `ar rcs` 增量追加对象。删除源文件后旧成员会残留在
   archive 里。→ A06 要求改为"重建到新临时文件"。
5. **staging 差集剪枝依赖上一次状态。** `staging-prune` 是一个约百行的 action
   （`build.py:2553-2660`），`esp:layout-links` 有 200 多个链接项。Make 没有原生等价物。
   → 只能靠成员清单 + 从空 staging 起步 + 原子发布重做（P3）。
6. **旧系统没有构建级互斥。** `flock` 只出现在 `make_image`、APK bootstrap 等处；计划所称"执行锁"是
   内核特性。同一输出目录并发构建无保护。→ 新系统已按 §6.3 的"明确拒绝"分支实现并测
   （`scripts/build-lock.sh`，证据见 `verification.md` 第 7 节）。实测过 unprotected 的真实伤害：
   两个共享 O 的 make 会让先起的那个在 `host/kconfig-frontends` 上失败。
7. **可重现性被随机值封死。** `tools/apk_distribution.py:421` 用
   `version = f"0.{time.time_ns()}-r0"` 生成包版本；`tools/make_image.py:114,128` 用
   `uuid.uuid4()` 生成每个分区 UUID 与 disk GUID。→ A13 在固定 `SOURCE_DATE_EPOCH` 下必然失败，
   P3 需改为可派生的稳定标识并记录。
8. **rootfs 组装以 rootless 名义自举提权。** `tools/make_ext2_root.py:20` 用
   `fakeroot -- sys.executable <self>` 重启自己。→ 计划第 10 节要求普通构建不要求 root，这里同时是
   Python 执行链与权限边界问题，P3 处理。
9. **资源生成器依赖 Python。** `prepare_ui_font.py`（290 行，真 TTF 改写器）、
   `make_app_icons.py`（62 输出）、`app-manifests`（67 输出）、`generate_gbk_table.py`、
   `gen_loader_integrity.py`、`make_grub_font.py`。计划第 7 节要求可重现的 C 生成器。
   `third_party/libpng`、`third_party/zlib` 可直接作为 host C 依赖复用。
10. **多输出/未声明输出遍地。** `image-vmdk` 4 个、`musl-ltp` 22 个、`app-icons` 62 个、
    `app-manifests` 67 个输出，而 `image-iso` 的 `efiboot.img` 未登记。→ A07 需要 GNU Make 分组目标
    `&:` 或等价证明。

## 4. 宿主与依赖事实

GNU Make 4.4.1、GCC 16.2.1、Clang 22.1.8、QEMU 11.1.1、`ld.lld`、`llvm-ar`、`llvm-objcopy`、
`xorriso`、`mtools`、`qemu-img`、`curl`、`flock`、`gperf`、`autoconf`、`automake`、`libtool`、`flex`、
`bison`、`msgfmt`、`patch`、`tar`、`xz` 均可用；`mke2fs` 来自
`/opt/android-sdk/platform-tools/mke2fs`（`make doctor` 必须校验这类路径歧义）。
`squashfs-tools` 缺失，但当前镜像链未使用。22 个 submodule 已检出，含 `kconfig-frontends`
（C 前端，v3.12.0.0，autotools 构建，符合计划第 9 节对上游 configure 的放行）与 `zlib`（含
`contrib/puff/puff.c` 参考解压器，许可清晰，可直接 host 复用）。

磁盘：`/home/xiaobai/Projects` 挂载点剩余 387G，现有 `build/` 占 78G。

目标工具链默认值是 LLVM 家族：`build.py:751-754` 用
`os.environ.get("CC"/"RUSTC"/"LD"/"OBJCOPY", ...)`，即**未声明的环境变量会静默改变工具选择**，
正是计划第 6.2 节点名要禁止的行为。`make doctor` 需校验 triple、链接器与 compiler-rt。

## 5. 调研稿的位置与可信度

三份逐项目底稿入库在 `docs/build/research/`（未复核的调研稿，不是验收证据）：

- `buildpy-targets.md`：429 行，逐目标输入输出与机制层
- `tools-scripts.md`：309 行，`tools/` 生产脚本逐条归属判断
- `python-ninja-audit.md`：691 行，Python/Ninja/Meson 调用点与 PAM 移植底稿

我对其中 8 条关键论断做了独立复核：**6 条成立**（第 3 节第 1、2、3、4、5、7、8 条即为其代表），
**2 条需纠正**：

- "SQLite 构建隐藏 `tclsh` 依赖"—— 不成立。`tools/build_sqlite.py:79-84` 实际是把上游
  `Makefile.linux-gcc` 复制到临时目录后 `make -f ... sqlite3.c sqlite3.h`，依赖是 `make`。
- "157 个目标声明点"成立，但"144 个字面量目标"应为 146 处 `name="`；差异来自统计口径，引用时以本文为准。

因此这三份稿子**不能直接当验收证据**。它们的价值是索引与线索，逐条结论需要在 P1–P3 实施时按新系统的
实际行为重新落证据。

PAM 移植的可执行结论（来自底稿，方向与我的独立复核一致）：难度低于直觉——45 个
`modules/*/meson.build` 是同一份 `module-meson.build` 的符号链接（可压成一张表）；`docs=disabled`
已切断整条 XML/xslt 链；flex/bison 只服务于**不安装**的 `pam_conv1`；构建期不需要以宿主方式运行任何
目标端 ELF。主要风险是 18 个 musl 函数探测必须"交叉链接但不执行"、`meson install` 会剥离链接期
RPATH 因此手写版必须自行处理（否则会把绝对工作区路径写进产物，违反计划第 10 节 SDK 可移动前缀要求）、
以及 `faillock.c`/`opasswd.c`/`bigcrypt.c` 同名源的多产物对象冲突。底稿另指出现网 meson 交叉探测链
本身是坏的（`-fuse-ld=lld`/`--rtlib` 混入 cross `c=` 导致 22 个加固旗标被静默丢弃），这属于**既有缺陷**，
是否顺带修需单独决策。

## 6. 尚未完成的部分（P2-c 之后）

已完成并提交的：P0 台账与基线、P1（Make 入口 + 4 个 C 宿主工具 + 内核闭环 + 契约测试）、
P2-a（依赖锁、`make fetch`、musl sysroot）、P2-b/P2-c（同 O 互斥、A08/A10/A16 长测）。

**仍缺的产品能力**（每项都还是 `exit 2` 的显式拒绝，不存在"看起来完成"的假象）：

- **P2-a3 认证链（第一段已交付）**：`linux-headers`（`make headers_install`）与 `libxcrypt`
  （上游 configure）已由 `tools/build/auth-upstream.sh` + `make leonos-auth` 构建并通过
  23 项契约（`verification.md` 第 11 节，含 `crypt.h` 逐字节等价与跨路径字节稳定）。
  **仍缺**：Linux-PAM（上游仅 Meson，需手写 Makefile 移植）、libmd/libbsd（libbsd 的
  `-idirafter` 头覆盖顺序要单独照搬）、util-linux、sudo、shadow。用户裁定不降级、不开
  Meson/Ninja 例外、不删认证。libpam 仍是 `runtime` 的硬前置：`userland/auth/*.c` 需要
  `security/pam_appl.h`，而 `libleonos.so.2` 的链接命令把 `libpam.so.0`/`libcrypt.so.2`
  列在 `-lc` 之前（`build.py:1467-1471`）。
- **P2-a2 runtime**：`libleonos.so.2`（`build/system/lib/`）+ `libleonos.a` /
  `libleonos-installer.a`（`build/musl/lib/`）。源集合、flags 与链接 argv 已核到行号，见
  `verification.md` 的 P2 计划段；`ar rcs` 追加缺陷（`build.py:1426/1436/2114`）改为
  `rm -f tmp && ar rcsD tmp && mv`。
- **P2-a2 userland**：62 个 app-kind 组件（`configs/components.toml` 驱动，`_BUILD` 符号来自
  `tools/generate_component_kconfig.py`）→ 需要组件清单工具（用已有的 `json.c`，不许 sed 解析）。
- **P2 SDK**：`tools/leonos_musl_cc.py` 的 C 移植 + `package_musl_sdk.py`/`package_devtools.py`
  两条打包路径；A16 目前**无法**覆盖 SDK，因为包装器还是 Python。
- **P3**：资源生成器（字体/图标/GBK 表）、APK 仓库、rootfs 组装与 staging 剪枝、三类镜像；
  含 `time_ns()`/`uuid4()` 两处不可重现源（`apk_distribution.py:421`、`make_image.py:114,128`）。
- **P3/P5 运行验证**：`scripts/reliefos/Makefile.run` + QMP/截图成功标记判定（5.1 节的受阻项），以及安装升级
  保留用户环境的实测。
- **P4 旧实现删除**：见 `legacy-removal.md`。`build.py`(4958 行)、`buildsystem/**`(48 个 py)、
  `tools/*.py`(198) 全部仍在；两个 GitHub 工作流与 `AGENT.md`/`README.md`/`docs/BUILDSYSTEM.md`
  仍指向旧入口，因此 A17 的"CI/文档无活跃旧入口"未达成。

**仍缺的验收证据**：A01 的"全目标"、A11 的断网全量、A13 的镜像/SDK 级哈希、A15 全部、
A16 的 SDK 与镜像链、A10 的写失败与真 Ctrl-C、P1-d 的来宾启动。
