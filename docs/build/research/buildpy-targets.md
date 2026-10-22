# LeonOS 4 旧构建系统：生产目标全量清单（P0-a 事实底稿）

> 历史研究，保留当时路径和行号。当前构建采用 Kbuild；旧预编译 EFI 树和宿主模块回退已移除，
> 现行双平台引导契约见 [2026-10-22 双启动记录](../dual-boot-2026-10-22.md)。

> 供主 Agent 撰写 `docs/build/migration-inventory.md` 使用。
> 全部行号基于 2026-09-19 工作区 `build.py`（4958 行）与 `buildsystem/`。
> 本文件位于 gitignore 的 `buildsystem/logs/` 下，不是交付物本身。
> 未运行任何 `python3 build.py ...`；所有统计来自静态阅读 + 只读文件系统 glob。

---

## 0. 对任务描述中已知前提的事实校正

| 前提 | 实测 | 证据 |
| --- | --- | --- |
| `build.py` 约 46 处静态 `name="..."` | **144 处**字面量 target 名（`grep -on 'name="[A-Za-z0-9:_.-]*"'`，剔除 1495 行 `soname="libleonos.so.2"` 与 3798 行 `"test-" + suffix` 拼接） | build.py 全文 |
| `buildsystem/` 43 个 Python 模块 / 7150 行 | **8 个真实模块 / 3405 行**。其余 35 个 `.py`（3745 行）全部在 `buildsystem/cache/apk/distributions/*/root/usr/share/vim/vim91/tools/demoserver.py`，是 APK 解包缓存里的 vim 样例，不是构建代码 | `find buildsystem -name '*.py' -not -path 'buildsystem/cache/*'` |
| `build.py help` 的 21 个顶层任务 | 其中 **`defconfig` 不是图节点**：`main()` 在构图前拦截 `run defconfig`（build.py:4839-4846），直接 `shutil.copy2(configs/default.conf, buildsystem/config/leonos.conf)` + `sync_config_file()`。其余 20 个是图节点。另有 **大量未列进 help 的生产目标**：`musl` / `auth-upstream` / `storage-upstream` / `apk-root` / `installer-root` / `installer-image` / `esp` / `kernel-link` / `release` / `images-iso` / `musl-*` / `rpr-apps` / `kconfig-mconf` 等 | build.py:4219-4246, 4839 |
| 真实图节点总数 | `graph.add(Target(...))` 共 **157 个源码调用点**，其中约 60 处在循环里按组件/源文件/expansion 生成；当前 checkout 下 `all` 闭包展开约 **900–1000 个可执行节点**（详见 §6 动态规则表实测计数） | `grep -c 'graph\.add(' build.py` = 157 |
| 需要保留"执行锁"（规格 §2） | **构建系统没有任何进程间执行锁**。`buildsystem/core/*.py` 与 `build.py` 中无 `flock`/`LOCK_EX`。仓库里唯一叫"execution lock"的是内核特性（`kernel/ntclks/lock.c`，由 `tools/test_execution_lock.py` 用 `cc -fsanitize=address,undefined` 在 tmp 里编译主机单测验证），与构建调度无关。规格 §6.3 要求的"同 O 目录双顶层构建互斥"在旧系统里 **本来就不存在** | `grep -rn 'flock\|LOCK_EX' buildsystem/core build.py` → 0 命中 |
| 仅有 2 处 flock | `tools/make_image.py:68-80`（`build/images/.leonos4.raw.lock`，`LOCK_EX\|LOCK_NB`，保护 raw 中间文件）与 `tools/apk_distribution.py:125-126`（`buildsystem/deps/apk-tools/.lock`，保护 apk.static bootstrap 下载） | 同左 |

---

## 1. `paths.out` 与旧目录布局（`buildsystem/core/state.py:47-137`）

`BuildPaths(root=ROOT)`，ROOT = `Path(build.py).resolve().parent`。**没有 arch/profile 维度**，全仓库只有一棵输出树。

| 属性 | 解析结果 | 说明 |
| --- | --- | --- |
| `paths.out` | `<ROOT>/build` | **所有 build.py 中 `paths.out / ...` 的展开基址**；spec §5 的新世界对应 `out/<arch>/<profile>/` |
| `paths.legacy_out` | `buildsystem/out` | 仅 `clean` 删除 |
| `paths.objects` | `build/obj` | `object_path()` = `build/obj/<prefix>/<源相对路径>.o`（build.py:402-405）；prefix 用作 ABI 隔离（`kernel`/`loader`/`musl-runtime`/`user-<app>`/...） |
| `paths.generated_include` | `build/include/generated` | `autoconf.h`、`autoconf-installer.h`、`rustcfg.args`、`loader_integrity.h`、`leonos_gbk_table.h`、`boot_logo.h` |
| `paths.staging` | `build/esp` | rootfs 边界目录（guest 相对路径镜像） |
| `paths.images` | `build/images` | `leonos4.vmdk`、`leonos4.iso`、`leonos4-installer.iso` |
| `paths.build_modules` | `boot/grub_modules_x86_64-efi` | **源码树内**；缺失时回落到 `/usr/lib/grub/x86_64-efi`（build.py:937-942） |
| `paths.state` | `buildsystem/state` | `targets.json`（全部 target 状态，单文件）、`targets/`（旧版逐文件，仅迁移读）、`tasks/<id>.json`、`build_number.txt`(**受版本控制**) |
| `paths.tasks` | `buildsystem/state/tasks` | 每次 CLI 调用一条记录 |
| `paths.logs` | `buildsystem/logs` | `<9 位随机任务 ID>.log` |
| `paths.tmp` | `buildsystem/tmp` | `config-<task_id>.conf`（`--profile/--set` 合并结果）、各上游工作目录 |
| `paths.config` | `buildsystem/config` | `leonos.conf`（活动配置，gitignore）、`settings.toml`（调度参数） |
| `paths.deps` | `buildsystem/deps` | 上游下载缓存：`auth/`、`apk-tools/`、`musl-gcc/`、`python/`、`storage/`、`fastfetch/`、`openrc-apks/` |

其他硬编码输出位置（不走 BuildPaths）：
- `include/generated/build_info.h` —— **受版本控制的源码树路径**（build.py:763），由 `build-info` 每次重写。
- `LeonOS4-Developer-SDK.zip` = `ROOT / "LeonOS4-Developer-SDK.zip"`（build.py:936）—— **源码根目录**，gitignore 已豁免（`.gitignore` 末段）。
- `Kconfig.components` —— `tools/generate_component_kconfig.py` 默认 `--output Kconfig.components`，**受版本控制的源码树路径**，由 `config-sync` 每次重生成。
- `buildsystem/cache/apk/{downloads,distributions}` —— 真正的**内容寻址缓存**（键 = SHA-256），见 §7。

`relative()`（build.py:302-310）：绝对路径若在 ROOT 下则转 ROOT 相对，否则保留绝对。**所有 target command 以 cwd=ROOT 执行**（`Target.cwd` 在本仓库从未被赋值，`grep 'cwd=' build.py` 只命中 subprocess 调用），因此命令里大量出现 ROOT 相对路径。

---

## 2. 机制层：`buildsystem/core/`

### 2.1 数据模型（`model.py`，187 行）

`Target` slots（model.py:15-37）：`name, outputs, inputs, implicit_inputs, depends_on, kind, description, command, action, action_key, depfile, cwd, group, always, source, environment`。`all_inputs() = inputs + implicit_inputs`。

`BuildGraph`（model.py:44-187）关键行为：

| 机制 | 位置 | 行为 | Make 下的等价物 |
| --- | --- | --- | --- |
| 路径归一 | `path()` 54-58 | 相对→ROOT 绝对 + `os.path.normpath`（不解析符号链接） | Make 的字符串路径（无归一，需小心 `./`） |
| 重名拒绝 | `add()` 68-69 | `duplicate target name` | Make 同名规则会静默合并/告警 → 需显式检查 |
| **重复输出拒绝** | `add()` 82-86 | 同一输出被两个 target 声明 → `GraphError: duplicate output` | Make 原生只告警"overriding recipes"，**必须自建校验**（A12） |
| 依赖边自动推导 | `dependencies()` 93-108 | 先按 `all_inputs` 反查 `_outputs` 得出生产者，再追加显式 `depends_on` | Make 原生（前置条件即文件路径） |
| 环检测 | `closure()` 159-178 递归 DFS，`visiting` 集 | `dependency cycle at <name>` | Make 原生报错 |
| 反向可达 | `dependents()` 122-135 | 供 `build.py affected` | 需 `make -p`/自建脚本 |
| 稳定查询 | `resolve_target()` 137-157 | 按名字 / 输出路径 / **源文件路径**（源文件映射多个 target 时报错） | Make 只认目标名/文件 → `gen <file>` 语义需自建 |
| 图导出 | `map_lines()` 180-187 | 供 `build.py map`（curses 分页，ui.py:96-124） | `make -p` / `--trace` |

### 2.2 调度与并行（`runner.py:557-773`）

- 闭包：`graph.closure(roots)`（roots 由 `build_roots()` 决定，见 §2.6）。
- 就绪集：`ready = [无依赖目标]`，**LIFO**（`ready.pop()`，runner.py:743）→ 完成顺序不稳定，A08（-j1 vs -j8 可比性）基线需注意。
- 线程池：`ThreadPoolExecutor(max_workers=settings.worker_threads)`（runner.py:739）。
- 进程上限：`threading.BoundedSemaphore(max_processes)`（runner.py:585），只包裹 `run_process`，**不包裹 Python 侧 action 的文件 IO**。
- worker 槽位：`queue.Queue` 里放 `worker_threads` 个整数，仅用于日志 `<N>` 前缀。
- 失败处理：第一个 future 抛异常 → 记录 `failed` → `pending.cancel()` 所有在跑 future → `break` → 重新抛出（runner.py:750-771）。**已启动的子进程不被杀**（无进程组管理）。
- 完成后校验 `metrics.completed != len(closure)` → `BuildFailure("build graph did not complete")`。
- mtime 缓存：`self._mtime_cache` + `_mtime_lock`（runner.py:592-593, 943-966），单次构建内同一路径只 lstat 一次；`_refresh_mtimes` 在执行后刷新。
- 进度 UI：`ProgressRenderer`（runner.py:102-175），仅 TTY 且 `theme=="default"` 时启用；`--theme linux|meson|cargo` 伪装 make/ninja/cargo 输出（runner.py:226-424）。**规格 §3 明确不重新实现**。
- 配置来源：`buildsystem/config/settings.toml` → `load_settings`（ui.py:14-30），缺文件时**自动写入** `worker_threads = max_processes = os.cpu_count()`、`download_retries = 3`。`build.py settings` 提供 curses 编辑器。

### 2.3 缓存与增量判定（`runner.py:918-1030`）

**不是内容寻址。** 三层：

1. **声明指纹** `_fingerprint()`（runner.py:918-932）
   `sha256(json.dumps({name, kind, outputs[], inputs(all_inputs)[], depends_on[], command[], action: action_key, depfile, environment}, sort_keys=True))`，路径先经 `root_relative()`。
   → 只哈希**声明字符串**，不哈希任何文件内容。改变 `action_key` 字面量、增删输入、改命令 argv、改 environment 都会让该 target 及其下游（靠 mtime 链）重建。
2. **时间戳比较** `rebuild_reasons()`（runner.py:968-997）
   ```
   always                              → 重建
   无 outputs 且有 action/command       → 重建（"target has no declared outputs"）
   无 outputs 且无 action/command（group）→ 永不重建
   任一 output 缺失                     → 重建
   指纹变化                             → 重建
   oldest_output = min(lstat mtime_ns(outputs))
   对 inputs + implicit_inputs + depfile_dependencies 逐项：
       缺失            → 重建
       mtime > oldest_output → 重建
   ```
   `artifact_mtime()`（runner.py:447-459）用 **lstat**，若是符号链接再取 `max(lstat, stat)`（跟随目标，允许 dangling）。
