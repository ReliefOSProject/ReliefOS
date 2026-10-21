# LeonOS 4 构建迁移验证记录（P0 与 P1 局部）

> 历史验证记录：以下路径、命令和结果对应记录时的构建链。当前 Kbuild 接口与验收见 [Kbuild 迁移记录](kbuild-migration.md)。

> 当前全量迁移结果见 [全量迁移记录](full-migration-2026-09-19.md)，以下为历史分阶段证据。

> 接管后的新增验证及修正见 [接管记录](takeover-2026-09-19.md)。本文原有通过记录只对应当时提交和范围，尤其不能代替当前锁实现或新 PAM/SDK 链的验收。

配套计划：原构建重建计划（已清理，历史版本可从 Git 查看）
分支：`xiaobai/dev/buildsystem`，基线提交 `6be4c69`
记录时间：2026-09-19（宿主 Linux 7.2.4-3-cachyos-bore-lto，x86_64）

本文只写**实际执行过并看到输出**的检查。第 5 节逐条列出未执行项。凡未执行的验收编号，一律写"未运行"，
不以 skip 状态冒充通过（计划第 13 节要求）。

## 1. 环境与依赖探测

命令：`make --version`、`gcc --version`、`clang --version`、`qemu-system-x86_64 --version`、
逐工具 `command -v`、`git submodule status`、`df -h .`

结果：GNU Make 4.4.1；GCC 16.2.1；Clang 22.1.8；QEMU 11.1.1；`ld.lld`、`llvm-ar`、`llvm-objcopy`、
`llvm-config`、`xorriso`、`mformat`、`mkfs.fat`、`qemu-img`、`curl`、`flock`、`gperf`、`autoconf`、
`automake`、`libtoolize`、`flex`、`bison`、`msgfmt`、`pkg-config`、`patch`、`tar`、`xz`、`install`
均存在。

两点必须记进 `make doctor` 的设计：`mke2fs` 解析到
`/opt/android-sdk/platform-tools/mke2fs`（不是 e2fsprogs 的系统路径）；`squashfs-tools` 不存在，
但当前镜像链未使用它。22 个 submodule 全部已检出，含 `third_party/kconfig-frontends`
（kconfig-frontends-3.12.0.0）与 `third_party/zlib`（v1.3.2，含 `contrib/puff/puff.c`）。

磁盘：`/home/xiaobai/Projects` 已用 312G、可用 387G；现有 `build/` 占 78G。

## 2. 旧系统行为基线（P0-b）

测量方法：`tests/build/measure.sh`，把被测命令放进独立进程组，每 0.2 秒采样该组 RSS 之和取峰值。
共享页按进程各计一次，因此数值**跨运行可比**，但不等于唯一驻留集。GNU time 未安装（首次尝试以
rc=127 全表失败），故改用此自研脚本，新旧系统将使用同一方法。

| 用例 | 命令 | 退出码 | 墙钟 | 峰值组 RSS | 日志 |
| --- | --- | --- | --- | --- | --- |
| kernel-1 | `python3 build.py -v run kernel` | 0 | 1.52 s | 150 MB | `buildsystem/logs/migration-baseline/kernel-1.log` |
| kernel-noop-again | 同上，紧接第二次 | 0 | 1.53 s | 149 MB | `.../kernel-noop-again.log` |
| sdk | `python3 build.py -v run sdk` | 0 | 146.7 s | 1.25 GB | `.../sdk.log` |
| image-vmdk | `python3 build.py -v run image-vmdk` | 0 | 193.4 s | 1.58 GB | `.../image-vmdk.log` |
| image-iso | `python3 build.py -v run image-iso` | 0 | 198.4 s | 1.64 GB | `.../image-iso.log` |
| installer | `python3 build.py -v run installer` | 0 | 266.7 s | 1.60 GB | `.../installer-retry.log` |
| all | `python3 build.py -v run all` | 重跑进行中 | — | — | `.../all-retry.log` |

`image-iso` 日志尾部为 `Started 6 tasks / Entered 159 folders / Built 563 files / Generated 424
files / Ran 541 commands / Downloaded 0 files / 0 errors`。
`installer` 日志尾部为 `Started 12 tasks / Entered 162 folders / Built 565 files / Generated 427
files / Ran 545 commands / Downloaded 0 files / 0 errors`。

**首次 installer 与 all 采集的 rc=126 不是构建失败**，是我自己的工具链缺陷：批量缩进规范化用
`expand -t4 $f > $f.tmp && mv` 覆盖了 `tests/build/measure.sh`，`mv` 带上了重定向产生的 0644，
可执行位丢失，于是 `"$MEASURE"` 无法执行。已 `chmod +x` 并以新文件名重跑，上表取自重跑结果。

产物哈希（本轮旧系统实际产出）：

```
b93d66628f80796e0b837fae3c229f44176390f4f9672e64d49394cd80ac087c  build/images/leonos4.vmdk
8a672b5f5e52f29a35bee3028dd8513e12095ef8c8d9134b93d2a05c8804cf6a  build/images/leonos4.iso
87b7da68dee236932f8e0d4c787e327d6dab6d4c8b0d4043b3fd028d0d6e6f2c  build/system/kernel.sys
357275d0b76111905491f875768def6069cea5dbe5f7536e9f033d08a66f51ea  build/system/kernel.debug
2c7c89ced5b5d219fe2e13bcdd925a75ad8c5d4d121a217f9c010c2e46690479  build/images/leonos4-installer.iso
```