3. **depfile**：仅 `add_compile` 生成的 `.o.d`。`parse_depfile()`（runner.py:469-503）自己实现 Make 风格解析（`\`-续行、`:`-分隔、反斜杠转义、相对路径归 ROOT），结果存入状态 `depfile_dependencies`，下次参与 mtime 比较。

状态持久化：`TaskStore.write_target` 只改内存 dict，`flush_target_states()` 在 `close()` / 成功 / 异常时把**整个** `buildsystem/state/targets.json`（`{"version":1,"targets":{...}}`）用 `atomic_json`（tempfile + `os.replace`，state.py:26-34）一次性替换。当前该文件已 2.6 MB / **1568 条**，其中含大量已删除的旧 target（`compile:libc`、`compile:static-libc`、`compile:ld-leonos`、`picolibc`、`tcc`、`python`、`musl-gcc`、`esp:api:*`、`gcc-probe-*`、`installer-policy:oobe`），只有显式 `build.py cache prune`（`prune_target_states`，state.py:240-268，按输出是否存在判过期）才清理。

> 关键缺陷（迁移必修复位）：mtime 判定对"内容变了但 mtime 变旧/相同"（`touch -d`、rsync、git checkout）不敏感；符号链接目标改变但 lstat 未变时 `esp:layout-links` 一类可能漏重建；`always=True` 目标每次都跑，其"输出"即使内容不变也会让依赖它的 mtime 链不动（因为 `ensure_parent` 只在内容变化时写）——这层"内容稳定写"完全散落在 action 里。

### 2.4 配置解析（Kconfig → 构建选择）

| 步骤 | 位置 | 行为 |
| --- | --- | --- |
| 读取配置 | `parse_config_values` build.py:425-439 | 支持 `CONFIG_X=v` 与 `# CONFIG_X is not set` → `"n"`；其他行忽略 |
| 合并默认 | `parse_kconfig` build.py:442-461 | 基线 `configs/default.conf` → 覆盖活动配置 → **未知符号即 `BuildFailure`**（白名单 = default.conf 键 ∪ `component_config_symbols()` ∪ 10 个 `retired` 符号）→ 若 `CONFIG_BUILD_USE_ADVANCED_OVERRIDES != y`，按 `CONFIG_BUILD_PRESET_{DEBUG,DEVELOP,RELEASE}` 强制覆盖 5 个编译策略符号（`BUILD_PRESET_VALUES`，build.py:112-134） |
| 互斥组 | `CONFIG_CHOICE_GROUPS` build.py:103-111 | 5 组；`--set K=y` 时把同组其余置 `n`（build.py:4286-4291） |
| 有效配置选择 | `resolve_build_config` build.py:4296-4325 | `--profile NAME` → `configs/profiles/NAME.conf`；无活动配置时**自动 copy2 default.conf 到 buildsystem/config/leonos.conf**（副作用！）；有 `--set` 或 profile 时写 `buildsystem/tmp/config-<task_id>.conf`；`run menuconfig` 且无 `--set` 时直接用源文件（interactive） |
| 生成物 | `config-sync` action build.py:947-961 | 先 `python3 tools/generate_component_kconfig.py`（写 **`Kconfig.components`，源码树受跟踪文件**），再 `python3 tools/kconfig_sync.py --config <cfg> --defaults configs/default.conf --out-dir build/include/generated --selection-out build/generated/component-selection.json`。`kconfig_sync.py:126-133` 有 `write_if_changed`（内容不变不 touch）；它还会 `write_config` **回写用户的 config 文件本身**（kconfig_sync.py:235） |
| 编译策略注入 | build.py:1050-1063 | `-O{0..3}`、`-g`、`-flto=thin`、链接侧 `--lto-O*`、`--strip-all`，转成 `--compile-flag=` / `--linker-flag=` 传给 `tools/build_*.py` |
| CLI 侧配置命令 | build.py:4361-4406 | `config list/save/load/reset/import/export`，均经 `normalize_config_copy` → 临时文件 + `sync_config_file` |

### 2.5 组件清单解析（`buildsystem/components.py`，361 行）

- `load_components(configs/components.toml)`（81-199）：`tomllib`，强制 `version == 1`；字段校验（`ID_RE`/`SYMBOL_RE`/`STAGE_PATH_RE`，`kind ∈ {system-app, program-app, package-app, tool, library}`，`default/required/stage/entry/sdk/api` 必须 bool，`extensions` 必须 `.name` 形式）；`depends` / `api_requires` 引用存在性 + 两次独立 DFS 环检测；重复 id/symbol 拒绝。**当前 73 个组件**：system-app 24、program-app 35、package-app 3、tool 7、library 4。
- `resolve_components(components, values)`（218-290）：`build = required or CONFIG_..._BUILD`，`depends` 递归强制置真，`api_requires` 会**反向把依赖也置真**；导出 `{build, image, entry, sdk, api}` 五个布尔。`required` 组件忽略用户符号、直接用 manifest 默认（266-275 注释）。
- `validate_component_targets(components, ROOT)`（316-361）：app 类必须在 `userland/apps/<id>` 或 `userland/<id>`（nano 额外允许 `third_party/nano/src`，fastfetch 走 5 个固定 package 输入）有 `.c/.S/.cpp`；tool 类要求 `userland/<id>` 存在；library 类要求 `third_party/<id>` 存在。**构图阶段的前置断言**。
- 消费点：`build_graph` 里 `component_enabled(id, option)`（build.py:735-737）驱动约 40 处 `if` 分支，决定 target 是否存在、`esp_names` 内容、`user_targets` 聚合成员、SDK 参数列表。
- 唯一"权威面"（对齐规格 §7）：`configs/components.toml` = 组件元数据；`Kconfig` + `Kconfig.components` = 功能选择；`configs/default.conf` = 编译/运行默认值。`build.py` 里**没有**第三份 defaults 副本（除 `BUILD_PRESET_VALUES` 与 `CONFIG_CHOICE_GROUPS`）。

### 2.6 build-info 与 build number（**污染每次构建**）

- `build_roots()`（build.py:4213-4216）：除 `BUILD_NUMBER_EXEMPT_TARGETS`（build.py:135-197，**61 个**名字，几乎全是 `test-*`）和 `build-info` 本身以外，**所有** CLI 运行都把 `build-info` 当作额外根节点。
- `build-info` 是 `always=True`（build.py:1074），执行 `python3 tools/build_info.py --header include/generated/build_info.h --state buildsystem/state/build_number.txt`；`build_info.py:36` 无条件 `build_number = read + 1`，`:37` `datetime.now()` 写入 `LEONOS_BUILD_TIME` / `LEONOS_COPYRIGHT_YEAR`，**无 SOURCE_DATE_EPOCH 支持**，且**总是重写**（无 write-if-changed）。
- 后果：`include/generated/build_info.h` 每次 mtime 变化 → 所有把它列为 input/implicit input 的 target 全部重建（`kernel/ntclks/version.c` 的编译显式带 `build_info`，见 build.py:1230）→ 内核重链 → `loader-integrity` → `loader-image` → 所有镜像。
- 下游读 header 内容：`tools/build_rpr_pages.py:19-20, 42-55` 用正则解析 `LEONOS_KERNEL_VERSION` / `LEONOS_BUILD_NUMBER` 并断言二者一致；`tools/build_rpr_apps.py:16` 同理。

### 2.7 任务记录、日志与后台 client

- `TaskStore.new_id()`（state.py:152-175）：`secrets.randbelow(900_000_000)+100_000_000`，`open("x")` 独占创建，最多重试 256 次 → **九位随机任务 ID**（规格 §3 明确不再这么做）。
- 记录：`buildsystem/state/tasks/<id>.json`，字段 `id/command/status/created_at/log`，运行中被 `update()` 追加 `queued_at/started_at/finished_at/task/total_targets/completed_targets/running_targets/progress_percent/elapsed_seconds/metrics/error/profile`。每次 `update` = 读 JSON → 合并 → `atomic_json` **整文件重写**（runner.py:775-786 `_publish_progress` 在**每次 dispatch 和每次完成**都调用 → O(N) 次全量写）。
- 日志：`buildsystem/logs/<id>.log`，`TaskLogger` 构造时 `open("w")` 截断（runner.py:198），每行 `strip_ansi` 后写入并 flush，带线程锁。
- `build.py status <id>` / `build.py log <id>`：`log` 直接 `subprocess.run(["vim","-R",<path>])`（build.py:4822）→ **隐式依赖 vim**。
- 后台执行 `build.py client run <task>`：`run_client`（build.py:4613-4667）写一条 `queued` 记录 → `subprocess.Popen([PYTHON,"build.py","--worker","--task-id",id,...], stdin=DEVNULL, stdout=stderr=/dev/null, start_new_session=True)` 立即返回，无守护进程、无 wait、无并发限制。**这是规格 §3 "不做后台 daemon" 的对应物。**

### 2.8 前置检查与宿主工具表

- `require_linux()`（4130）：`platform.system() != "Linux"` 即失败；另外模块导入时 `os.name == "nt"` 直接 `SystemExit`（build.py:95-98）。
- `require_tools(task_tools(task))`（4135-4198）：**手工 if 链**，按 target 名返回工具元组，未命中返回 `()`。已登记：`clang, ld.lld, llvm-ar, autoreconf, autoconf, automake, libtoolize, make, gcc, g++, gperf, flex, bison, rustc, grub-mkfont, grub-mkstandalone, truncate, mkfs.fat, mcopy, mke2fs, dd, qemu-img, grub-mkrescue, xorriso, qemu-system-x86_64`。
  **表里缺失但实际会被调用**：`curl`、`git`、`openssl`、`patch`、`fakeroot`、`unshare`、`e2fsck`、`readelf`、`llvm-ranlib`、`grub-mkimage`、`python3` 本身、`llvm-objcopy`、`apk.static`。→ 迁移到 `make doctor` 时必须补全（规格 §4）。
- `require_grub_efi_modules()`（4142-4153）：只对 10 个 target 名生效，检查 `buildsystem/deps/grub-efi-amd64-bin/...` 或 `/usr/lib/grub/x86_64-efi/modinfo.sh`。
- 构图阶段的源码存在性断言：build.py:1005-1046（busybox / nano / fastfetch / sl / less / lua / cmd / pleditor / zlib / libpng / file / sqlite / portablegl 各缺文件即 `GraphError`）。这些是**子模块初始化检查**，等价 `make doctor` 的一部分。

---

## 3. 生产目标全量清单（顶层：`build.py help` 列出的 21 个）

列宽有限，`输入/输出` 只写代表性路径；`<...>` 表示变量展开。`always` 列：`A`=Target.always=True（无条件跑），`C`=kind=command/test 且无 outputs（`rebuild_reasons` 必然返回原因，等效每次跑），`G`=group/aggregate（永不执行动作），空=mtime+指纹判定。

| # | 目标 | 行号 | kind | depends_on（显式） | always | inputs（声明） | outputs（声明） | 实际执行 | 下游消费者 | 网络/root/mount/块设备 | 迁移难点 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | `all` | 3544 | aggregate(group) | config-sync, build-info, loader, kernel, kerneldebug-module, drivers, middlelayer, userland, sdk, esp | G | — | — | 无 | `installer`；`.vscode/tasks.json`；`run` 系列间接 | 间接触发全部 | 聚合面与新 `make all`（spec §4 要求含 apk-repo/三个镜像）**不一致**：旧 `all` 不含 image-vmdk/image-iso/sdk 之外的镜像 |
| 2 | `config-sync` | 965 | generate | — | **A** | configs/default.conf, configs/components.toml, Kconfig, Kconfig.components, tools/generate_component_kconfig.py, tools/kconfig_sync.py, buildsystem/components.py | buildsystem/config/leonos.conf, build/include/generated/autoconf.h, autoconf-installer.h, rustcfg.args, build/generated/component-selection.json | `python3 tools/generate_component_kconfig.py` + `python3 tools/kconfig_sync.py ...` | 几乎全部编译 target（autoconf.h 是 implicit input）、staging-prune、esp:config | 无 | **5 输出 + 写源码树 `Kconfig.components`**；`always=True` 且需内容稳定；Make 的 generated-include 重启风险（spec §7） |
| 3 | `build-info` | 1066 | generate | — | **A** | tools/build_info.py | **include/generated/build_info.h（受跟踪）**, buildsystem/state/build_number.txt（受跟踪） | `python3 tools/build_info.py --header ... --state ...` | kernel/ntclks/version.c 编译、`rpr-apps`、`rpr-pages`、`esp:config`（间接） | 无 | 版本号自增 + `datetime.now()`；无 epoch 支持；写在源码树；spec §7 要求移到 O/generated |
| 4 | `loader` | 1380 | aggregate | loader-image | G | — | — | 无 | `esp:loader.elf`、`image-iso`、`installer-image`、`musl-*iso` | 无 | 真实产物在 `loader-image` |
| 5 | `kernel` | 1277 | aggregate | kernel-image, kernel-debug | G | — | — | 无 | `loader-integrity`、`esp:leonos/kernel.sys`、所有 ISO 目标 | 无 | — |
| 6 | `drivers` | 1399 | aggregate | driver:{mouse,serial,e1000,ac97,es1371} | G | — | — | 无 | `esp:driver:*` | 无 | 5 个模块由 `DRIVER_MODULES`（build.py:100）常量列表驱动 |
| 7 | `middlelayer` | 1341 | aggregate | middlelayer-image | G | — | — | 无 | `loader-integrity`、`esp:leonos/middlelayer.sys`、所有 ISO | 无 | 依赖 `rustc --target x86_64-unknown-none`（spec §2 要求保留 Rust 组件并登记依赖） |
| 8 | `userland` | 2355 | aggregate | `user_targets`（约 30–45 项，随组件开关变化） | G | — | — | 无 | `app-manifests`（depends_on）→ `esp:app:*` | 视组件（busybox 需 make，file-magic 需 autoreconf） | `user_targets` 列表在 build.py:2146-2174/2225/2354 边构造边 append，成员集**依赖组件选择**；`app-manifests` 反向 `depends_on=("userland",)` 形成"聚合再依赖具体"的交错 |
| 9 | `sdk` | 2507 | generate | musl, auth-upstream, archive:libc, archive:zlib, archive:libpng (+按需 ncurses/file/sqlite/portablegl/archive:stardustui/lua/app:stardust*/config-sync) | — | build.py:2358-2504 长列表（含 `*collect("include/uapi/**/*.h")`、`*collect("devtools/**/*")`、各 .a/.so/stamp/LICENSE） | **LeonOS4-Developer-SDK.zip（源码根目录）** | `python3 tools/package_devtools.py ...`（40+ 个参数，随组件 `if` 拼装） | 外部开发者；`release` | `musl-sdk` 无网；`package_devtools.py:407` 把 **`tools/leonos_musl_cc.py` 原样塞进 SDK 的 `bin/leonos-musl-cc`** → **生产 SDK 使用 Python**（spec §2 禁止面直接命中） | 单输出但内部 stage 到 `devtools/`（源码树！）并 `rmtree`；命令 argv 由 6 段 `if` 拼出；`--component-file/--component-tree` 变长参数 |
| 10 | `esp` | 3276 | aggregate | `esp_names`（约 **170+** 个 staging 生产者） | G | — | — | 无 | `apk-root`（inputs 用 `*esp_outputs`）、`musl-desktop-vim-root`、`image-vmdk`（经 apk-root） | 无 | `esp_names`/`esp_outputs` 两个 list 在 2660-3265 之间**手工同步维护**，且 build.py:3270-3275 事后遍历全图给"任何输出落在 staging 的 target"补 `depends_on += ("esp:rootfs",)` —— **构造后突变依赖**，Make 无法直译 |
| 11 | `rpr-pages` | 3337 | generate | apk-root, rpr-apps, kernel, middlelayer, build-info | **A** | build/generated/normal/manifest.json, build/apk/root/usr/share/leonos/apk/repository/packages.adb, 3 个 .apk, kernel.sys, middlelayer.sys, build_info.h, tools/build_rpr_pages.py, tools/apk_distribution.py | build/rpr-pages/.complete, build/rpr-pages/manifest.json | `python3 tools/build_rpr_pages.py --repository ... --repository ... --kernel ... --middlelayer ... --build-info ... --output ...` | `.github/workflows/publish-rpr.yml:69` 上传；来宾 `leonos-check-update` | **网络**（`bootstrap()` 下载 apk-tools-static 若缓存缺失）；**openssl 签名**（需 `LEONOS_APK_SIGNING_KEY`，mode 0600 强制）；`always=True` | 版本从 build_info.h 正则解析；输出目录被 `rmtree`；`.complete` 单 stamp 掩盖真实多输出（spec §6.3 禁止） |
| 12 | `image-vmdk` | 3366 | generate | apk-root | — | `*esp_outputs`（170+）、apk manifest、config、tools/make_image.py, tools/make_ext2_root.py, layout.ROOTFS_CONTRACT, tools/leonos_layout.py, tools/image_test_accounts.py + system/test-accounts/* | **4 个**：build/images/leonos4.vmdk, leonos4.raw, esp.fat, root.ext2 | `python3 tools/make_image.py --out ... --raw ... --esp-tree build/apk/root --esp-image ... --root-image ... --root-fs ext2 --default-language {en\|zh} --size-mib <CONFIG_IMAGE_SIZE_MIB>` | `run`, `run-debug`, `run-iso`, `release`；全部 `test-qmp-*`（作为 inputs 且被 qemu-img backing） | 无 root/无 mount/无块设备。用 `truncate`, `mkfs.fat`, `mcopy -i <file>`, `mke2fs -d`(经 fakeroot), `e2fsck -f -n`, `dd`, `qemu-img convert -O vmdk` | **多输出规则** → Make grouped target `&:`（spec §6.3）；`make_image.py` 内部 `.lock` + 原子 rename；VMDK 由 raw 转换（中间 `raw` 也被声明为输出） |
| 13 | `image-iso` | 3385 | generate | —（**靠 inputs 隐式**） | — | build/live/root.ext2（=desktop-live-root 输出）, loader.elf, kernel.sys, middlelayer.sys, grub 字体, boot/grub/live.cfg, boot/grub/installer_embedded.cfg, tools/make_installer_iso.py | **1 声明**：build/images/leonos4.iso | `python3 tools/make_installer_iso.py --out ... --stage build/iso --boot-image build/live/efiboot.img --loader ... --kernel ... --middlelayer ... --installer-root build/live/root.ext2 --grub-font ... --work-dir build/live/work --grub-efi-dir ... --grub-config boot/grub/live.cfg --bios` | `run-iso`, `release` | 无 root/无 mount。`grub-mkstandalone`, `grub-mkimage -O i386-pc`, `mkfs.fat -F 16`, `mcopy`, `xorriso` | **未声明的额外输出**：`build/live/efiboot.img`、`build/iso/`、`build/live/work/` 都不在 outputs → 删了不会重建（A07 反例）；`--bios` 分支需要 `grub-mkimage`（未在 `task_tools` 表内） |
| 14 | `installer` | 3565 | aggregate | all, installer-root, installer-image | G | — | — | 无 | `release`；CI `build-installer.yml:203`；`test-musl-distribution`（间接经 installer-root） | 同 image-* | — |
| 15 | `release` | 3612 | generate | image-vmdk, image-iso, installer, sdk | — | docs/THIRD_PARTY.md, config_path, build/images/{leonos4.vmdk,leonos4.iso,leonos4-installer.iso}, LeonOS4-Developer-SDK.zip | build/release/.release-stamp (+ 条件 `SHA256SUMS.txt`, `THIRD_PARTY_NOTICES.md`) | **Python action `make_release`**（build.py:3587-3609）：分块 sha256 四产物、复制 docs/THIRD_PARTY.md、`stamp.touch()`；关闭开关时**删除**已生成文件 | 发布流程；CI | 无 | 输出集合随 `CONFIG_RELEASE_WRITE_CHECKSUMS` / `CONFIG_RELEASE_INCLUDE_THIRD_PARTY_NOTICES` 变化（声明式输出集随配置漂移）；stamp-only 输出无法证明 SHA256SUMS 完整 |
| 16 | `run` | 3556 | command | image-vmdk | **C** | build/images/leonos4.vmdk | — | `qemu_command(paths, values)`（build.py:631-693）：`qemu-system-x86_64 -cpu max\|-enable-kvm -cpu host -machine pc\|q35 -m <MB> -smp <N> [-bios OVMF] -display gtk,grab-on-hover=on,show-cursor=on -serial stdio -device VGA,xres=,yres= -netdev user,id=net0 -device <e1000 等> -audiodev sdl,id=snd0 -device AC97 -drive file=leonos4.vmdk,if=none,id=sata0,format=vmdk -device ich9-ahci,id=ahci -device ide-hd,...` | 人（前台 GUI） | **KVM**（`/dev/kvm` 权限，非 root）、**GTK/SDL 显示**、OVMF 文件解析（`resolve_qemu_ovmf_path` 扫 6 个候选路径 build.py:485-512） | 读环境变量 `LEONOS_QEMU_IDE` / `LEONOS_QEMU_NVME` 改变拓扑（**未声明的环境输入 → 不进指纹**，spec §6.2 禁止）；交互前台进程 |
| 17 | `run-debug` | 3557 | command | image-vmdk | **C** | 同上 | — | 同上 + `-no-reboot -no-shutdown` | 人 | 同 `run` | 同 `run` |
| 18 | `run-iso` | 3558 | command | image-vmdk, image-iso | **C** | vmdk, iso | — | 同上 + `-no-reboot -no-shutdown -cdrom build/images/leonos4.iso`（vmdk bootindex=2） | 人 | 同 `run` | 同 `run` |
| 19 | `menuconfig` | 3662 | command | kconfig-mconf | **A** | Kconfig, Kconfig.components, build/host/kconfig-frontends/bin/kconfig-mconf, configs/default.conf, configs/components.toml, tools/generate_component_kconfig.py, tools/kconfig_sync.py, buildsystem/components.py | — | **Python action**（build.py:3644-3659）：`generate_component_kconfig.py` → `copy2 default.conf→config` → `kconfig-mconf Kconfig`（env `KCONFIG_CONFIG=<绝对路径>`，**要求 TTY**，runner.py:1047-1049）→ `kconfig_sync.py` | 之后的所有构建（配置被重写） | 需 ncurses 宿主开发库（`kconfig-mconf` 自身构建） | 交互式 + 图内动作 + 写用户配置；spec §4 要求 `menuconfig` 不触发生产构建 |
| 20 | `defconfig` | **不是图节点** | — | — | — | configs/default.conf | buildsystem/config/leonos.conf + 全套 sync 产物 | `main()` 直调 `apply_defconfig`（4354-4358）→ `copy2` + `sync_config_file`（subprocess `generate_component_kconfig.py` + `kconfig_sync.py`） | 同 menuconfig | 无 | 与 `config-sync` target 行为重复实现两遍（4328-4342 vs 947-961）——迁移时应合并成一条规则 |
| 21 | `clean` | 3678 | command | — | **A** | — | — | **Python action**（4671-4676）：`rmtree(build/, buildsystem/out, buildsystem/state/targets/, buildsystem/tmp/)` + `store.clear_target_states()`（删 `targets.json` 及 `targets/*.json`）+ `paths.ensure()` | — | 无 | **不做所有权校验**，直接 rmtree 硬编码目录（spec §6.3 要求拒绝空 O / `/` / 源码根 / 逃逸符号链接）；不清 `buildsystem/deps`、`buildsystem/cache`（这点与 spec §4 clean 语义一致） |

---

## 4. 未列入 `help` 但生产必需的目标（按区域）

### 4.1 工具链 / sysroot / 上游（`tools/build_*.py`）

| 目标 | 行号 | kind | depends_on | always | 输出（节选） | 命令/被调用者 | 网络 | 难点 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `musl` | 768 | compile | — | — | `build/musl/sysroot/` 下 **12 个**：`.leonos-musl.json`, `lib/libc.so`, `lib/libc.a`, `lib/libmimalloc.so.3`, 2 个 license, `lib/mimalloc.o`, `crt1.o`, `Scrt1.o`, `crti.o`, `crtn.o`, `libssp_nonshared.a` | `python3 tools/build_musl.py --build-dir build/musl --prefix build/musl/sysroot` → 内部：`git rev-parse`（断言 musl/mimalloc 固定 commit）、`git archive`、`patch -p1`、`configure`、`make -j<min(cpu,16)>`、`make install`、`clang`、`llvm-ar`、`clang -print-resource-dir`、`ld.lld -shared` | **需要 git**（子模块），不联网 | 一个 Python 脚本产出 **整个 sysroot 树（>12 个真实文件）** 但只声明 12 个输出；内部自带 `-j` 上限（spec §6.1 禁止写死子 make -j）；`--build-dir` 复用 + 条件 `make clean`（按 patches 元组变化） |
| `auth-upstream` | 794 | command | musl | — | `build/auth-upstream/root/` 下 libpam.so.0, libcrypt.so.2, 2 个头, usr/bin/{sudo,passwd}, bin/su, util-linux 命令/库集合（来自 `storage_tools.UTIL_LINUX_*`）, libuuid.a, libblkid.a, lib/security/pam_leonos_password.so | `python3 tools/build_auth_upstream.py linux-pam sudo util-linux shadow --work build/auth-upstream --musl build/musl/sysroot` → `curl`（fetch_auth_upstream.fetch，SHA-256 校验 + `tarfile` data filter + `verify_source_tree`）、`make -C linux ARCH=x86_64 headers_install`、**`meson setup/compile/install`（linux-pam！→ 底层 Python+Ninja）**、其余包 `configure`/`make`/`make install`、`patch --fuzz=0`、写 `<pkg>-build.json` 元数据、`chmod 04755 sbin/unix_chkpwd` | **是**（curl，缓存 `buildsystem/deps/auth`）；无 root（DESTDIR 装到 work/root） | **spec §2 特别点名的 Meson 路径**；`build_auth_upstream.py:20-22` 还**反向 import** `build_musl.REVISIONS` 与 `build_musl_ltp.SOURCES`（Python 模块间硬耦合，Linux 内核 tarball URL/摘要来自 LTP 模块常量）；linux-pam 输出目录名带 patch 摘要（`linux-pam-<sha[:16]>`）→ 内容寻址式目录 |
| `storage-upstream` | 814 | command | musl, auth-upstream | — | `build/storage-upstream/root/.storage-package.json` + `FORMATTER_COMMANDS` | `python3 tools/build_storage_upstream.py --work build/storage-upstream --musl ... --util-root build/auth-upstream/root` → `configure`/`make`/`make install`、`readelf -l -d` 校验 | 复用 auth 缓存（curl 可能） | 同上 |
| `ncurses` / `vim` | 836（循环 830-845） | compile | vim→{musl,ncurses}；ncurses→{musl} | — | 各自 prefix 下 stamp + 库/可执行 + 数据目录（ncurses 5 项含 `share/terminfo`；vim 3 项含 `share/vim/vim91`） | `python3 tools/build_terminal_packages.py {ncurses\|vim} --work ... --prefix ... --musl ... [--ncurses ...]` → `git rev-parse`、`clang -print-resource-dir`、`./configure`、`make -j`、`make install DESTDIR=` | 需 git（子模块），不联网 | 目录树型输出（terminfo / vim91 整树）；`inputs` 含 `*collect("third_party/<pkg>/**/*")`（**全树枚举进指纹**，任何文件增删都改声明） |
| `file-magic` | 1572 | generate | — | — | `build/userland/magic.mgc`, `build/userland/magic.stamp` | `python3 tools/build_file_magic.py --source third_party/file --output ... --stamp ...` → `autoreconf`、`configure`、`make` | 否 | 2 输出 + 主机 autotools 链 |
| `file` | 1590 | compile | musl, runtime, runtime-loader | — | **5**：file.elf, libmagic.so.1, libmagic.a, build/generated/file/magic.h, file.stamp | `python3 tools/build_file.py --source ... --port ... --musl-prefix ... ... *compile_option_args *linker_option_args` | 否 | 5 输出；`--compile-flag=`/`--linker-flag=` 变长参数把**编译策略塞进子脚本**（spec §6.2 的"参数签名"必须覆盖这些） |
| `sqlite` | 1645 | compile | musl, runtime, runtime-loader | — | 4：sqlite.so.3, sqlite.a, build/generated/sqlite/sqlite3.h, sqlite.stamp | `python3 tools/build_sqlite.py ...` → `make`（`Makefile.linux-gcc`）、`clang`；inputs 含 `tool/mksqlite3c.tcl`（**tclsh**，未登记） | 否 | 同上 |
| `portablegl` | 1686 | compile | runtime, runtime-loader | — | 3：libportablegl.so.1, libportablegl.a, portablegl.stamp | `python3 tools/build_portablegl.py ...` | 否 | 需要非 GPR ABI（`cflags_glxgears` 去掉 `-mgeneral-regs-only`） |
| `busybox-source-revision` | 1757 | generate | — | **A** | `build/busybox/source-revision.txt` | **Python action**：`git -C third_party/busybox rev-parse HEAD` + `ensure_parent`（内容稳定写） | 需 git | `always=True` 但内容稳定 → Make 下应表达为"每次 lstat/git 检查但只在内容变化时更新"，spec §6.2 允许的一次轻量 FORCE 检查 |
| `busybox` | 1766 | compile | busybox-source-revision, musl, auth-upstream | — | 3：busybox.elf, busybox.stamp, busybox.links | `python3 tools/build_busybox.py --source third_party/busybox --config userland/busybox/leonos.config ... --official-source` → `make`（busybox 需要 `flex`,`bison`,`host gcc`） | 否 | `busybox.links` 被 `esp:layout-links` 读取并逐行校验 → **文件内容成为另一个 target 的语义输入**（A06 关组件时依赖 prune） |
| `nano` | 1807 | compile | runtime, runtime-loader | — | nano.elf, nano.stamp | `python3 tools/build_nano.py ... --dynamic` | 否 | — |
| `fastfetch` | 1842 | generate | — | — | fastfetch.elf, fastfetch.stamp, **`tools/package_fastfetch.py.CACHE`（模块常量，非 build 下路径）** | `python3 tools/package_fastfetch.py --cache <CACHE> [--source $LEONOS_FASTFETCH_BINARY] --output ... --stamp ...` → `readelf -l -d`、`curl` GitHub release 2.68.1 | **是**（除非环境变量给预编译二进制） | **读环境变量 `LEONOS_FASTFETCH_BINARY` 且把它塞进 `inputs`**（build.py:902-904）→ 环境改变输入集；输出之一在源码树 `tools/` 下 |
| `kconfig-mconf` | 3627 | compile | — | — | `build/host/kconfig-frontends/bin/kconfig-mconf` | `python3 tools/build_kconfig_frontends.py --source third_party/kconfig-frontends --work-dir build/host/kconfig-frontends-work --prefix build/host/kconfig-frontends` → `./bootstrap`（autoreconf 链）、`./configure`、`make -j`、`make install` | 否（inputs 用 `collect(".../**/*")` 排除 `.git` 段） | 主机工具 bootstrap 却挂在生产图里；spec §4 把它归 `tools`/`menuconfig` 前置 |