本轮**未在 QEMU 中启动**这些镜像，因此它们只是文件产出证据，不构成第 11 节要求的运行验证。

### 2.1 关键发现：旧系统的"重复构建"不是空操作

对 `kernel-noop-again.log` 逐行核对，第二次 `run kernel` 实际执行了 7 条命令：

```
python3 tools/generate_component_kconfig.py
python3 tools/build_info.py --header include/generated/build_info.h --state buildsystem/state/build_number.txt
python3 tools/kconfig_sync.py --config buildsystem/config/leonos.conf ...
clang -target x86_64-unknown-none -O3 ... -c kernel/ntclks/version.c -o build/obj/kernel/kernel/ntclks/version.c.o
ld.lld -nostdlib -z max-page-size=0x1000 -T kernel/ntclks/arch/x86_64/linker.ld -o build/system/kernel.unstripped <87 objects>
llvm-objcopy --strip-debug ... ; llvm-objcopy --only-keep-debug ...
```

机制在 `build.py:4213-4217`：`build_roots()` 给除 `BUILD_NUMBER_EXEMPT_TARGETS` 外的每个目标前置
`build-info`，而该目标是 `always=True`。实测两次构建把受跟踪的 `buildsystem/state/build_number.txt`
从 3833 推到 3836，并重写受跟踪的 `include/generated/build_info.h`。

→ 结论：A02（空操作不重编译）与 A17（受跟踪文件零变化）在旧系统上**本来就不成立**。新系统必须做到，
而不是"沿用旧行为"。

## 3. 新 C 宿主工具（P1-a，TDD）

### 3.1 红灯

命令：`gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Wformat=2 -Wshadow -Wstrict-prototypes
-Wmissing-prototypes tests/host/test_common.c`

首次输出（正确的失败原因——生产实现不存在）：

```
tests/host/common/... fatal error: ../../tools/host/common/buffer.h：没有那个文件或目录
```

红灯过程中还暴露了测试脚手架自身的一个真实缺陷：`test-support.h` 检查 `rewind()` 返回值，
而 `rewind()` 返回 `void`，`-Werror` 下编译失败。已改为 `fseek(stream, 0, SEEK_SET)`。

### 3.2 绿灯

`tools/host/common/{buffer,io,process}.{c,h}` 实现后：

| 检查 | 命令 | 结果 |
| --- | --- | --- |
| GCC 严格警告集 | `gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes` | 编译无输出，`ok - host/common: 74 checks passed`，rc=0 |
| Clang 同一警告集 | `clang` + 同上 | `ok - host/common: 74 checks passed`，rc=0 |
| ASan + UBSan + 泄漏检测 | `gcc -g -O1 -fsanitize=address,undefined` 后 `ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1` 运行 | rc=0，无 sanitizer 报告 |

覆盖的契约（计划第 8 节）：扩容保留旧内容、`SIZE_MAX` 请求被拒且旧缓冲不丢、
`buffer_destroy` 接受零值结构、按请求 mode 建文件、零字节输入产出空文件、
**内容相同则 mtime 与 inode 均不变**、内容变化则原子替换且目录内不留临时文件、
只读目录中发布失败**且原文件完好**、子进程正常退出、非零退出码进 `child_status`、
信号死亡用 `WIFSIGNALED`/`WTERMSIG` 可辨、程序不存在与非existent 工作目录返回 -1 且置 `errno`、
`working_dir` 真实生效。

### 3.3 `leonos-emit`

单独编译通过（同一严格警告集），手工契约验证：

| 场景 | 观察 |
| --- | --- |
| 首次写入 `--mode 0600` | 文件建立，权限 `-rw-------` |
| 相同内容再写一次 | mtime 纳秒级完全不变（`...08.468586876`） |
| 不同内容再写 | 内容更新，mtime 改变 |
| `--help` | 打印用法，rc=0 |
| 缺 `--output` | `leonos-emit: --output is required`，rc=1 |
| 目标目录只读 | `cannot publish ...: Permission denied`，rc=1，原文件保留 |

注意：`leonos-emit` **尚未有 Make 目标**，以上是我用 `gcc` 直接编译二进制后手工执行的。它被计入
"已编译并验证行为"，不计入"已接入构建系统"。

## 4. 新 Make 系统的 P1 结果

### 4.1 契约测试

| 套件 | 命令 | 结果 |
| --- | --- | --- |
| C 单元测试（普通 + ASan/UBSan） | `make test-tools` | `ok - host/common: 74 checks passed` 与 `test_json` 各两次，rc=0 |
| 入口契约 | `sh tests/build/test-bootstrap.sh` | **22 checks, 0 failures** |
| 并发互斥（A09） | `sh tests/build/test-concurrency.sh` | **17 checks, 0 failures** |
| 依赖锁 CLI | `sh tests/build/test-deps.sh` | **44 checks, 0 failures** |
| 增量契约（A02–A07） | `sh tests/build/test-incremental.sh` | **21 checks, 0 failures** |
| 可重现性（A13） | `sh tests/build/test-reproducible.sh` | **11 checks, 0 failures** |
| 第三方/sysroot（A11 部分） | `sh tests/build/test-third-party.sh` | **29 checks, 0 failures** |
| 长测（A08/A10/A16） | `make test-long` | `test-execchain` **20 checks**、`test-jobs` **19 checks**，均 0 failures |