### 4.2 运行库与归档

| 目标 | 行号 | kind | 输出 | 命令 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `boot-logo` | 1201 | generate | `build/include/generated/boot_logo.h` | `python3 tools/generate_boot_logo.py --input logo.png --out ...` | 纯 Python 图像→头生成器（spec §7 要求换 C 生成器或锁定非 Python 工具） |
| `gbk-table` | 1402 | generate | `build/include/generated/leonos_gbk_table.h` | `python3 tools/generate_gbk_table.py --source third_party/litehtml/src/encodings.cpp --output ...` | 输入是**另一个子模块的 .cpp**；产物被 musl-runtime/installer-runtime 全体编译隐式依赖 |
| `generate:libpng-config` | 998 | generate | `build/generated/libpng/pnglibconf.h` | **Python action**（978-995）：读 `pnglibconf.h.prebuilt`，在 `/* end of options */` 前插 4 行 `#undef`，`ensure_parent` 内容稳定写 | 纯文本转换 → spec §8 的 `tools/host/assets` C 工具 |
| `generate:glxgears-source` | 1740 | generate | `build/generated/glxgears/gears-upstream.c` | **Python action**（1723-1746）：删 `#define PORTABLEGL_IMPLEMENTATION` 一行，marker 计数≠1 即 GraphError | 同上 |
| `compile:zlib:<file>` ×11 | 1420 | compile | `build/obj/zlib/...` | clang + `-MMD -MF <dep>` | 11 个源来自 `ZLIB_SOURCES` 常量（build.py:274-278），**手写清单** |
| `archive:zlib` | 1425 | link | `build/userland/libz.a` | `llvm-ar rcs` | **`ar rcs` 追加式** → 删除源文件后旧成员残留（spec §6.1 明确禁止；对照 `musl-extension-archive` 用了 rebuild_archive.py） |
| `compile:libpng:<file>` ×15 / `archive:libpng` 1434 | 1430 / 1434 | compile / link | `build/obj/libpng/*` / `build/userland/libpng.a` | clang / `llvm-ar rcs` | 同样 `ar rcs` 追加问题；inputs 含生成的 `pnglibconf.h` |
| `compile:musl-runtime:<rel>` ×**107** | 1458 | compile/assemble | `build/obj/musl-runtime/<rel>.o` + `.d` | clang `-target x86_64-linux-musl`（.S 用 `[cc,"--target=...","-fPIC","-Iinclude/uapi"]`） | 源集合 = `userland/libc/src/*.c|*.S`(46 含 auth) + 35 mbedtls + 11 zlib + 15 libpng = **107**；隐式输入含 `gbk_table_header`、`libpng_config`、auth 头 |
| `musl-runtime` | 1465 | link | `build/system/lib/libleonos.so.2` | `ld.lld -shared --no-undefined --hash-style=both -soname libleonos.so.2 ... -l:libmimalloc.so.3 <auth libs> -lc <compiler-rt.a>` | 输入含 **`compiler_rt_archive`（由 `find_compiler_rt_archive(cc)` subprocess 探测，build.py:229-262）** → 工具链身份不进声明，只在绝对路径变化时进指纹 |
| `musl-extension-archive` | 1474 | link | `build/musl/lib/libleonos.a` | `python3 tools/rebuild_archive.py --ar llvm-ar --output ... <objs>` → 临时文件 `ar rcsD` + `replace` | **这就是 spec §6.1 "静态库重建到新临时文件" 的现有实现**，应转为 C/Shell 工具 |
| `compile:installer-runtime:<rel>` ×107 / `installer-runtime` 1492 / `archive:installer-libc` 1496 | 1487 / 1492 / 1496 | compile / link / link | `build/userland-installer-policy/libleonos.so.2` / `build/musl/lib/libleonos-installer.a` | `musl_link.shared(...)` / `rebuild_archive.py` | 与 musl-runtime 只差一个 `-include autoconf-installer.h` → **两遍全量编译 214 个 TU**；`musl_link.py` 是纯 Python argv 构造器（**构建期 Python**，非运行期） |
| `runtime`, `runtime-loader`, `archive:libc`, `archive:libc-static` | 1500-1503 | aggregate(group) | → musl-runtime / musl / musl-extension-archive / musl-extension-archive | 无 | 4 个别名，只为让 `depends_on` 可读 |
| `compile:dynlinkerror` 1504 / `dynlinkerror` 1507 | 1504 / 1507 | compile / link | `build/userland/dynlinkerror.elf` | `musl_link.executable(..., static=True)` | 静态链接用 `crt1.o` + `--image-base=0x4000000` + `mimalloc.o` |
| `musl-sdk` | 1513 | generate | `build/musl/leonos-musl-sdk.tar.gz`（**单声明输出**，实际 stage 出 `build/musl/sdk/` 整棵树） | `python3 tools/package_musl_sdk.py --prefix sysroot --runtime ... --archive ... --png-config ... --stage build/musl/sdk --output ...` | `musl-probe:*` 与 `musl-ltp` **直接执行 `build/musl/sdk/bin/leonos-musl-cc`（= `tools/leonos_musl_cc.py`，Python）** → 生产链上真实 exec Python |
| `musl-probe:dynamic` / `musl-probe:static` | 1529（循环 1527） | link | `build/musl/tests/musl-abi-{mode}.elf` | `(PYTHON, paths.out/"musl/sdk/bin/leonos-musl-cc", "-O2", "-DPROBE_KIND=...", ["-static"], "tools/tests/musl_guest_test.c", "-o", ...)` | **命令 argv[0] 是 python3**；15 个测试 .c 全列 inputs |
| `musl-probes` | 1550 | aggregate | — | — | 被 `musl-installer-probes` 依赖 |
| `musl-ltp` | 1557 | generate | **22 个** `build/musl/ltp/<name>.elf` | `python3 tools/build_musl_ltp.py --cache build --sdk build/musl/sdk --out build/musl/ltp` → **`curl` 下载 LTP(linux-test-project tar.gz) 与 linux-6.12.tar.xz**、`make headers_install`、`make autotools`、`ltp/configure`、`make include-all lib-all`、逐个 `make -C testcases/... <t>`、再用 **`leonos-musl-cc`（Python）** 链接 | **是** | 网络下载 + 上游 autotools + Python 链接器；22 输出 |

### 4.3 用户态应用（详见 §6 动态规则）

关键固定 target：`app:pleditor`(2029)、`archive:stardustui`(2109)、`app:stardust{hello,layout,showcase}`(2135 循环)、`musl-userland`(2219)、`installer-policy:{desktop,settings}`(2236 循环)、`installer-tool:gptinit`(2254)、`app-manifests`(2305)、资源组（2329-2353）。

`musl-app:<app>` ×**62**（2216，`add_copy` 到 `build/musl/userland/<app>.elf`）——把用户态 ELF 再复制一份给 musl checkpoint 镜像使用，是 `musl-checkpoint-root:*` 的输入。

`app-manifests`（2305）输出 = **每个 registry 应用一个 `manifest.ini`，路径含 guest 目录结构**（`registry_manifest_dir / (runtime_app_relative(app,'elf').parent / "manifest.ini")`），命令 `python3 tools/generate_app_manifests.py --out-dir ... --apps <62 个名字> --system-apps <24 个>`，`depends_on=("userland",)`。

### 4.4 GRUB / staging（ESP）

| 目标 | 行号 | kind | 输出 | 命令/动作 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `grub-bdf` | 2517 | generate | `build/generated/grub/leonos-pixel.bdf` | `python3 tools/make_grub_font.py --out ...` | PSF→BDF，Python |
| `grub-font` | 2525 | generate | `build/generated/grub/leonos-unicode.pf2` | **`grub-mkfont -n "LeonOS Pixel" -o ... <bdf>`**（外部程序，非 Python） | 唯一纯外部工具的字体链 |
| `grub-efi` | 2537 | generate | `build/esp/EFI/BOOT/BOOTX64.EFI` | **`grub-mkstandalone -d <grub_dir> -O x86_64-efi -o ... --modules="part_gpt fat multiboot2 normal search search_fs_file configfile echo serial terminal video video_bochs video_cirrus efi_gop efi_uga all_video font gfxterm gfxmenu" "boot/grub/grub.cfg=boot/grub/embedded.cfg"`** | `-d` 参数在仓库目录与 `/usr/lib/grub/x86_64-efi` 间切换（`using_system_grub`，**绝对路径进命令 argv → 进指纹**，spec §6.2 需显式化） |
| `staging-prune` | 2652 | generate | `build/generated/component-staging-prune.json` | **Python action `prune_component_staging`**（2553-2649，约 100 行）：删 `userland/{init,serviced}.{elf,stamp}`、`layout.RETIRED_TOOL_PATHS`、约 15 条硬编码 obsolete staging 路径、`build/api/*.api`、9 个 legacy 目录（boot/system/programs/drivers/docs/share/lib64/usr/lib64/opt/busybox）、重置 `bin`/`sbin`/`lib`、按组件删除禁用应用的目录与符号链接、删除禁用共享库、删 `LEONOS_LIB` 下 4 个旧 payload、按 `entry` 删图标/ini/theme，最后写 selection JSON | **A06 的核心机制**：Make 无法用"stamp 存在"表达"上次构建残留必须删除"。`remove_staging_path_within`（537-547）做 staging 逃逸防护。`action_key` 字符串本身是版本计数器（`staging-prune-musl-v11-retired-api-packages`） |
| `esp:rootfs` | 2700 | generate | `build/generated/rootfs-layout.json` + **`system/rootfs` 下全部 93 个文件/符号链接在 staging 的对应物** | **Python action `stage_rootfs`**（2668-2698）：`layout.layout_directories`、复制 93 项（符号链接保 target）、按文件名强制 mode（shadow/gshadow 0600、sudoers 0440、否则保源 mode）、建 `etc/skel/{desktop,documents,downloads}` 0700、检测并删除 magic 字节 `32535541 00000000` 的旧 `var/lib/leonos/users.db`（非空即 GraphError） | 输出集随 `system/rootfs` 内容变化（**glob 驱动的声明**）；guest 元数据（uid/gid）不在这里表达，靠后续 fakeroot/mke2fs `-d` |
| `esp:rpr-client` | 2732 | generate | `staging/etc/leonos/rpr.conf` + 4 个 `staging/usr/sbin/leonos-*` 脚本 | **Python action `stage_rpr_client`**：写 `RPR_BASE_URL=<CONFIG_RPR_BASE_URL>` + `RPR_PUBLIC_KEY=leonos-rpr.rsa.pub`（0644），复制 4 个脚本并 chmod 0755 | URL 在构图时校验（必须 https、无空白/引号，build.py:709-714）；4 输出 + 配置派生内容 |
| `esp:auth` | 2790 | generate | `build/generated/auth-payload.json` + `staging/usr/sbin/fdisk` + `UTIL_LINUX_COMMANDS/LIBRARIES` 全集 | **Python action `stage_auth_payload`**（2747-2788），`always=True`：校验 fdisk 存在、校验每个 payload 文件不逃逸 auth_root（`resolve().is_relative_to`）、先删同名符号链接、`shutil.copytree(symlinks=True, dirs_exist_ok=True)` 复制 bin/sbin/lib/usr、**只复制 rootfs seed 中不存在的 etc/ 配置**、建 4 个 sudo 目录并设 mode、删 2 个 obsolete、对 `usr/bin/{sudo,passwd}`、`bin/su`、`sbin/unix_chkpwd` **chmod 04755** | 集合型输出（整棵 util-linux 落地），`always=True` + 副作用式 prune |
| `esp:storage` | 2808 | generate | `build/generated/storage-payload.json`, `staging/usr/sbin/leonos-grub-installer`, `FORMATTER_COMMANDS` 全部 | **Python action `stage_storage_payload`**（2801-2806）+ `always=True`：`storage_tools.stage_filesystems(storage_root, staging)`、复制 grub-installer 脚本、透传 `.storage-package.json` | 同上 |
| `esp:terminal-packages` | 3020 | generate | `build/generated/terminal-payload.json` + 条件性 staging 路径（vim/LICENSE/defaults.vim；terminfo 两处/clear/infocmp/tput/COPYING） | **Python action `stage_terminal_packages`**（2817-2851）：整目录复制 + `/usr/bin` **逐子项删除后复制**（注释明说与别的 job 共享目录）+ terminfo 额外复制到 `/etc/terminfo` | **输出集合随组件开关动态变化**；与 `esp:layout-links` 争用 `/usr/bin/*`（BusyBox applet 链接"已打包命令优先"逻辑在 3220-3224） |
| `esp:grub-font` 2853 / `esp:grub-theme` 2858 / `esp:grub/grub.cfg`·`esp:loader.elf`·`esp:leonos/kernel.sys`·`esp:leonos/middlelayer.sys` 2871 / `esp:kerneldebug` 2875 / `esp:lib/ld-musl-x86_64.so.1`·`esp:lib/libc.so`·`esp:lib/libmimalloc.so.3`·`esp:usr/lib/leonos/libleonos.so.2` 2888 / `esp:license:{musl,mimalloc}` 2894 / `esp:dynlinkerror` 2918 / 7 个 `esp:<SYSTEM_FILES 目标路径>` 2924 / `esp:stardustui-theme:<stem>` 2957 / `esp:window-icon:<icon>` 2962 / `esp:minesweeper-asset:<name>` 2967 / `esp:driver:<driver>` 2972 / `esp:app:<app>` 2977 / `esp:icon:<app>` 2985 / `esp:manifest:<app>` 2991 / `esp:asset:{leonmmcoset,xiaobai}` 2997/3003 / `esp:busybox` 3029 + `esp:busybox:LICENSE` 3033 / `esp:file` 3039 + `:COPYING` 3043 + `:magic.mgc` 3048 / `esp:lua:lua.elf` 3053 + `:LICENSE` 3057 / `esp:cmd:cmd.elf` 3063 + `:cmd:{LICENSE,README.md}` 3070 / `esp:nano:COPYING` 3075 / `esp:fastfetch:{LICENSE,config,ascii,hyfetch-config,package}` 3081-3100 / `esp:sl` 3105 + `:LICENSE` 3109 / `esp:less` 3115 + 3 个文档 3122 / `esp:pleditor:LICENSE` 3127 / `esp:test:test.mp3` 3132 / `esp:config` 3155 + `esp:config:<system/config>` 3162 + `:display.conf` 3166 + `esp:boot-display.conf` 3170 + `:desktop-entries.conf` 3174 + `esp:doc:<*.hlp>` 3180 | generate（`add_copy` 566-577） | 各 1 个 staging 路径 | **Python action `copy_action`** → `context.copy` = `shutil.copyfile` + `shutil.copymode`（runner.py:538-548，注释说明必须保 exec 位否则 guest `execve` EACCES） | `action_key="copy-v3-preserve-mode"`。约 **350 个**（当前配置下 ~170）"复制一个文件"的节点 → Make 用 `cp -p` + order-only 目录即可，但要保住 mode 语义 |
| `esp:manifest` 2880 / `esp:musl-search-path` 2898 | generate | 1 个 staging 文件 | **`text_action`**（530-534）→ `ensure_parent` 内容稳定写 | 无输入声明（`inputs=()`）→ 靠 `action_key` 变化驱动，Make 下等价 `$(file >...)`/固定文本规则 |
| `esp:system-font` | 2944 | generate | 5 个 staging 字体（metro/win95/times-new-roman/simsun/system.psf） | **Python action `sync_ui_font`**（2932-2941）：先删 5 个 legacy 字体名，再 `context.copy` 5 次；`action_key="sync-ui-font-v10"` | 5 输出 + prune 副作用混在一个 action |
| `rpr-apps:oschinpt-index` | 3144 | generate | `build/rpr-apps/pinyin_simp.idx` | `python3 tools/make_oschinpt_index.py --input <rime dict> --output ...` | 输入是 300 万行级 Rime 词典，Python 解析 YAML（spec §7 资源生成器风险） |
| `esp:layout-links` | 3249 | generate | `build/generated/layout-links.json` + **`layout_link_map` 里每个 staging 符号链接路径本身**（约 200+） | **Python action `stage_layout_links`**（3204-3246），`always=True`：`layout_directories`、并入 BusyBox `busybox.links` 逐行（严格校验前缀/父目录/名字，非法即 GraphError）、**读上次 stamp 的 links 以删除已消失的链接**、`os.symlink` 逐个（内容一致则跳过） | **旧系统唯一"读上次状态算差集删除"的实现**，是 A06 的第二核心；Make 需要显式成员清单 + 差集删除工具（spec §6.1 末段）；输出集随 BusyBox config 与组件开关变化 |
| `esp:apk-ownership` | 3258 | generate | `build/generated/apk-ownership-inventory.json` | `python3 tools/apk_ownership.py --root build/esp --output ...`，`always=True`，`depends_on=("esp:layout-links",)` | **扫描整棵 staging 树** → 语义上是"校验器"而非生产者；其输出**不被任何 target 声明为输入**（3265 只把它加进 `esp_names`，未加进 `esp_outputs`） |