`make test` 的 rc=0 记录：P1+P2-a/P2-b 批次跑为 2:05（`make -j12 test`，宿主同第 1 节）；
`make test-long` 两个套件合计约 6 分钟，独立于 `make test`。

### 4.2 目标面

`make help`（PATH 中无编译器时仍可用）、`make doctor`、`make tools`、`make defconfig`、
`make kernel`、`make fetch`、`make test-tools`、`make test-build`、`make test-long`、
`make clean` 已实现并实测；`userland`、`runtime`、
`sdk`、`rootfs`、`apk-repo`、`image-vmdk`、`iso`、`installer`、`run*`、`test-smoke`、`test-legacy`
由 `scripts/not-migrated.sh` 显式 **exit 2**，不伪装成功。

### 4.3 验收行实测证据

| 编号 | 观察 |
| --- | --- |
| A02 | 第二次 `make kernel` 输出的 `CC/AS/LD/IMAGE/GEN/HOSTCC/CONFIG` 动作数为 **0**；`$(O)/obj/kernel` 下全部 85 个对象与 `kernel.sys`、`kernel.debug`、`kernel.unstripped`、`autoconf.h`、`boot_logo.h` 的 mtime（含纳秒）逐项不变 |
| A03 | `touch kernel/ntclks/futex.c` → 恰好 1 次 `CC`（且就是该文件）、1 次 `LD`、2 次 `IMAGE` |
| A04 | `touch kernel/ntclks/include/ntclks/types.h` → 重编译数 **恰好等于** depfile 记录了该头的对象数 **82**，既无漏编也无过编；随后 1 次 `LD` |
| A04b | `touch kernel/ntclks/arch/x86_64/linker.ld` → 0 次 `CC`、1 次 `LD` |
| A05 | `KERNEL_CFLAGS=-DLEONOS_SIG_PROBE` → `$(O)/obj/kernel` 下 **83 个 `.c.o` 的 mtime 全部改变、2 个 `.S.o` 一个都没改变**（逐个对象比较两次快照，不看 Make 的日志流）；`kernel.sys` 等产品 mtime 改变 |
| A09 | 见第 7 节：同 O 双进程已被显式拒绝，不同 O 并行互不串用 |
| A06 | 移走 `kernel/ntclks/signalfd.c` → 重新链接，且 `make -n` 的输出不再包含该对象；恢复后再链接一次 |
| A07 | 删除 `kernel.sys` 或删除 `boot_logo.h` → 各自被重新生成（多输出阶段无 stamp 误判） |
| A12 | `O=` 空、`O=/`、`O=.`（含解析为源码根的相对形式）、含空格、含换行、含非法字符全部被拒；`clean` 对无所有权标记的 O 拒绝执行；未知 `CONFIG_*` 命令行覆写报错 |
| A14 | `tools/host` 与 `tests/host` 在 GCC 16 与 Clang 22 的 `-std=c11 -Wall -Wextra -Wpedantic -Werror -Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes` 下零告警；ASan+UBSan（`detect_leaks=1`、`halt_on_error=1`）无报告 |
| A17 | 新构建不改动任何受跟踪文件：`buildsystem/state/build_number.txt` 与 `include/generated/build_info.h` 哈希保持 `ebdbfc0b` / `b135f754`；跨文件系统 `O=/tmp/...` 构建后源树 `git status` 干净，且 `kconfig-frontends.sh` 会回收它留下的 `.tmpconfig.*` 与空 `include/config` |

### 4.4 产物等价性（内核）

新系统从空 `O` 产出的 `kernel.sys` 为 **1196352 字节**，与旧基线 `build/system/kernel.sys` 同尺寸。
两者不可能逐字节相同：旧基线嵌入 `LEONOS_KERNEL_VERSION "4.7.1-3838"` 与调用时刻的
`LEONOS_BUILD_TIME`，新系统按 §7 不递增计数、不读挂钟。逐字节比较需在两侧固定
`SOURCE_DATE_EPOCH` 与 `BUILD_ID` 后进行，属 A13，**本轮未做**。

`leonos-boot-logo` 与退役的 `tools/generate_boot_logo.py` 输出**逐字节一致**（仅第一行署名不同），
`diff` 只报告第 1 行；`leonos-version` 重放出同名同形状的宏集合。

## 5. 验收矩阵状态