### 4.5 APK 仓库与 RPR

| 目标 | 行号 | kind | always | 输出 | 命令 | 网络/root/mount | 难点 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `apk-root` | 3282 | generate | **A** | `build/apk/normal/manifest.json`, `build/apk/root/lib/apk/db/installed`, `build/apk/root/usr/share/leonos/apk/repository/packages.adb` | `python3 tools/apk_distribution.py --source build/esp --output build/apk/root --work build/apk/normal` | **是**：`bootstrap()` 若 `build/apk-preparation-reference` 无参考件则 `curl` 清华镜像 `apk-tools-static-3.0.8-r0.apk`（SHA-256 固定）+ 两个 LICENSE URL；`openssl dgst -verify` 校验 Alpine 公钥；**`unshare -Ur` 跑 apk，失败回落 `fakeroot`**（`apk_distribution.py:39-58`）；签名 key 默认 `~/.local/share/leonos/apk-signing/key.pem`，缺失则 `openssl genpkey` 生成（0600） | **不 mount、不碰块设备**；不需要 root | `always=True` + 目录型输出 + 外部可变 APK 索引（spec §9 要求固定包版本与字节摘要）；`.lock` 串行 bootstrap；内部读 `readelf -dW` 与执行 `bin/busybox --list`（**在构建机上运行来宾二进制？** 仅在无 user-namespace 分支） |
| `rpr-apps` | 3303 | generate | **A** | `build/rpr-apps/repository/{leonos-helloworld.apk, leonos-doom.apk, leonos-oschinpt.apk, packages.list}` | `python3 tools/build_rpr_apps.py --output build/rpr-apps/repository --build-info include/generated/build_info.h --icon-dir ... --oschinpt-index ... --helloworld ... --doom ... --doomlauncher ... --oschinpt ...` | 同 apk-root（bootstrap + openssl） | `depends_on` 用 `if name in graph.targets` 过滤（3314-3319）→ **组件关闭时依赖集静默收缩**；输出目录被 `shutil.rmtree` 后重建；版本号来自 build_info.h 正则 |

### 4.6 镜像与安装器（除 §3 的 12/13/14 外）

| 目标 | 行号 | kind | 输出 | 命令 | 难点 |
| --- | --- | --- | --- | --- | --- |
| `desktop-live-root` | 3378 | generate | `build/live/root.ext2` | `python3 tools/make_live_root.py --tree build/apk/root --out ...` | 输入含 `*esp_outputs` + apk manifest + 5 个 tools 脚本 + test-account 清单；内部再调 `make_installer_root`/`make_ext2_root`/`make_image` 模块 |
| `installer-root` | 3398 | generate | `build/install/root.fat` | `python3 tools/make_installer_root.py --out ... --stage build/install/root --esp-tree build/apk/root --installed-policy-dir build/userland-installer-policy --policy-runtime libleonos.so.2 --policy-apps desktop settings --userland-dir build/userland --gptinit ... --generated-icons-dir build/generated/app-icons --size-mib <CONFIG_INSTALLER_ROOT_SIZE_MIB>` | 依赖 `app_elfs["desktop"]` 与 **`app_elfs["installer"]`**（`installer` 组件 `stage=False`，不在镜像里但必须构建）；`--policy-apps` 变长参数由组件开关推导 |
| `installer-image` | 3443 | generate | **2**：`build/images/leonos4-installer.iso`, `build/install/installer-efiboot.img` | `python3 tools/make_installer_iso.py --out ... --stage build/installer-iso --boot-image ... --loader/--kernel/--middlelayer/--installer-root/--grub-font/--work-dir build/install/--grub-efi-dir` | 多输出；`grub-mkrescue`/`xorriso`；CI 主产物（`build-installer.yml:203`） |
| `musl-checkpoint-root:{installer,probes,ltp}` | 3501（循环 3484） | generate | `build/musl/checkpoint-<s>/install/root.fat` | `python3 tools/make_musl_checkpoint.py --base build/install/root.fat --musl build/musl --stage ... [--abi-probes \| --ltp build/musl/ltp]` → 内部 **`debugfs -w -R mkdir/writewrite`** + `mcopy -o -i` | 用 `debugfs` 改已生成的 ext2 镜像（spec §10 rootless 方式）；`--ltp` 分支依赖 22 个 LTP ELF |
| `musl-installer`, `musl-installer-probes`, `musl-installer-ltp` | 3507（同循环） | generate | `build/images/leonos4-musl-<suffix>.iso` | `python3 tools/make_installer_iso.py ... --grub-config <诊断 cfg>` | 三条镜像链与生产 `installer-image` 共用一个脚本、只换 GRUB cfg |
| `musl-desktop-vim-root` | 3523 | generate | `build/musl/live-desktop-vim/root.fat` | `python3 tools/make_live_root.py --tree build/esp --out ...` | **`depends_on=("esp",)` 而 `image-vmdk` 走 `apk-root`** → 同一 staging 树两种消费者 |
| `musl-desktop-vim` | 3530 | generate | `build/images/leonos4-musl-desktop-vim-fixed.iso` | `make_installer_iso.py` | 三条诊断镜像链之外还多一条"fixed"变体，名字里的 `-fixed` 无对应语义；实测 157 个 `graph.add(Target(...))` 调用点**全部显式声明 kind**（无 dataclass 默认值漏网），可作为台账 kind 列的可靠性依据 |
| `images-iso` | 3566 | aggregate | — | — | 未在 help 列出 |
| `test-live-iso` | 3521 | test | — | `python3 tools/test_live_iso.py` | 无输出 → 每次跑 |

### 4.7 测试目标（`build.py test <item>` / 独立 target）

| 类别 | target | 行号 | 执行 |
| --- | --- | --- | --- |
| 纯主机 Python（无图依赖） | `test-oobe` 3681, `test-sudo-policy` 3685, `test-builtin-tool-removal` 3700, `test-installer-setup` 3702, `test-linux-memory` 3752, `test-linux-process-vm` 3754, `test-linux-sysv-msg` 3756, `test-linux-sysv-sem` 3758, `test-linux-pty` 3760, `test-power` 3762, `test-init-power` 3764, `test-linux-permissions` 3766, `test-storage-metadata` 3768, `test-storage-rename` 3770, `test-storage-mkdir-mount` 3772, `test-ext2-cache` 3774, `test-ext2-performance` 3776(写 `build/ext2-performance.json`), `test-ext2-write-batch` 3779, `test-storage-write-batch` 3781, `test-installer-copy` 3783, 循环 3785-3800 生成 13 个 `test-{linux-rootfs,apk-layout,apk-ownership,apk-distribution,linux-inventory,regular-file-io,tmpfs,linux-resources,linux-socket-batches,linux-threads,linux-descriptors,linux-ioctl-cloexec,linux-vfork-stack}`, `test-musl-distribution` 3801(依赖 installer-root), `test-uapi` 3805 | `python3 tools/test_*.py`；多数 `always=True` |
| 有 inputs 声明的主机测试 | `test-installer-input` 3704（11 个 C 源/测试文件）, `test-svga` 3719（collect 驱动）, `test-license-server` 3729, `test-los2w` 3736（`python3 -c` 内联导入 `los2w.selftest`）, `test-unix-paths` 3741, `test-linux-abi-contract` 3743, `test-abi-migration` 3808（**有输出** `build/generated/abi-migration-report.txt`）, `test-component-config` 3816, `test-kconfig-frontends` 3825（inputs 含 **`ROOT/build.py` 自身**）, `test-openrc-shutdown` 3833 | — |
| musl/终端包 ABI 测试 | `test-musl-abi` 785, `test-terminal-packages` 846, `test-fastfetch-package` 1857 | 依赖各自构建 |
| **QEMU/QMP 测试** | `test-qmp-terminal` 4043, `-pleditor` 4048, `-vi` 4052, `-vim` 4056, `-fastfetch` 4060, `-sl` 4064, `-less` 4068, `-dynlinkerror` 4072, `-abittest` 4076, `-cmd` 4080, `-stardust` 4084, `-glxgears` 4088, `test-qmp-suite` 4108 | **全部** `kind="command"` + `depends_on=("image-vmdk",)` + Python action `qmp_test`（3840-4042，约 200 行）：`tempfile.gettempdir()/leonos-qmp-<task_id>-<test>.{sock,qcow2}`、`qemu-img create -b vmdk -F vmdk` 覆盖层、替换 `-drive file=` 参数、`-qmp unix:...`、后台起 QEMU（stdout→`build/qmp-<test>-serial.log`）、`python3 tools/qmp_terminal_smoke.py` 驱动、`process.wait(timeout=15)` + terminate/kill、**用正则断言来宾串口日志**（`spawn path=` / `exec pid=` / `scheduler task exited pid=... code=0` / `[abittest] ALL PASS`）、部分要求 `build/images/*-qmp-smoke.ppm` 截图存在 | 这些是 spec §11 "测试必须识别来宾成功标记并核验退出状态" 的现有实现，**必须完整移植到 `make test-smoke`**，且它们共用 `context.runner.task_id` 做临时文件命名 |
| 聚合 | `test-all` 4126 | depends_on 由 `CONFIG_TEST_LICENSE_SERVER` / `CONFIG_TEST_LOS2W` / `qmp_suite_specs` / `CONFIG_TEST_COMPONENT_CONFIG` + 2 个恒定项组成 | 成员集随配置漂移 |

> 另有未注册的孤儿/特殊构图：`main()` 在 `test svga` 时**另建一张只含 `test-svga` 的空图**（build.py:4858-4863），绕过全图构建。

---