| 编号 | 状态 |
| --- | --- |
| A02 A03 A04 A05 A06 A07 A12 A14 A17 | **内核范围内通过**（证据见 4.3；A12/A14/A17 只覆盖已实现部分） |
| A01 | 部分：干净 O 可构建内核与 musl sysroot，`make fetch` 已实现并验证（见 6.2），但 `userland/runtime/sdk/镜像` 目标未迁移，故"全目标"未达成 |
| A08 | **通过**：`-j1` 与 `-j8` 各三轮（含一轮 82 对象的重轮），选中的对象集合两侧**逐行相同**，六个产物 SHA-256 两两相同（见 7.4，19 项） |
| A09 | **通过**：同 O 双进程按"明确拒绝"契约实现并有变异检验；两个不同 O 并行互不串用；嵌套 make 继承同一 O 的锁不死锁（机制与证据见第 7 节，17 项） |
| A10 | **部分通过**：中断恢复已证（SIGTERM 落在工作中、无伪产物、重跑逐字节恢复，见 7.5）。**未做**：磁盘写失败（需容量受限文件系统）、交互 Ctrl-C（需 pty）、子进程失败的独立 fixture |
| A11 | 部分通过：`make fetch` 下载 19 项并逐一校验 SHA-256（实测 `alpine-findutils` 摘要与锁文件一致）；`--verify-only` 在缺项时列出 id 并非零退出、不联网。**未覆盖**：断网下完成全套生产目标（属 P4） |
| A13 | **通过**：固定 `SOURCE_DATE_EPOCH=1700000000` + `BUILD_ID` 下，两个不同深度的干净 O 全量重建，`autoconf.h`、`boot_logo.h`、`build_info.h`、`obj/kernel/sources.list`、`kernel.unstripped`、`kernel.sys`、`kernel.debug` 七个产物 SHA-256 **两两相同**；原地重建后镜像不变（`tests/build/test-reproducible.sh`，11 项） |
| A15 | **未运行**：SDK/APK/三类镜像与升级尚未迁移 |
| A16 | **已迁移范围内通过**（见 7.6）：`defconfig`(4259)、`tools`(204)、`kernel`(348)、musl sysroot(3040)、暖缓存 `fetch`(100)、缓存校验(97) 六条 execve 跟踪**零** python/meson/ninja，且每条都断言跟踪非空。**未覆盖**：SDK 与镜像链（未迁移） |
| A18 起 | 不适用 |

### 5.1 P1-d 的来宾运行验证：受阻，未通过

`make kernel` 的产物已在 QEMU 中尝试启动，但**判定不成立**，因此本项记为受阻而非通过：

1. 首轮用 `-bios` 指向不存在的路径，QEMU 直接退出；对照组同样失败，属夹具缺陷。
2. 次轮不带固件（SeaBIOS）：`control`（旧 `build/system/kernel.sys`）与 `candidate`
   （新 `out/.../kernel.sys`）的串口日志**完全同形**，都停在
   `Booting 'LeonOS 4 Live Desktop (musl + Vim + GCC)'` 后重新回到 GRUB 横幅。
3. 三轮用 `/usr/share/edk2/x64/OVMF.4m.fd`：对照组的串口变成 EDK2 自身的异常寄存器转储
   （`Find image based on IP(...)`、`CR0/CR3/IDTR` 等），说明该固件镜像与 q35 + 这张
   `--bios` 生成的 ISO 组合不可用。

新旧内核在所有配置下行为一致，因此**没有回归证据**，但也**没有成功启动证据**。
按 AGENT.md 第 7 节"编译通过不等于镜像已含文件；镜像已生成不等于虚拟机能启动"，
以及计划第 11 节"测试必须识别来宾成功标记并核验退出状态，启动 QEMU 进程本身不算通过"，
本项必须等 `mk/run.mk` + QMP/截图判定（P3）落地后重做。用于对照的两张 ISO 与临时产物已删除。
## 6. P2-a：依赖锁文件、`make fetch` 与 musl sysroot

### 6.1 锁文件成为唯一权威

`configs/dependencies.lock.json`（`schema_version=1`，41 条）合并了原先分散在三处的同类职责：
`configs/auth-upstream.json`（7 条）、`configs/storage-upstream.json`（3 条）、
`configs/openrc-packages.json`（5 条 APK，其 URL 原先是 `repository + 名字 + 版本` **拼出来的**，
现在逐条存为字面 URL），另加 23 个 `third_party/` 子模块的固定提交、LTP、Linux 6.12 头、
fastfetch 与 apk-tools-static。补丁不再"构建时临时算摘要"，而是 `{"path","sha256"}` 钉死：
`patches/musl/0001-...patch` = `e336ed20…`，与旧系统留下的 `.leonos-musl.json` 记录一致。

解析与校验只有一处：`tools/host/manifest/json.c`（严格 JSON 读取器，66 项 C 契约测试）+
`leonos-deps` CLI（44 项 shell 契约测试）。计划第 5 节禁止"用正则冒充 JSON 解析器"，
因此 Make 侧只在解析期对锁文件整体取摘要放进签名，从不切片读字段。

三个旧清单文件**仍然存在且仍被旧 `build.py` 使用**；它们的消费者（`auth-upstream`、
`storage-upstream`、`apk-root`）迁移到新目标时改读锁文件，随后删除旧文件。本轮不提前删，
以免旧构建系统在本次交付里失去依据。

### 6.2 `make fetch` 与离线契约

`tools/build/fetch.sh` 逐行消费 `leonos-deps --fetch-list`（`id⇥sha256⇥url⇥缓存名`）：
下载到 `.<name>.partial.$$/`、校验、再用 `link()` 原子发布；摘要不符**直接失败且不回写锁文件**。
实测：19 项全部下载并校验通过（`cache/downloads` 176 MB），第二次 `make fetch` 全部命中缓存、
零联网；`--verify-only` 在空缓存下逐一列出缺失 id 并非零退出。并发不需要锁文件：
两个进程各自下载自己的 partial，`link()` 只有一个赢，输的一方复用已存在文件并重新校验。

### 6.3 musl sysroot 的产物等价性

`mk/third-party.mk` + `tools/build/musl-sysroot.sh`（POSIX sh + musl 自带 configure/Makefile，
无 Meson/Ninja/Python）产出 `$(O)/sysroot/musl`。与旧基线 `build/musl/sysroot` 逐文件比较：

| 项 | 结果 |
| --- | --- |
| 文件数 | 239 = 239 |
| 全部文件 SHA-256 | **238 个相同**；唯一差异是 `.leonos-musl.json`（`indent=2` 排版不同，`json.load` 后内容完全相同） |
| `lib/libc.so` / `lib/libc.a` | SHA-256 逐一相同 |
| `config.mak` | 归一化前缀路径后逐行相同 |
| 重复 `make` | no-op（stamp mtime 不变，无 `SYSROOT/RESET/PATCH` 动作） |
| 耗时 | 首次约 75 s（`-j8`，含 configure/make/install/mimalloc） |

`tests/build/test-third-party.sh` 29 项把上述内容固化为契约：14 个链接产物既声明又存在、
stamp 记录的提交/补丁摘要与锁文件一致、锁文件内容变化会移动 sysroot 签名（保持 mtime 也移动，
证明走的是摘要而非时间戳）、校验失败的缓存文件不会被 `--verify-only` 删除。

### 6.4 本轮由测试暴露并修掉的两个自身缺陷

1. **测试计数缺陷（严重）**：`test-bootstrap.sh` 的 `expect_output_contains` 与 `expect_failure`
   打印 `FAIL` 但**没有累加 `failures`**，因此第 4.1 节记录的 "20/20" 中，关于
   `ARCH/PROFILE/O` 拒绝与 `HOSTCC` 分组的断言**永远不会使套件失败**。已补计数，并把判定改为
   双保险：`mk/tests.mk` 现在既看退出码也扫描输出中的 `FAIL - ` 行。修好后 bootstrap
   的真实计数是 22/22，且 `make doctor` 被改为显式打印 `HOSTCC`、`TARGET_CC`、`TARGET_LD`
   三行，使"宿主与目标编译器相互独立"成为可断言的事实而不是空话。
2. **A04 断言自身的子串匹配缺陷**：`test-incremental.sh` 原来用
   `grep -lF "$header"` 统计"哪些 depfile 引用了这个头"，而 depfile 里的路径是子串关系
   （`ntclks/signal.h` 被 `posix/signal.h` 之类前后缀包含时同样命中），于是期望值比真实
   消费者多一个，表现为"改了 types.h 只重编 81 个"。现改为按**整词**统计；A05 也从
   "对象数 vs 磁盘计数"改成"干净构建编译过的源文件**集合** vs 本次重编集合"，
   失败时直接点出是哪个对象没重编。修好后的三轮串行/并行复跑结果见本节末尾。
3. **同 O 并发的签名争用**：`$(O_META)/<class>.candidate` 原先是固定文件名，两个共享同一
   输出目录的 make 进程在解析期会互相覆盖对方的 candidate，于是某个进程可能提升一份
   **自己从未计算过**的签名。上一轮把它改成 `$(O_META)/<class>.$(MAKEPID).candidate` 并记录为
   "已修"，**这个结论是错的**：GNU Make 在 POSIX 平台上不把 `MAKEPID` 定义出来，实测
   `make -f x.mk` 里 `MAKEPID=[]`，于是文件名退化成 `<class>..candidate`，仍然是共享的。
   本轮改用 `LEONOS_PARSE_ID := $(or $(MAKEPID),$(shell echo $$$$))`（后者取一个子 shell 的 pid，
   `:=` 保证每个 make 进程只展开一次），并确认 `$(O)/meta` 下的 candidate 名字真的带上了 pid。
4. **A05 读的是从未写入的文件**：把 A05 的度量从"数 Make 日志行"改成"比较两次快照的
   对象 mtime"时，快照被存进了 shell 变量 `before_flags/after_flags`，而下面的
   `changed_objects` 却按文件名读 `$work/flags-before`/`$work/flags-after`。awk 打不开第一个
   文件就什么都比不上，于是稳定报告 `moved=0 of 83`。改成落盘快照后 83/83 才是**真的量出来**
   的。同一次改动还暴露出 `comm` 在混合 locale 下对 C 排序输入告警，已在套件顶部
   `export LC_ALL=C`。
5. **同 O 双 make 并未被拒绝**（上一条遗留的问题）：见第 7 节。

## 7. P2-b/P2-c：并发互斥、并行调度与执行链（A09、A08、A10、A16）

### 7.1 要求与为什么不是 flock

计划第 6.3 节要求：同一 O 的两个顶层构建必须**明确拒绝其中一个**或经测试安全串行；不同 O
可以并行；递归 make 不得重复获取外层锁而死锁。

先试的是 `flock`：Make 无法为整个构建持有一个描述符（每条 recipe 各自 fork shell），也没有本
构建可依赖的退出钩子——**实测 GNU Make 4.4.1 既不执行 `.EXIT` 也不执行 `.STATUS`**（正常退出、
SIGINT、SIGTERM 三种情况下都没有运行它们的 recipe）。所以锁改成"owner 记录 + 进程存活性"：
记录文件名是 `<pid>.<进程启动时钟滴答>`，只要那个进程还在且启动时间一致才算持有；构建被
SIGKILL 也不会永久卡住输出目录。

### 7.2 机制（`scripts/build-lock.sh`，由顶层 `Makefile` 在解析期调用）

- owner 身份 = 进程祖先链上**最近的一个 `make`**（不是跑脚本的那个短命 shell）。
- 判定"谁先"用的是全序 `(启动时间, pid)`：每个 make 先写下自己的记录再扫描目录，
  只在自己不是最早时拒绝。两个进程从同一集合算出同一结论，因此**无论发布如何交错都恰好
  有一个赢家**——"我先到"式的判断在双方都还没写完时会让两个都赢。