## 5. 图构造期的"非 target"副作用（必须迁移但清单里没有节点）

| 位置 | 行为 |
| --- | --- |
| build.py:749-755 | **环境变量决定工具链**：`CC`(默认 clang)、`CXX`(clang++)、`RUSTC`(rustc)、`AR`(llvm-ar)、`LD`(ld.lld)、`OBJCOPY`(llvm-objcopy)。工具身份**不进 fingerprint 之外的声明**（只以字符串出现在 command argv 里，进指纹），但 spec §6.2 要求 `$(origin CC)` 式区分与显式描述文件 |
| build.py:755 | **构图期 subprocess**：`clang -print-resource-dir`（`check_output`，失败即整体失败）→ 绝对路径进 `cflags_user_base` 的 `-isystem`，参与所有应用编译 argv |
| build.py:756-758 | 构图期 `find_compiler_rt_archive(cc)` 再跑 **2 个 subprocess** 并按候选路径探测文件系统；找不到 → `GraphError` |
| build.py:901-904 | 环境变量 `LEONOS_FASTFETCH_BINARY` 改变 `fastfetch` 的 inputs 和 argv |
| build.py:936-942 | `grub_efi_dir` 在 `boot/grub_modules_x86_64-efi` 与 `/usr/lib/grub/x86_64-efi` 间探测，影响 3 个 target 的 argv |
| build.py:647-648 | `LEONOS_QEMU_IDE` / `LEONOS_QEMU_NVME` 改变 QEMU 拓扑（qemu_command 在构图期求值） |
| build.py:3270-3275 | **构图后突变**：给所有输出落在 staging 的 target 追加 `depends_on=("esp:rootfs",)` |
| `import` 期 | `from tools import leonos_layout as layout`（64）、`from tools import musl_link`（65）、函数体内 `from tools.storage_tools import ...`（788）、`from tools.package_fastfetch import CACHE`（901）、`buildsystem.core.tui`（4834） |

---

## 6. 动态生成规则（按 `configs/components.toml` 与源码 glob）

组件计数（实测 2026-09-19）：**73 组件**（24 system-app / 35 program-app / 3 package-app / 7 tool / 4 library）。

| 规则 | build.py 位置 | 驱动 | 当前 checkout 实测基数 | 生成 target 名 | 迁移说明 |
| --- | --- | --- | --- | --- | --- |
| 内核 TU | 1210-1232 | `collect("kernel/ntclks/**/*.{c,S}", "kernel/ostui/**/*.c", "drivers/bootstrap/**/*.{c,S}")` 减 `drivers/bootstrap/storage/*.c` | **85**（state 里旧值 87） | `compile:kernel:<rel>` | `storage.c` 是 façade：`storage/*.c` 与 `storage_internal.h` 作为**该 TU 的 implicit_inputs**（1224-1226），Make 下需在单条规则里显式列出。特例：`boot_splash.c` 依赖 `boot_logo`；`kernel/ntclks/version.c` 的隐式输入是 **`build_info` + 其余全部 kernel+rust 源文件**（1229-1230）→ 任何一个内核文件改动都重建 version.o（放大重编译面，spec §6.2 "按组件/动作拆签名"直接反例） |
| loader TU | 1361-1368 | `collect("boot/loader/**/*.{c,S}")` | **2** | `compile:loader:<rel>` | `main.c` 额外依赖 `autoconf, loader_integrity, boot_logo`；`loader_integrity.h` 依赖 `kernel.sys`+`middlelayer.sys` → **kernel↔loader 双向串接**，Make 下是一条天然回边链，务必勿形成环 |
| 驱动 TU | 1384-1396 | `DRIVER_MODULES` × `collect("drivers/<d>/**/*.{c,S}")` | 5 模块 × 1 源 = 5 | `compile:driver:<d>:<rel>` + `driver:<d>` | `ld -r` 部分链接（`.drv`） |
| 运行库 TU（两份 ABI） | 1452-1463 / 1480-1491 | `libc_sources(81) + zlib(11) + libpng(15)` | **107 × 2 = 214** | `compile:musl-runtime:<rel>` / `compile:installer-runtime:<rel>` | 同一批源用两个 autoconf 各编一遍；对象目录 prefix 分别 `musl-runtime` / `musl-installer-runtime` |
| 普通应用 TU | 2191-2210 | `user_app_sources(app)` = `userland/apps/<app>/*.{c,S}`（doom 额外 + `third_party/doomgeneric/doomgeneric/*.c` 过滤 14 个宿主后端文件，实测 **81** 个额外源） | **173**（不含 doom 的 81 → 含 doom 后 **254**） | `compile:app:<app>:<rel>` | 组件级 `cflags_app` 分派：`doom` / `mp3play` / `glxgears` / 默认（2194-2198）；`glxgears` 额外隐式输入 `generate:glxgears-source` 输出 |
| 应用链接 | 2207 | `build_user_apps`（app 类且 build=y） | **62** | `app:<app>` | 其中 `nano/fastfetch/pleditor` 复用别的 target 的 ELF（2180-2188），`stardustui_apps`（depends 含 stardustui 的 3 个）走专用分支 |
| stardustui TU | 2101-2106 | 固定 7 个 `third_party/stardustui/src/*.cpp` + `userland/stardustui/src/platform_leonos.cpp` + `collect(components/*.cpp)` | **17** | `compile:stardustui:<rel>` | C++17 `-fno-exceptions -fno-rtti -nostdinc++`，独立 ABI |
| stardust 示例 | 2122-2143 | 3 个硬编码 example | 3×(wrapper+example+link) = **9** | `compile:app:<app>:<wrapper>`、`compile:app:<app>:<example>`、`app:<app>` | `depends_on=(compile_name, wrapper_compile_name, "archive:stardustui", "runtime", "runtime-loader")` |
| 安装器策略应用 | 2226-2241 | `installer_policy_apps` = (desktop, settings) ∩ image=y | TU **14 + 1**，链接 2 | `compile:installer-app:<app>:<rel>`、`installer-policy:<app>` | 与正常 desktop/settings 是**同源码不同 autoconf** 的第二次全量编译 |
| 用户态 ELF 复制 | 2214-2217 | `app_elfs` | **62** | `musl-app:<app>` | 复制到 `build/musl/userland/` |
| staging 应用/图标/清单 | 2975-2993 | `staged_user_apps`（system-app+program-app 且 image=y）、`entry=y`、`registry_apps` | 约 62 / 28 / 67（state 实测 esp:app 62、esp:icon 28、esp:manifest 67） | `esp:app:<app>`、`esp:icon:<app>`、`esp:manifest:<app>` | 目标路径由 `tools/leonos_layout.app_exec_path()` 计算，**guest 路径逻辑在 Python 里**（spec §10 要求清单化） |
| 应用图标 | 2339-2342 | `build_user_apps` | **62** 个 bmp 作为**一个 target 的多输出** | `app-icons` | `python3 tools/make_app_icons.py --out-dir build/generated/app-icons --apps <62>` |
| 应用 manifest | 2305-2314 | `registry_apps` | **67** 输出 / 1 target | `app-manifests` | 输出**路径本身含 guest 目录层级**；`--apps` argv 是 62+ 个字面量 → 组件增删即改指纹 |
| 测试 target 列表 | 3785-3800 | 13 项硬编码 suffix 元组 | 13 | `test-<suffix>` | — |
| QMP 测试 | 4043-4114 | 12 个 `qmp_test(...)` lambda + `qmp_suite_specs`（3840-4105，条件随配置） | 12 + suite | `test-qmp-*` | 每个都是独立 Python 闭包动作 |

---

## 7. `buildsystem/cache`（唯一真正内容寻址的地方）

`tools/apk_distribution.py:88-120`：`_download_verified(urls, destination, expected_sha256)`，若 destination 在 ROOT 下则
1. 查 `buildsystem/cache/apk/downloads/<sha256>`，命中即 `shutil.copyfile` 返回（**键 = SHA-256，不查 URL**）；
2. 未命中则逐个 `curl --fail --location --retry 3` 到 `<name>.download`，摘要匹配才 `replace`，再 `copyfile→<sha256>.tmp→rename` 入缓存。

`buildsystem/cache/apk/distributions/<64位十六进制>/root/...` 是解包后的 APK 根（当前 36 份，每份含完整 vim 等，**这就是任务描述把 `buildsystem/` 数成 7150 行 Python 的原因**）。`buildsystem/deps/apk-tools/.lock` 用 `flock(LOCK_EX)` 串行 bootstrap。

> spec §3 说"不实现内容寻址构建缓存"，但 §9 要求 `cache/downloads/` 保持"校验过的下载文件 + 与 profile 无关"——**旧系统这一条已经符合**，迁移时应保留同语义（curl + sha256 文件名）而非重新发明。

---

## 8. 下游消费者（外部契约）

| 消费者 | 引用 | 需要更新的点 |
| --- | --- | --- |
| `.github/workflows/build-installer.yml:145,188-191,201-203` | `build.py run defconfig`、`build.py info release/app:stardusthello/userland --profile default`（**用 `info` 的 JSON 输出做断言**）、`run image-vmdk`、`run image-iso`、`run installer` | spec §4 无 `info` 命令 → 需要新的"清单/契约"输出面（spec §5 `tools/host/manifest`） |
| `.github/workflows/publish-rpr.yml:68-69` | `run defconfig`、`run rpr-pages` | 需 `LEONOS_APK_SIGNING_KEY` |
| `.github/workflows/code-count.yml`, `sourcehut-sync.yml` | 不引用 build.py | — |
| `.vscode/tasks.json:14,37,56,75,94,166,295` | `run image-vmdk/all/kernel/installer/run/clean`、`test qmp-terminal`，并透传 `--theme ${config:leonos.buildTheme}` | 主题面（runner.py:35 `LOG_THEMES`）在新系统里被 spec §3/§11 判定为非目标 → 需删配置项 |
| `README.md`, `AGENT.md`, `docs/BUILDSYSTEM.md` 等 19 个 md | 文档契约 | spec §15 要求新文档 |
| `pyproject.toml`, `MANIFEST.in` | 打包 Python 自身 | P4 删除时一并处理 |
| 来宾内 | `build/esp/etc/leonos/rpr.conf`（RPR URL）、`/usr/share/leonos/apk-ownership.json`、`/usr/share/leonos/resources/*`、`manifest.ini` 族、`/etc/ld-musl-x86_64.path`、`grub/fonts/leonos-unicode.pf2`、`/leonos/kernel.sys` + `osmlayer.manifest` | spec §10 清单化时必须逐条对齐 |

---

## 9. 机制 → Make 映射判定

### 9.1 Make 原生可替代（**删除旧实现，不重做**）

| 旧机制 | Make 等价 |
| --- | --- |
| `BuildGraph.dependencies()` 输出→生产者反查（model.py:93-108） | 前置条件即文件路径 |
| `closure()` DFS 拓扑 + 环检测（model.py:159-178） | Make 原生依赖解析与 "Circular ... dependency dropped" |
| `_schedule()` 线程池 + 就绪集 + 失败传播（runner.py:724-773） | `-j`、`.DELETE_ON_ERROR`、退出码传播 |
| `parse_depfile()`（runner.py:469-503） | `-MMD -MP` + `-include` |
| `ProgressRenderer` / `--theme linux\|meson\|cargo` / `TaskLogger.building()` 的 `<N> Building`（runner.py:102-424） | spec §3 明令不重做；用 Make 原生输出或 `.NOTPARALLEL`-free 的 `MAKE_TERMOUT` |
| `build.py map`（curses） | `make -p` / `--trace`（spec §4） |
| `explain_target` / `build.py why`（4513-4533） | `make --trace`、`-d`（不必移植 JSON 面，但 spec §15 验证记录需替代手段） |
| `graph.resolve_target(源文件)`（`gen <file>`） | Make 的目标即文件路径 |
| `require_tools` 的存在性检查 | `make doctor`（但要重写完整表，见 §2.8） |