- 释放 = 下一次 acquire 时清理死掉的记录（没有退出钩子可依赖）。
- 继承：acquire 成功后导出 `LEONOS_BUILD_OWNER=<pid>.<ticks>:<绝对 O>`。**同一个 O** 的嵌套
  make 继承它（`defconfig` 的递归 `$(MAKE)` 因此不会拒绝自己的外层构建），**不同 O** 的嵌套
  make 各自加锁，所以测试套件在外层 `make test` 之下仍然得到真实的互斥。
- 豁免：`make -n`、`make -q`，以及不写任何东西的 `help`/`doctor`（裸 `make` 也不能要求
  别处没有构建在跑）。
- 依赖 `/proc`；读不到时脚本以 2 退出而不是"假装加锁成功"。

### 7.3 实测（`tests/build/test-concurrency.sh`，17 项）

同一 O：先起的 make 完成构建（85 个对象与 `sources.list` 逐项对得上、`kernel.sys` 非空），
后起的 make **非零退出并打印持有者 pid、启动滴答与记录位置**；`-n`/`help`/`doctor` 在锁被
持有时仍然可用；构建结束后下一条 `make` 不再看到陈旧记录。两个不同 O 并行：各自产出镜像，
各自生成的 `sources.mk` 只指向自己的目录（不串用）。嵌套 make 不自拒。

**变异检验**：把 `Makefile` 里的加锁分支改成永不成立（等价于"没实现互斥"）后复跑，同一 O 的
两个 make 都跑起来，套件报 3 项失败，其中第一项是真实伤害——后起的进程在
`host/kconfig-frontends` 的构建目录上把先起的进程撞失败（`kconfig-frontends: front end build
failed`）。这正是该锁要防的事，也证明这些断言测的是生产规则而不是测试自己的空文件。

长测套件独立成 `tests/long/`，由 `make test-long` 运行（`make test` 只跑 `tests/build/`）：
它们要重复整棵树的构建，按第 13 节不能混进每次改动都要跑的套件里，但也必须是**可运行的目标**
而不是"忘了跑的脚本"。

### 7.4 A08：`-j1` 与 `-j8` 各三轮（`tests/long/test-jobs.sh`，19 项）

两侧都从干净 O 开始，固定 `SOURCE_DATE_EPOCH=1700000000` 与 `BUILD_ID=7`，因此两棵树**按构造
可比**；对象快照记 `%T@`（纳秒）并按各自树的相对路径记录，否则比的是测试自己的目录名而不是
Make 选中的工作集。

| 观察 | 结果 |
| --- | --- |
| 第 1 轮（`types.h`，82 个消费者） | `-j1` 与 `-j8` 移动的**对象集合完全相同**（82 = 82，逐行 `cmp`） |
| 第 2、3 轮（单个 `.c` + 链接脚本） | 两侧同为 1 个对象，集合相同 |
| 产物内容 | `kernel.sys`、`kernel.debug`、`kernel.unstripped`、`sources.list`、`autoconf.h`、`build_info.h` 六个产物 SHA-256 **两两相同**（如 `kernel.sys 4afa332cc79e…`） |
| 签名 | 归一化 `-I<tree>/include` 后，两个 job level 的 `kernel-cc.sig` 相同：工具、身份、flags 一致 |
| 声明 | 四个内核动作类（cc/as/link/objcopy）都发布了 `.sig`；未被本目标用到的类没有签名是**预期**的 |

### 7.5 A10（中断部分）：并行构建被打断不留伪产物

同一套件里用 `-j2` 重编 82 个对象，**等到确实有对象被重写**再发信号（`kill -INT` 一度得到
status=0，原因是 POSIX shell 在关闭 job control 时会给后台命令把 SIGINT 设为 ignore，信号被
吞掉、构建跑完了；改用 SIGTERM 后 status=143，这才是真的打断）。断言与结果：

- 中断确实落在工作中（`objects rewritten while interrupted > 0`，退出码非零）；
- 被中断后 `kernel.sys` 要么不存在、要么仍是上一份完整产物——实测**不存在**，没有半成品被
  Make 当作最新；
- 重跑恢复出**逐字节相同**的镜像（`cmp` 通过）。

**未覆盖**：计划第 13 节要求的"写失败"（容量受限文件系统）与交互 Ctrl-C（需要 pty）。A10 因此
记为**部分通过**，不是全通过。

### 7.6 A16：execve 跟踪（`tests/long/test-execchain.sh`，20 项）

对每个已迁移的生产入口跑 `strace -f -e trace=execve`，然后检查被执行程序的路径与 argv 里的
可执行词。"没有 Python"必须是**看到的**，不是推理出来的。

| 入口 | execve 记录数 | python/meson/ninja |
| --- | --- | --- |
| `make defconfig` | 4259 | 0 |
| `make tools` | 204 | 0 |
| `make -j4 kernel` | 348 | 0 |
| musl sysroot（上游 configure + make） | 3040 | 0 |
| `make fetch`（硬链接暖缓存，不联网） | 100 | 0 |
| 缓存校验（`--verify-only` 路径） | 97 | 0 |

每条目还断言"跟踪确实看到了真实工作"（execve 记录 >20 且包含编译器/make），否则"零命中"是空
跟踪造成的假象。另外 8 项断言 `userland/runtime/sdk/rootfs/apk-repo/image-vmdk/iso/installer`
仍然以退出码 2 明确拒绝——这是"旧 Python 链没有被偷偷留着"的另一半证据。

**未覆盖**：SDK 与镜像链本身（尚未迁移），所以 A16 记为**已迁移范围内通过**。

## 8. 发现并已修的计划/环境问题

`.gitignore` 第 1 行的 `build/` 会匹配**任意层级**名为 `build` 的目录，导致计划第 5、12、15 节要求的
`docs/build/migration-inventory.md`、`docs/build/legacy-removal.md`、`docs/build/verification.md`
和 `tests/build/` 契约测试目录**全部无法提交**；本轮新建的 `tools/build/` 是第三次踩坑。
已把首行改为锚定的 `/build/` 并移除两条临时反选，验证 `docs/build`、`tests/build`、`tools/build`
不再被忽略，而 `build/linux-6.12/tools/build` 仍随 `/build/` 一起被忽略（`git status` 无新增噪声）。

## 9. 已知限制

- `userland/runtime/sdk/rootfs/apk-repo/三类镜像/run` 仍是 `exit 2` 的未迁移目标；
  `all` 只做到 `kernel` 后明确报错，不冒充完整产物。
- 认证链只走了第一段（第 11 节）：`linux-headers` 与 `libxcrypt` 已由新链构建并验证，
  **Linux-PAM 仍未移植**，因此 `libleonos.so.2` 与 `runtime` 现在**不可能**被诚实构建。
  `make leonos-auth` 是内部阶段目标，不是产品目标。
- auth/sysroot 这类上游包的构建**每次调用都会重跑 configure 之外的部分**（auth 已在输入键
  未变时整体早退，musl 尚未这样做），把 `$(O)/logs/*.log` 写满；不影响正确性，但影响耗时。
- 来宾启动证据仍缺（见 5.1）；`test-smoke` 未交付。
- 布局新增一个计划未列出的目录 `$(O)/third-party/`（上游 configure 的 out-of-tree 构建目录），
  `make clean` 的显式清单已包含它。这是对计划第 5 节目录表的**扩展**，需审阅确认。
- `docs/build/research/` 三份底稿是未复核的调研稿，其中两条论断已确认有误（见
  `migration-inventory.md` 第 5 节），不得当验收证据引用。
- 本轮改动未推送、未合并、未发布镜像。

## 10. 性能记录与产物事实（计划第 15 节要求）

测量方法与第 2 节一致（同一宿主、同一配置）。命令为 `make -s O=<全新临时目录> -j8 kernel`，
用 GNU time 的 `-v` 取墙钟与最大驻留；`incremental` 是 `touch kernel/ntclks/futex.c` 之后同命令，
`noop` 是紧接着再跑一次同命令。

| 用例 | 墙钟 | 峰值 RSS | 说明 |
| --- | --- | --- | --- |
| clean kernel（85 个对象 + 链接 + 两个 objcopy，含 host 工具与 kconfig 前端） | 16.1 s | 128 MB | 全新 O，一次性 |
| incremental（1 个对象 + 链接 + 镜像） | 0.24 s | 104 MB | |
| no-op | 0.11 s | 46 MB | 与 A02 的"零动作"一致 |
| `make test`（C 单测双份 + 6 个 shell 套件，含内核全量构建） | 2:05 | — | `make -j12 test` |
| `make test-long`（A16 六条 execve 跟踪 + A08 两轮树 + A10 中断） | 约 6 分 | — | `make test-long` |

**不做倍数对比**：第 2 节的旧系统 `kernel-1 = 1.52 s` 是热目录上重复同一条命令的采集，不是干净
构建，旧系统**没有**干净内核构建的同法基线；按计划第 15 节"无可比基线就只报告观察"。
`installer` 也没有可比数据（旧值 266.7 s，新链未迁移）。

产物事实（截至本提交，新链**实际产出并被验证**的东西）：

| 产物 | 路径 | SHA-256（前 12） | 是否启动过 |
| --- | --- | --- | --- |
| 内核镜像（固定 epoch 1700000000 / BUILD_ID 7） | `<O>/generated/system/kernel.sys` | `4afa332cc79e` | **否**（见 5.1：受阻，且无回归证据也无成功启动证据） |
| 未剥离内核 | `<O>/generated/system/kernel.unstripped` | `45ce7e9b2c02` | — |
| 调试内核 | `<O>/generated/system/kernel.debug` | `c61de02c4d2f` | — |
| musl sysroot | `<O>/sysroot/musl/`（14 个声明产物） | 见 6.3 的逐文件比较表 | — |

**新链没有产出过任何 SDK、APK 仓库或镜像**，因此本审查包不附带任何镜像哈希，也没有"旧镜像
冒充新结果"的风险：交付物只有源码、Make 片段、C 工具、脚本与测试。

## 11. P2-d：认证链第一段（Linux UAPI 头 + libxcrypt）

`tools/build/auth-upstream.sh` + `mk/third-party.mk` 的 auth 段，`make leonos-auth`，
契约套件 `tests/build/test-auth-stage.sh`（23 项，0 失败）。

### 11.1 为什么只做这两个包