### 9.2 必须显式重做（Make 无原生等价物）

| 旧机制 | 为什么必须重做 | 目标承载（spec 章节） |
| --- | --- | --- |
| **mtime 判定对内容不敏感** + `_fingerprint` 只哈希声明 | Make 默认按时间戳；`touch`/checkout 会漏重建 | §6.2 参数签名文件（内容变化才原子替换）+ 一次 FORCE 检查 |
| `always=True` 的 35 处用法（其中 `staging-prune`、`esp:auth`、`esp:storage`、`esp:layout-links`、`esp:apk-ownership`、`apk-root`、`rpr-apps`、`rpr-pages`、`config-sync`、`build-info`、`busybox-source-revision`、`menuconfig`、`clean` 是**语义必需**） | FORCE 不能直接挂对象/链接/镜像 | §6.2 |
| **prune 语义**：`staging-prune` + `esp:layout-links` 读上次 stamp 算差集删除 | Make 只会"缺了就重建"，不会"多余就删" | §6.1（稳定排序成员清单）、§10（从空 staging 开始、原子发布）→ **必须新建 C/Shell 工具 + 成员清单** |
| **多输出**：`image-vmdk`(4)、`musl-ltp`(22)、`app-icons`(62)、`app-manifests`(67)、`esp:layout-links`(200+)、`installer-image`(2)、`release`(1-3 条件)、`musl`(12+) | 单个 stamp 存在不代表全部输出完整 | §6.3 `&:` grouped targets |
| **未声明的副作用输出**：`image-iso` 的 `build/live/efiboot.img`、各 `*-work/` 目录、`musl-sdk` 的 `build/musl/sdk/` 整树、`package_devtools.py` 写的 `devtools/` | A07 会失败 | §6.3 |
| `rebuild_archive.py`（临时文件 + rename 重建 archive） | `ar rcs` 追加会残留已删成员（`archive:zlib`、`archive:libpng`、`archive:stardustui` 现在就是**追加式，已有此 bug**） | §6.1 → 统一改用 C 工具或 `ar rcD` 到新文件 |
| 任务级互斥（**旧系统完全没有**） | spec §6.3 要求同 O 双顶层构建拒绝或安全串行 | §6.3 flock + 所有权标记 |
| `ensure_parent` / `write_if_changed` 的"内容稳定写"（散落在 build.py 与 kconfig_sync.py） | 生成头/签名文件不能无谓 touch | §8 `write_file_if_changed` |
| `resolve_qemu_ovmf_path`、`find_compiler_rt_archive`、grub 目录探测、`grub_dir_arg` 绝对路径 | 工具身份/固件路径不能靠 PATH 猜 | §6.2（doctor 校验 triple/链接器/compiler-rt） |
| `CONFIG_RPR_BASE_URL` 等"配置值进入内容"的 target（`esp:rpr-client`）| 输入是配置派生文本 | §7 配置转换工具 |
| `build_roots()` 把 `build-info` 强塞进每次运行 + `BUILD_NUMBER_EXEMPT_TARGETS` 白名单 | 版本自增污染增量的根源 | §7（普通 build 不递增、SOURCE_DATE_EPOCH 或提交时间）→ **可以整体删除该机制** |
| 环境读取（`CC/CXX/AR/LD/OBJCOPY/RUSTC/LEONOS_*`）在构图期求值 | 未声明环境不得改变工具选择 | §6.2 |
| `run_client` 后台 worker + `status`/`log`（`log` 起 vim） | spec §3 非目标 | **删除**（不迁） |
| TUI（`buildsystem/core/tui.py`，1342 行）、`settings` curses 编辑器、`map` | spec §3 非目标 | **删除**（不迁） |
| 4 种日志主题的字符串伪装 | spec §3/§11 | **删除** |
| 61 项 `BUILD_NUMBER_EXEMPT_TARGETS` | 随 build-info 机制一起消失 | §7 |

### 9.3 属于"不要重新实现"的非目标（spec §3 直接命中）

依赖图引擎、线程池调度器、内容寻址构建缓存、后台 daemon、任务数据库、远程执行平台、插件框架、随机 9 位任务 ID、进度条/主题拦截输出、curses UI —— 上述在旧系统分别对应 `BuildGraph`/`_schedule`/`cache/apk`(保留但降级为下载缓存)/`run_client`/`TaskStore`/—/`Target.action` 闭包/`TaskStore.new_id`/`ProgressRenderer`+`LOG_THEMES`/`tui.py`+`ui.py`。

---

## 10. 风险清单（按"卡住可行性"排序）

1. **Meson/Ninja/Python 三处在生产链上，且不是"一个脚本"而是结构性依赖**：
   (a) `tools/build_auth_upstream.py:156-164` 对 **linux-pam** 走 `meson setup/compile/install`（Meson 本体是 Python，生成 `build.ninja` 由 Ninja 执行）；`auth-upstream` 又是 `storage-upstream`、`esp:auth`、`sdk`、`apk-root`、全部镜像的上游。
   (b) `tools/leonos_musl_cc.py` **原样**作为 `bin/leonos-musl-cc` 打进 SDK（`package_devtools.py:407`），并被 `musl-probe:*`(1529) 与 `musl-ltp`(1561) 当作链接器直接 exec（argv[0] = `python3`）。
   (c) `musl_link.py` / `storage_tools.py` / `package_fastfetch.py` 被 `import` 进构图。
   → spec §2 A16 的 execve 追踪在 `installer` 一条链上就会同时命中 Meson、Ninja、Python。**必须先给出 linux-pam 的 autotools/Makefile 替代或等价适配**，否则整个计划的验收前提不成立。
2. **构建污染受跟踪源码树**：`include/generated/build_info.h`（`build-info`，`always`）、`Kconfig.components`（`config-sync`，`always`）、`buildsystem/state/build_number.txt`（已用 `!` 反向豁免）、`LeonOS4-Developer-SDK.zip` 落在源码根、`tools/package_devtools.py` 会 stage 进 `devtools/`。本任务开始时工作区曾出现 `buildsystem/state/build_number.txt` 与 `include/generated/build_info.h` 被改动（spec §2 已预警，实为并发构建进程所为）→ A17（"构建前后受跟踪文件无新增变化"）与 spec §7"版本头移至 O/generated、移除源码目录自动写入"是**硬冲突需要一并解决**。
3. **prune 是镜像正确性的唯一防线，Make 没有等价物**：`staging-prune`（100 行 action + 硬编码 obsolete 清单 + 组件差集）与 `esp:layout-links`（读上次 `layout-links.json` 删除消失的符号链接，并与 BusyBox applet 链接、已打包命令优先级三方仲裁）。删掉一个组件 / 删一个源文件 / 改名一个应用，靠"时间戳重建"完全不会让 `build/esp` 变干净，最终 ext2/ISO 会残留旧 ELF。A06 只能通过**新的成员清单 + 从空 staging 原子发布**达成，这是 P3 最大工作量集中点。
4. **增量正确性建立在"mtime + 只哈希声明的指纹"上，且没有任何构建级互斥锁**：
   - `_fingerprint` 不含文件内容、不含被调用 Python 脚本的内容（脚本内容只作为独立 input 文件被 mtime 跟踪）、不含 `find_compiler_rt_archive` 探测结果、不含 4 个环境变量。
   - `archive:zlib` / `archive:libpng` / `archive:stardustui` 用 `ar rcs`（追加）而非 `rebuild_archive.py`，**当前就会残留已删成员** —— 基线抓取时必须记录这一已知缺陷，避免把它当"正确行为"照搬到 Make。
   - 两个 `build.py` 进程可无保护并发：`targets.json` 单文件整表重写 → 后者覆盖前者的状态；`build/obj`、`build/esp` 共享。spec §6.3 的锁是**净新增能力**，不是"保留旧行为"。
5. **多输出/条件输出/未声明输出遍地**（`image-vmdk` 4、`musl-ltp` 22、`app-icons` 62、`app-manifests` 67、`esp:layout-links` 200+、`musl` 实际 12+ 而真实 sysroot 上千；`release`/`esp:terminal-packages`/`image-iso` 的输出集合随配置或环境变量而变）。用单个 `.stamp` 表达会在 A07 立刻失败；`&:` grouped targets 与"按成员清单校验"必须成对设计。
6. **聚合面的语义与 spec §4 不同**：旧 `all` = config-sync+build-info+loader+kernel+kerneldebug-module+drivers+middlelayer+userland+**sdk**+esp，**不含任何镜像**；`installer` = all+installer-root+installer-image；`esp`/`image-vmdk`/`apk-root` 都不在 help 列表里。新 `all` 要求含 `rootfs`/`apk-repo`/`image-vmdk`/`iso`/`installer` → 这不是重命名而是**改变聚合契约**，CI 与 `.vscode/tasks.json` 三条链都要同步。
7. **宿主工具事实表不完整**：`task_tools()` 未登记 `curl/git/openssl/patch/fakeroot/unshare/e2fsck/readelf/llvm-ranlib/grub-mkimage/llvm-objcopy/vim`，而 `musl`（git）、`auth-upstream`（curl+patch+make+meson）、`apk-root`（curl+openssl+unshare/fakeroot+e2fsck）、`image-*`（grub-mkimage、grub-mkrescue、mcopy、dd、qemu-img）都依赖它们。`make doctor` 必须从**实测调用**重建，不能照抄 §2.8 表。
8. **测试面的真实断言在 Python 里**：12+1 个 QMP 测试把"来宾串口正则 + 截图存在性 + 退出码"的判定写在 `build.py:3840-4042` 与 `tools/qmp_terminal_smoke.py`，用 `context.runner.task_id` 生成临时 socket/overlay 名。spec §11 要求测试识别来宾成功标记并核验退出状态 —— 迁移到 C/Shell 意味着**重写这套断言**，且旧断言文本（例如 `spawn path=`、`[abittest] ALL PASS`、`scheduler task exited pid=... code=0`）是内核日志格式的隐式契约。

---

## 11. 建议的验收证据来源（P0-b 对应用）

| 台账列 | 证据来源 |
| --- | --- |
| 目标存在性/kind/依赖 | `python3 build.py info <target> --json`（build.py:4927-4938，输出 name/kind/outputs/inputs/depends_on）——**注意这会触发 build-info，污染一次版本号**；或在隔离 worktree 执行 |
| 全图清单 | `python3 build.py map`（非 JSON，curses，需 TTY）→ 建议改用 `build.py info` 逐名采样，或在 §6 统计表基础上以 `--verbose` 日志的 `resolved closure (N targets): ...` 行（runner.py:637-640）作为权威闭包快照 |
| 是否重建 | `python3 build.py why <target> --json`（`explain` → `will_rebuild` + `reasons`，runner.py:1002-1010） |
| 实际执行过的 target 与其输出 | `buildsystem/state/targets.json`（fingerprint + depfile_dependencies + outputs） |
| 命令 argv 与耗时 | `buildsystem/logs/<id>.log`（`-v` 时含 `Running command:` 与 cwd/env 覆盖）、`buildsystem/state/tasks/<id>.json` 的 `metrics.timings`（`build.py profile <task> --json`，runner.py:682-707） |
| 文件清单/权限 | `build/esp` 与三类镜像的 `debugfs ls -l` / `mdir -i <esp image>`（旧系统自己就用 `debugfs -w`，见 `make_musl_checkpoint.py`） |
| 启动行为 | `build/qmp-<test>-serial.log`、`build/images/*-qmp-smoke.ppm` |