Linux-PAM 1.7.2 上游**只有 Meson**（autotools 在 1.6.0 被移除），而裁定是不降级、不开
Meson/Ninja 例外、不删认证，所以 libpam 需要手写 Makefile 移植，**本段不包含它**，
并且用一条断言把它"不在"这件事钉住：`Linux-PAM is honestly absent`（stage 里既没有
`lib/libpam.so.0` 也没有 `usr/include/security/pam_appl.h`）。仍带 POSIX `configure` 的包
按计划第 9 节允许由上游自己的 configure/Makefile 构建，因此这里做的是 `linux-headers`
（`make headers_install`）与 `libxcrypt`。

configure argv 与交叉编译环境逐条抄自退役的 `tools/build_auth_upstream.py:89-98,166-195`
（`--host/--prefix/--sysconfdir/--localstatedir/--libdir=/lib/--enable-hashes=all/
--enable-obsolete-api=no`，`LDFLAGS=-Wl,-z,relro,-z,now ...`，`PKG_CONFIG_*`），差异只有一处，
见 11.3。

### 11.2 与旧系统的一致性

| 比较 | 结果 |
| --- | --- |
| `usr/include/crypt.h` | 与 `build/auth-upstream/root/usr/include/crypt.h` **逐字节相同**（11195 字节，`cmp` 通过） |
| `config.h` 的 `#define` 集合 | 两侧各 304 条，`diff` **无差异**：所有探测结果一致 |
| 动态符号表 / SONAME | `SONAME=libcrypt.so.2`，导出 `crypt`、`crypt_r`、`crypt_gensalt`、`crypt_gensalt_rn`、`crypt_checksalt`，`NEEDED` 只有 `libc.so`（musl 的 SONAME；出现 `libgcc_s.so.1` 会被判为链到了宿主） |
| 库二进制 | 见 11.3：差异的**唯一来源**是构建目录路径字符串 |

### 11.3 一处有意的偏离：`-fmacro-prefix-map`
比较新旧 `libcrypt.so.2.0.0` 时先得到"`.text` 大小相同但 206 字节不同、文件差 240 字节"。
根因是 libxcrypt 的断言把 `__FILE__` 编进 `.rodata`：旧树里嵌的是
`/home/xiaobai/.../build/auth-upstream/src/libxcrypt-4.5.2/lib/*.c`，新树是
`/tmp/auth1/third-party/auth/src/libxcrypt-4.5.2/lib/*.c`。`.rodata` 短了 240 字节，
`.eh_frame_hdr` 及其后**所有段地址整体平移 0xF0**，于是 `.text` 里所有与地址相关的立即数
都变了——代码本身相同，差异全部来自编译路径。

这与旧系统行为一致，但对新链是不可接受的：产物字节会随 `O=` 路径变化，直接违反计划第 6.4
节对可重现的要求。因此 auth 适配器额外传 `-fmacro-prefix-map=<srcdir>=libxcrypt`，并用断言
钉住结果：**两个不同深度的输出树构建出的 `libcrypt.so.2.0.0` 与 `libcrypt.a` 逐字节相同**。

### 11.4 顺带修掉的两个真实缺陷

1. **`O=` 泄漏进内核构建。** 内核 Makefile 自己把 `O=` 解释为 out-of-tree 构建目录，而顶层
   `make O=/path` 通过 `MAKEFLAGS` 把它传给了子 make，于是 `headers_install` 把整套
   `arch/`、`scripts/` 内核构建树写进了本项目的输出目录。现在显式传
   `O=$work/build/linux-headers`，并新增断言"`$(O)` 顶层只允许布局里声明过的目录"——
   这条断言属于计划第 6.3/12 节的输出目录安全范围（A12）。
2. **`make clean` 认不出只做构建目标产生的树。** 所有权标记原先只有 `defconfig` 一类目标才写，
   因此 `make kernel`/`make leonos-auth` 造出来的树会被 clean 以"不是我创建的"拒绝，
   而 `$(O)/auth` 也不在 clean 的显式清单里。现在标记随每次非被动调用写入，`auth` 已加入
   清理清单，并由 `make clean removes the staged tree and keeps the directory` 断言覆盖。

### 11.5 本轮的一次自身失误与恢复（如实记录）

做 11.3 的比较时，我用了 `llvm-objcopy -j .text --dump-section .text=FILE <输入>` 且**没有给
输出文件参数**：`llvm-objcopy` 就地重写输入，于是同时毁掉了新树和旧基线的
`build/auth-upstream/root/lib/libcrypt.so.2.0.0`（`readelf -d` 报 `no .dynamic section`）。
因此那一次比较得到的"段大小相同 / dynsym 相同"结论是在已损坏文件上算的，**作废**。
恢复方式：用旧系统自己的 action 重新生成（`python3 build.py run auth-upstream`，rc=0，
`0 errors`，文件回到受损前的 227216 字节且 `readelf -d` 正常）。该次重建把两个**受跟踪**文件
按旧系统的习惯向前跳了一个构建号（`buildsystem/state/build_number.txt` 3833→3834、
`include/generated/build_info.h` 的 `LEONOS_BUILD_NUMBER`/`LEONOS_BUILD_TIME`），
我已把这两个文件恢复到来时状态，`git status` 现在只剩本阶段的改动。这也从反面说明新链为什么
坚持不写受跟踪文件（A17）。正确且只读的比较方式是
`objcopy -O binary --only-section=.text <输入> <输出>`（显式输出文件），本文的结论以重做后的
数据为准。
