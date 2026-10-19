# ReliefMath 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 subagent-driven-development（推荐）或 executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 新增与 ReliefExcel、ReliefWord 排版一致的中文原生公式编辑器，支持多公式文档、LaTeX 工具辅助编辑、离线实时渲染及 SVG、透明 PNG、源码导出，并交付目标系统真实运行截图。

**架构：** FLTK GUI 与不依赖 FLTK 的文档、编辑及工具核心分离。通过有界、带代次的非阻塞协议驱动独立渲染 worker，预览及两种图像导出共用同一数学布局；文档 I/O 和批量导出另由后台任务处理。首先证明候选数学后端在目标工具链、字体和许可条件下可用，再实施文档、工具、界面与系统集成。

**技术栈：** C++17、FLTK 1.3.11 原生 `gtk+` scheme、现有 SimSun/gettext、POSIX 子进程与管道、GNU Make、主机 ASan/UBSan 测试和 QEMU 图形验收。MicroTeX 固定审查提交为 `0e3707f6dafebb121d98b53c64364d16fefe481d`，它是待验证候选，不是已确认可用依赖。

**规格：** [已确认的 ReliefMath 设计](../specs/2026-10-19-reliefmath-design.md)。执行者须同时阅读规格及根目录 `AGENT.md`。本文件描述拟实现接口和验收要求，不代表这些接口已经存在。

## 全局约束

- 工作区 `/home/xiaobai/Projects/Projects/ReliefOS_Leon_WorkSpace`，当前分支 `rewrite/desktop-apps`，规格基线 `372641705ae46f7523d26fef0e4e200212842c78`。
- 本任务没有新建分支、提交、推送或发布授权；任何任务完成都只更新本计划复选框与验证记录，不运行自动 commit。保留用户文件、现存改动和无关未跟踪文件。
- 全部构建临时数据使用仓库 `out/tmp`；不提交 `out/`、`build/`、`result/`、`.zcode/` 或 `tools/dependencies.py`。不使用 `git status --untracked-files=all`。
- GUI 与核心模块遵守现行 C++17、`-fno-exceptions -fno-rtti`、无完整 STL 开发头的工具链约束；第三方 worker 的例外只能局部设置，不改全局编译标志。
- 沿用 `reliefos_fltk::init("reliefmath")` 和标准 FLTK 控件，不自创主题，不改变控件绘制、颜色或圆角；办公布局参考 ReliefExcel、ReliefWord。
- 初始窗口约 1180×780，最小约 960×600；约 34px 顶部行、约 100px 分组功能区、可调三栏、状态栏；功能区“开始”“符号”“结构”“排版”“视图”。
- 首版每文档最多 256 条公式；单条源码最多 64 KiB，文档最多 16 MiB；解析嵌套最多 128 层；一次预览或导出的尺寸最多 16,384 × 16,384 像素，栅格总像素最多 16,777,216。修改上限必须先修订规格并说明影响。
- 禁止任意 TeX 宏、宏包、文件、shell 或网络访问；未知命令必须定位报错。SVG 含中文在内的字形转为真实矢量轮廓，PNG 背景透明，不能嵌位图冒充矢量。
- 导出绑定源码与设置快照，禁止导出旧预览冒充当前公式；原子保存、外部修改检测、恢复锁、取消和逐项失败报告必须落地。
- 不修改内核、ABI golden、audio manifest、系统背景、全局 FLTK 支持或其他办公应用行为。文件管理器仅增加本应用打开入口。
- 生产入口仅根 Makefile 和现有 C/Shell 构建链，下载只经显式 `make fetch` 及锁文件；Python 只用于现有类别的非生产测试/维护工具。
- 分别报告主机测试、目标编译、镜像核对、QEMU、VMware 和真实硬件结果。VMware 验证未完成，未经实际运行不得更改此结论。截图必须来自实际目标程序。

## 实施顺序与完成证据

后端可行性 → 文档及编辑核心 → 数学渲染和协议 → 异步任务 → FLTK 界面 → 系统接入 → 目标运行和截图。每个任务执行失败测试、最小实现、通过测试及定向差异检查；依赖任务通过后才进入后续集成。


## 文件与职责

下列新增接口均为计划接口。采用命名空间 `rm`，复用现有 Office 的显式分配失败处理方式，但不改动其他应用源码。

| 路径 | 职责 |
| --- | --- |
| `userland/apps/reliefmath/storage.h`、`limits.h`、`error.h` | 不抛异常的 Text/Array/Bytes；唯一资源上限；UTF-8 字节位置错误 |
| `userland/apps/reliefmath/model.h/.cpp`、`history.h/.cpp` | 稳定公式 ID、选项、原子文档命令及全局撤销重做 |
| `userland/apps/reliefmath/document.h/.cpp`、`file_io.h/.cpp`、`recovery.h/.cpp` | 原生编解码、外部修改检测、原子写入、恢复目录和实例锁 |
| `userland/apps/reliefmath/catalog.h/.cpp`、`snippet.h/.cpp`、`editor_core.h/.cpp` | 可渲染命令清单、符号/模板、结构向导、占位、补全、查找与词法样式 |
| `userland/apps/reliefmath/render_types.h`、`protocol.h/.cpp` | 渲染快照与结果、消息分帧和流式工件传输 |
| `userland/apps/reliefmath/process.h/.cpp`、`jobs.h/.cpp`、`export.h/.cpp` | 非阻塞子进程、代次/取消、后台 I/O、批量导出及逐项结果 |
| `userland/apps/reliefmath/worker/main.cpp`、`renderer.h/.cpp` | 独立 ELF 的模式入口及 Renderer 外观，不进入 GUI 源文件通配符 |
| `userland/apps/reliefmath/worker/font.h/.cpp`、`scene.h/.cpp`、`output.h/.cpp` | 固定字体资源、字形轮廓、统一显示列表、SVG 和透明 PNG 输出 |
| `userland/apps/reliefmath/worker/backend_microtex.cpp` | 仅在可行性通过时编译的 MicroTeX 适配器，局部捕获上游异常 |
| `userland/apps/reliefmath/worker/lexer.h/.cpp`、`parser.h/.cpp`、`boxes.h/.cpp`、`backend_boxes.cpp` | 候选不满足条件时采用的受限数学盒模型；仍执行全部兼容与视觉测试 |
| `userland/apps/reliefmath/main.cpp`、`window.h/.cpp`、`ui_layout.h/.cpp` | 参数入口、原生功能区、公式列表、三栏布局和状态同步；纯布局独立测试 |
| `userland/apps/reliefmath/editor_view.h/.cpp`、`preview.h/.cpp` | FLTK 源码编辑桥接、预览、缩放、错误定位和图片剪贴板 |
| `userland/apps/reliefmath/dialogs.cpp`、`files.cpp` | 向导/符号/查找/帮助对话框及文档、恢复、导出工作流 |
| `userland/apps/reliefmath/reliefmath.desktop` | 标准 Office/Math 桌面入口和 `application/x-reliefmath` MIME 声明 |
| `tools/build/reliefmath-renderer.sh`、`reliefmath-resources-stage.sh` | 通过现有 Make 生产链构建 worker、校验及装配本应用资源 |
| `tests/build/test-reliefmath.sh`、`tests/host/test_reliefmath_*.cpp` | 可选择单套件的主机测试入口、ASan/UBSan 回归 |
| `tests/build/test-reliefmath-integration.sh`、`tests/build/test-reliefmath-backend.sh` | 组件、依赖/许可、资源 staging、生产构建和增量契约 |
| `tools/tests/reliefmath_backend_probe.cpp`、`reliefmath_guest_verify.cpp` | 目标后端探针，以及独立生成/验证来宾文档与输出的测试工具 |
| `tools/test_reliefmath_qemu.py` | 仅非生产测试：私有镜像、GUI 操作、截图、磁盘回读和证据清单 |
| `tests/fixtures/reliefmath/formulas.tsv`、`tests/fixtures/reliefmath/invalid.tsv` | 每个公开命令/环境及无效输入的固定测试语料；LaTeX 字段是实际源码，测试加载器不再解释反斜杠 |
| `docs/RELIEFMATH.md`、`docs/RELIEFMATH_COMMANDS.md`、`docs/RELIEFMATH_BACKEND.md` | 用户帮助、完整兼容表、工具链/字体/许可可行性证据 |
| `resources/licenses/reliefmath/`、`resources/reliefmath/` | 实际选用资源的来源、完整许可证及必要数学字体输入，不复制未使用资源 |

现有接入文件的修改仅限新增 ReliefMath：

- `mk/userland.mk`：GUI 目标、独立 worker 目标及其局部编译/链接/成员签名。
- `configs/dependencies.lock.json`、`tools/build/x11-development.sh`：只增加后端实际需要且已校验的来源与开发头/库；保留 GUI 原标志。
- `configs/components.toml`、`Kconfig.components`、`configs/apk-ownership.json`：Office 组件、配置与包归属。
- `mk/rootfs.mk`、`tools/build/rootfs-stage.sh`：有条件地 stage worker、字体/许可、桌面文件与 MIME 数据。
- `tools/build/installer-console-profile.sh` 及 `tests/build/test-installer-stage.sh`：控制台不携带本 GUI/私有资源。
- `userland/apps/fileman/boundary.c`、`main.cpp`、`tests/host/test_fileman_boundary.c`：`.rmath` 打开关联和真实 X11 程序路由。
- `configs/app-icons.list`、`tools/make_app_icons.py`、`resources/build-art/app-icons/`：独立应用图标及六种 hicolor 尺寸，生产仍消费既有资源链。
- `configs/nls/messages.c`、`configs/nls/po/leonos.pot`、`configs/nls/po/zh_CN.po`：新增真实界面文案及中文翻译。
- `tests/build/test-components.sh`、`test-components-metadata.sh`、`test-apk-ownership.sh`、`test-fileman-boundary.sh`、`test-app-icons.sh`：扩展已有契约；不以修改旧期望掩盖无关失败。

## 计划接口与测试入口

任务 2 建立 Text/Array/Bytes、Error 与模型类型，任务 5 在 `render_types.h` 建立其余渲染类型；后续任务使用相同名称和字节位置约定。`Text` 是有界、引用计数 UTF-8 文本，`Bytes` 是不带 NUL 语义的二进制缓冲区，`Array<T>` 为显式返回分配失败的移动容器。`Error` 保存 `Code code`、`size_t begin/end` 和 `Text message`，位置使用源码 UTF-8 字节区间，UI 再转换为行列/字符数。`Code` 至少定义 None/InvalidUtf8/Limit/Format/Version/Io/Conflict/Cancelled/Timeout/Protocol/Unsupported/NoMemory/Font/Parse。

```cpp
namespace rm {
using FormulaId = uint64_t;
struct Style {
    uint32_t rgba = 0x000000ff; // RRGGBBAA
    uint16_t point_size = 24;
    uint16_t padding = 12;
    bool display = true;
};
struct Formula {
    FormulaId id = 0;
    Text name, source;
    Style style;
    uint64_t generation = 1;
};
struct Document {
    Text title;
    Array<Formula> formulas;
    FormulaId next_id = 1;
    uint64_t revision = 0;
};
struct Snapshot {
    uint64_t request_id = 0;
    uint64_t session_id = 0;
    Formula formula;
    double scale = 1.0;
    uint32_t dpi = 96;
    uint8_t artifacts = 1; // 1=RGBA, 2=SVG, 4=PNG
};
struct Raster {
    uint32_t width = 0, height = 0, stride = 0;
    Bytes rgba;
};
struct Rendered {
    Snapshot snapshot; // 对应验证过的待处理请求，不从未验证的 worker 字节猜测
    FormulaId formula_id = 0;
    uint64_t request_id = 0, generation = 0;
    Bytes svg, png;
    Raster raster;
    double baseline = 0;
};
bool validate_document(const Document &, Error &);
bool encode_document(const Document &, Bytes &, Error &);
bool decode_document(const uint8_t *, size_t, Document &, Error &);
bool same_snapshot(const Snapshot &, const Snapshot &);
bool valid_raster_extent(uint32_t width, uint32_t height);
}
```

`same_snapshot` 比较 session_id、公式 ID、代次、源码、Style、缩放及 DPI，而非只比较数字代次；请求 ID 和所需工件位掩码分别验证。请求 ID 在进程生命周期内单调递增，重新打开文档递增 session_id，防止 ID 重用使旧响应落入新文档。`Rendered.snapshot` 保留请求的源文本与设置，任务 7 可据此核对实际导出对象。两份 Bytes 不默认同时填充：预览只请求 RGBA，SVG/PNG 导出只请求相应工件。

`tests/build/test-reliefmath.sh` 的约定：

```sh
mkdir -p out/tmp
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath.sh model
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath.sh document
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath.sh snippets
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath.sh renderer
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath.sh protocol
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath.sh process
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath.sh jobs
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath.sh export
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath.sh ui
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath.sh
```

每个套件编译对应 `tests/host/test_reliefmath_<suite>.cpp`；非 FLTK 套件不引入 X11，GUI 适配测试只在 UI 套件链接 FLTK。入口给测试程序传入专有工作目录和已校验字体资源目录，renderer 测试以 `argv[2]` 作为 `test_resource_root`；不依赖宿主随机安装的字体。默认主机参数 `-std=c++17 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -fsanitize=address,undefined -fno-sanitize-recover=all -g`。第三方适配器单独编译到 worker 对象，所需异常设置仅限该对象集合。测试入口不得把编译失败或缺少资源打印为通过。

### 任务 1：证明目标渲染后端、字体与资源链可用

**交付物：** 可在目标工具链编译的最小数学渲染探针、可复现构建路径、许可证清单及明确后端结论，尚非完整编辑器。

**文件：** 创建 `tools/tests/reliefmath_backend_probe.cpp`、`tests/build/test-reliefmath-backend.sh`、`tools/build/reliefmath-renderer.sh`、`docs/RELIEFMATH_BACKEND.md`、`resources/licenses/reliefmath/README.md`；按实际选择修改 `mk/userland.mk`、`configs/dependencies.lock.json` 和 `tools/build/x11-development.sh`。

- [ ] **步骤 1：写后端探针的失败验收。** 探针用如下固定公式生成 SVG 和 RGBA/PNG：`x^2+\frac{1}{y}`、`\sqrt[3]{a+b}`、`\int_0^1 x\,dx`、`\begin{pmatrix}a&b\\c&d\end{pmatrix}`、`\text{中文公式} + \alpha`；另外拒绝 `\input{secret}`。测试脚本接受 `--host`、`--target`、`--check-evidence`，无参数默认只运行主机探针；缺失当前模式所需输出/目标 ELF/许可文件均非零退出。探针使用单独的小型输入/输出封装，不提前依赖任务 2/5 的公共类型，后者建立后将探针切到相同 Renderer。先运行：

  ```sh
  TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath-backend.sh --host
  ```

  预期失败：探针/后端未建立；失败不得来自错误解释器或无权限目录。

- [ ] **步骤 2：固定并检查候选。** 以规格 SHA 取得 MicroTeX 的实际源码，新增依赖时把来源归档、实际 SHA256、版本、许可路径加入锁文件，显式 `make fetch` 取得缓存；后续构建禁网。检查真实编译标准、异常/RTTI、文件系统和图形适配 API。不能拿宿主 glibc 产物链接 musl，也不能手工从网络绕过锁文件建立生产依赖。

- [ ] **步骤 3：做最小适配，不引入完整 GTK 应用。** 用上游公开绘图接口收集数学布局到统一 Scene；通过实际可交叉编译的 C 字体/绘图接口输出轮廓和透明栅格。GUI/核心继续无异常；若 worker 需要 C++20 或异常，写入该目标的局部编译签名。新增开发头必须与锁定的 runtime ABI 匹配。候选字体逐项记录文件、来源、版权、许可证及是否嵌入/轮廓输出；中文使用已有系统字体，不将字体位图描边伪装成轮廓。

- [ ] **步骤 4：执行明确的可行性判定。** 必须同时满足主机渲染、目标 ELF 链接、依赖在镜像可提供、矩阵/积分排版、中文真实轮廓、透明 PNG、错误定位及字体可重分发。若候选有任何未解决项，在 `docs/RELIEFMATH_BACKEND.md` 写入编译错误/缺失能力和证据，删除未消费的试验依赖，直接选用规格已允许的本地盒模型；仍须让同一探针通过，不能减少规格语法或更换成网页 UI。通过任务 5 扩展完整语法后才能视为功能完成。

- [ ] **步骤 5：跑主机及目标证据检查。** 在根 Make 增加 `reliefmath-backend-probe` 目标，只构建测试探针和依赖，不将测试入口 stage 入正式镜像：

  ```sh
  TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath-backend.sh --host
  TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug -j4 reliefmath-backend-probe
  TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath-backend.sh --target
  TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath-backend.sh --check-evidence
  git diff --check
  ```

  `--target` 使用 `readelf` 检查目标架构、动态解释器、实际 NEEDED 链和目标资源，不在宿主机直接运行目标探针冒充来宾验证。记录命令/退出码；真正目标绘图仍在任务 12 的 QEMU 中验证。

### 任务 2：多公式模型、原子命令和撤销重做

**文件：** 创建 `storage.h`、`limits.h`、`error.h`、`model.h/.cpp`、`history.h/.cpp`（均位于 `userland/apps/reliefmath/`），`tests/host/test_reliefmath_model.cpp` 和主机测试入口。

- [ ] **步骤 1：先写稳定 ID、失败原子性及边界测试。** `model.h` 公开 `add_formula(Document &, const char *name, const char *source, FormulaId &, Error &)`、`clone_document(const Document &, Document &, Error &)` 和 `move_formula(Document &, FormulaId, size_t index, Error &)`；`History` 提供 `begin(doc,error)`、`commit(doc,error)`、`undo(doc,error)`、`redo(doc,error)`。首次 begin 复制引用计数快照，失败不改原文档；commit 只记录真正修改。

  ```cpp
  rm::Document d; rm::Error e; rm::FormulaId a = 0, b = 0;
  assert(rm::add_formula(d, "分数", "\\frac{1}{2}", a, e));
  assert(rm::add_formula(d, "积分", "\\int_0^1 x\\,dx", b, e));
  rm::History h;
  assert(h.begin(d, e));
  assert(rm::move_formula(d, b, 0, e));
  assert(h.commit(d, e));
  assert(d.formulas[0].id == b);
  assert(h.undo(d, e) && d.formulas[0].id == a);
  assert(h.redo(d, e) && d.formulas[0].id == b);
  assert(!rm::valid_raster_extent(16384, 16384));
  assert(rm::valid_raster_extent(16384, 1024));
  ```

  增加空文档、256/257 个公式、64 KiB/超长源码、无效 UTF-8、非法颜色/字号、分配失败、ID 溢出及历史分支测试。生成 ID 单调，复制公式得到新 ID，删除/重排不能让其他 ID 变化。

- [ ] **步骤 2：运行 `sh tests/build/test-reliefmath.sh model`，确认因模型缺失失败。**
- [ ] **步骤 3：实现最少模型和事务。** 源码/样式改变递增公式 generation 和文档 revision；纯视图缩放不污染文档历史。公式增删、改名、复制、排序、源码与样式均通过相同事务接口；重做/撤销也重新分配运行时代次，不能复用已发送渲染代次。以候选文档提交事务，分配失败时自动回退，不能让失败的 commit 留下半次修改。清理历史不释放仍被异步快照持有的文本。
- [ ] **步骤 4：同一命令在 ASan/UBSan 下通过。** 验证连续撤销/重做、保存节点撤销后再编辑的 dirty 状态，以及模拟分配失败前后所有公式 ID、顺序、名称、源码和 Style 不变；`git diff --check`。审查时重点核对所有权与 generation 规则。

### 任务 3：原生格式、原子保存、冲突与恢复

**文件：** 创建 `document.h/.cpp`、`file_io.h/.cpp`、`recovery.h/.cpp`、`tests/host/test_reliefmath_document.cpp`；补充 `docs/RELIEFMATH.md` 的格式/恢复章节。

- [ ] **步骤 1：编写往返及拒绝坏文件的测试。** `.rmath` v1 使用固定小端字段而非内存结构体：8 字节 `RMATH\r\n\0`、`u32 version=1`、`u32 total_bytes`、`u32 formula_count`、`u32 title_bytes`、`u64 next_id`、UTF-8 标题，随后每条记录为 `u64 id`、`u32 name_bytes`、`u32 source_bytes`、`u32 rgba`、`u16 point_size`、`u16 padding`、`u8 display`、7 个零保留字节及名称/源码。revision、generation、缓存和路径不落盘。总长度必须精确匹配，无尾随垃圾、重复 ID、非零保留位或计数乘法溢出。

  ```cpp
  rm::Document original, loaded; rm::Bytes bytes;
  rm::Error error; rm::FormulaId id = 0;
  assert(rm::add_formula(original, "中文", "\\text{面积}=\\pi r^2", id, error));
  assert(rm::encode_document(original, bytes, error));
  assert(rm::decode_document(bytes.data(), bytes.size(), loaded, error));
  assert(loaded.formulas[0].id == id);
  assert(loaded.formulas[0].source == original.formulas[0].source);
  assert(!rm::decode_document(bytes.data(), bytes.size()-1, loaded, error));
  assert(loaded.formulas[0].source == original.formulas[0].source);
  ```

  对每个截断位置、未知版本、16 MiB 边界、非法 UTF-8、记录数不符、路径含空格执行测试；加载失败保留旧 Document。

- [ ] **步骤 2：运行 `sh tests/build/test-reliefmath.sh document` 确认失败。**
- [ ] **步骤 3：实现编解码及文件事务。** `read_document(path,doc,stamp,error)` 读到临时对象后提交；`atomic_save(path,doc,expected_stamp,new_stamp,error)` 在同目录 `O_CREAT|O_EXCL` 建临时文件，处理 EINTR/短写、文件 fsync、冲突重查、rename 和目录 fsync。Stamp 由目标支持的 stat 字段及内容摘要构成，文件读取与摘要在后台完成。默认拒绝符号链接和不安全临时文件，覆盖必须由 GUI 确认；冲突返回专门错误。rename 前失败保留原文件，rename 后目录同步失败报告“已替换但持久化未确认”，不能伪称原文件未改变。文档说明不配合锁的外部写入者仍存在最终检查与 rename 之间的竞态，不宣称跨程序 compare-and-swap。
- [ ] **步骤 4：实现恢复及锁。** `RecoveryStore::open(directory,error)`、`save(doc,error)`、`list(error)`、`discard(id,error)` 管理每实例锁及独立恢复文件；路径遵守 XDG 规则，目录只允许当前用户，不能跟随链接。活实例的恢复副本不可被另一实例接管；崩溃后可发现。原文路径仅作提示，不自动覆盖；取消恢复不丢弃副本。
- [ ] **步骤 5：注入写入、fsync、rename、目录同步失败并运行 suite。** 验证外部变更、两实例、崩溃遗留、重复恢复、缺 HOME、空间不足、权限拒绝；没有通过静默 fallback 绕过失败。主机此处只验证恢复存储，真实 60 秒计时留任务 12。

### 任务 4：符号、结构向导、补全与编辑核心

**文件：** 创建 `catalog.h/.cpp`、`snippet.h/.cpp`、`editor_core.h/.cpp`、`tests/host/test_reliefmath_snippets.cpp`、两份 `tests/fixtures/reliefmath/*.tsv` 及 `docs/RELIEFMATH_COMMANDS.md`。

- [ ] **步骤 1：先写工具与 UTF-8 字节区间测试。** 定义 `Range{size_t begin,end;}`、`Snippet{Text source; Array<Range> stops;}`、`Tool` 枚举、`ToolArgs{uint16_t rows,columns; bool numbered_root; Text selected;}`；`build_snippet(Tool,const ToolArgs &,Snippet &,Error &)` 输出合法源码与非空可访问占位文本，Tab/Shift-Tab 选中它们。默认分数代码和区间约定为：

  ```cpp
  rm::Snippet s; rm::Error e; rm::ToolArgs args{};
  assert(rm::build_snippet(rm::Tool::Fraction, args, s, e));
  assert(s.source == "\\frac{a}{b}");
  assert(s.stops.size() == 2);
  assert(s.stops[0].begin == 6 && s.stops[0].end == 7);
  assert(s.stops[1].begin == 9 && s.stops[1].end == 10);
  ```

  测试所选中文包裹、矩阵各格、嵌套结构插入、占位编辑后的位移、跨占位删除失效、撤销恢复、补全前缀 `\sq`、UTF-8 查找替换和括号匹配。替换不得从中文码点中间切开。

- [ ] **步骤 2：运行 `sh tests/build/test-reliefmath.sh snippets` 确认失败。**
- [ ] **步骤 3：建立统一可渲染目录与工具。** 每项含稳定键、英文 msgid、类别、命令、插入片段、参数规则、帮助例子；模板与收藏保存为用户数据，不添加未实现命令按钮。工具完整覆盖规格的希腊字母、关系、二元运算、箭头、集合、括号、间距，以及分数、根式、上下标、伸缩定界符、求和、乘积、积分、极限、函数、矩阵、cases、aligned、重音、绝对值、范数和数学字体。每项输出必须加入有效语料并经过任务 5 真实渲染。
- [ ] **步骤 4：实现编辑纯核心。** 词法样式区分命令/分组/注释/错误；`find_text(source,needle,start,Range &,error)`、`replace_text`、`matching_bracket` 和 `complete_command` 均使用 UTF-8 边界。占位 Tab 优先于编辑器默认 Tab，列表为空后归还标准焦点导航；普通输入和 IME 提交合并成可预测的撤销单元。
- [ ] **步骤 5：运行 snippets suite 并核对兼容表。** 表格每项标明命令/参数、环境、是否支持、限制和例子；明确拒绝宏定义、宏包、外部资源及未知环境。收藏持久化失败可提示重试，不影响文档或内置工具；检查 `git diff --check`。

### 任务 5：实现完整数学子集与统一矢量/栅格渲染

**文件：** 创建 `render_types.h`、`worker/renderer.h/.cpp`、`font.h/.cpp`、`scene.h/.cpp`、`output.h/.cpp`、选定后端源文件、`tests/host/test_reliefmath_renderer.cpp`；扩展公式语料和后端文档。

- [ ] **步骤 1：固定 Renderer 契约并编写失败测试。** `Renderer::initialize(resource_root,error)` 加载只读字体；`render(snapshot,result,error)` 每次从新状态解析，错误携带源码区间，成功结果绑定输入快照。只请求实际需要的工件，预览不必生成 SVG；所有工件仍来自同一 Scene。

  ```cpp
  rm::Error e; rm::Rendered output; rm::Snapshot input;
  input.formula.id = 17; input.formula.generation = 3;
  assert(input.formula.source.set("\\frac{1}{\\sqrt{x}}+\\text{中文}"));
  rm::Renderer renderer;
  const char *test_resource_root = argv[2]; // 由 suite 校验并传入固定字体目录
  assert(renderer.initialize(test_resource_root, e));
  assert(renderer.render(input, output, e));
  assert(output.formula_id == 17 && output.generation == 3);
  assert(output.raster.width > 0 && output.raster.height > 0);
  assert(output.raster.stride == output.raster.width * 4);
  assert(rm::valid_raster_extent(output.raster.width, output.raster.height));
  assert(input.formula.source.set("\\unknown{x}"));
  assert(!renderer.render(input, output, e));
  assert(e.begin == 0 && e.end > e.begin);
  ```

  测试语料应包含组合而不只单命令：`\sum_{k=1}^{n}\frac{k}{n}`、`\lim_{x\to0}\frac{\sin x}{x}`、多重/闭合积分、带根指数、深层上下标、伸缩括号、`matrix/pmatrix/bmatrix/vmatrix/Vmatrix`、`cases`、`aligned`、`gathered`、中文 `\text{}`、`mathrm/mathbf/mathit/mathbb/mathcal`、向量/帽子/横线。命令表与测试语料一一覆盖。

- [ ] **步骤 2：运行 `sh tests/build/test-reliefmath.sh renderer`，确认未实现能力会失败。** 测试断言分数横线及上下布局、脚标基线方向、根号覆盖、矩阵行列对齐、伸缩定界符高度和中文非缺字轮廓；不只断言输出文件非空。
- [ ] **步骤 3：接入任务 1 的后端。** 候选可用时做公共子集验证及 Scene 适配，不开启任意上游宏/文件能力；候选不可用时采用递归下降受限 parser 和盒模型，分别建立水平序列、脚标、分数、根式、定界符、上下限、矩阵/对齐、文字及重音布局。解析检查深度 128、输入 64 KiB、整数/浮点/分配溢出，所有循环有输入推进。运行时间受 worker 超时控制，不能在 UI 内执行该模块。
- [ ] **步骤 4：实现真实字形轮廓与共用 Scene。** Scene 是有界 path、stroke、fill 和变换列表；数学字体与中文字符经可核验字体映射得到曲线，中文不是嵌入图片或依赖接收端字体的 `<text>`。将 `FT_Outline_Decompose` 得到的轮廓和数学布局画线转换为相同坐标系，记录基线/包围盒。字体文件不存在、缺字或损坏必须明确失败，不能把 tofu 方框当成功。
- [ ] **步骤 5：输出安全 SVG 和 RGBA PNG。** SVG 自己按 allowlist 写 `svg/g/path` 与有限数字属性，无 DTD、实体、image、script、use/href、事件或外链。PNG 用相同 Scene 绘制，保持透明 alpha；导出倍率/DPI与 point_size 明确换算，检查缩放后长宽及像素乘积再分配。尺寸从结果测量而非硬编码；背景棋盘格只由预览 UI 绘制，不进入导出像素。
- [ ] **步骤 6：运行 renderer suite 和独立文件检查。** 解析生成 XML 检查真实 path、viewBox、有限坐标及禁用节点；独立 PNG 解码检查 alpha 有透明/非透明像素和尺寸。嵌套 128 成功、129 拒绝，超长/非法 UTF-8/未配对分组/未知环境定位报错；边长允许但总像素超限也拒绝。视觉基准固定字体与测试缩放，不能在失败时直接更新基准图。记录后端来源/版本及实际资源许可证。

### 任务 6：流式协议和非阻塞 worker 生命周期

**文件：** 创建 `protocol.h/.cpp`、`process.h/.cpp`、`worker/main.cpp`、`tests/host/test_reliefmath_protocol.cpp`、`tests/host/test_reliefmath_process.cpp`，完善独立 worker 构建。

- [ ] **步骤 1：写协议分片与身份测试。** 40 字节小端头：magic `RMIP`、`u16 version`、`u16 kind`、`u32 payload_bytes`、`u32 flags`、`u64 request_id`、`u64 formula_id`、`u64 generation`。`Kind` 定义 Begin/SourceChunk/End/ArtifactBegin/ArtifactChunk/ArtifactEnd/Done/Failure/Cancel；`Frame` 含上述身份、`Kind kind`、`uint32_t flags` 和 `Bytes payload`，默认协议版本为 1。`encode_frame(const Frame &,Bytes &,Error &)` 显式编码；`Decoder::feed(const uint8_t *,size_t,Error &)` 有界积累、`take(Frame &)` 取完整帧。单帧 payload 最多 65,536 字节；完整 64 KiB 源码经 Begin/SourceChunk/End 传输，不能因头部/样式挤占源码上限。Begin payload 包含 session_id、Style、scale、DPI、工件掩码和源码长度；ArtifactBegin/Chunk/End 流式传 PNG/SVG/RGBA，先核验输出声明和缓冲容量。GUI 只根据自己保存的 pending Snapshot 回填结果身份。

  ```cpp
  rm::Frame original; rm::Bytes wire; rm::Error e;
  original.kind = rm::Kind::Cancel;
  original.request_id = 42; original.formula_id = 7; original.generation = 9;
  assert(rm::encode_frame(original, wire, e));
  rm::Decoder decoder;
  for (size_t n = 0; n < wire.size(); ++n)
      assert(decoder.feed(wire.data()+n, 1, e));
  rm::Frame received;
  assert(decoder.take(received));
  assert(received.request_id == 42 && received.generation == 9);
  ```

  覆盖半个头、短读写、多帧合并、错误版本、过长长度、未知 kind/flags、未完成工件、请求 ID 不匹配和 EOF。未验证长度不能先分配大块内存。

- [ ] **步骤 2：运行 protocol/process suite 确认失败。** process 测试用自己创建的假 worker 模式模拟慢读、管道背压、crash、忽略取消和乱序，不干扰用户进程。
- [ ] **步骤 3：实现固定路径 exec 与非阻塞泵。** GUI exec `/usr/lib/reliefos/apps/reliefmath/reliefmath-render.elf --render-worker`，不用 shell。stdin/stdout 仅协议，诊断在 stderr，父子关闭无关 fd；图像 bytes 不进入日志。使用 fcntl 非阻塞、poll 和 waitpid(WNOHANG)，GUI 用 FLTK fd/timer 通知，每轮限制工作量。errno/EINTR/EAGAIN/EPIPE/子进程退出分别处理。协议队列和预览图像缓存只保留当前请求/最新候选与上次成功图像；不为 256 个公式保留最大 RGBA。批量请求顺序执行并流式写入，避免结果累积使内存无界。
- [ ] **步骤 4：实现取消、限时及重启。** 渲染默认超时 10 秒，取消或超时先中止当前 worker，回收后按需启动下一请求，避免单线程渲染器不能读取 Cancel 而卡住；只信任最终完整帧。连续 30 秒内最多自动重启 2 次，超过后提供明确“重试”，不无限循环。目标支持的进程资源限制可附加使用，不能假定缺失 syscall 可用或为此改内核；输入/布局/输出本身仍有界且不访问任意外部资源。
- [ ] **步骤 5：运行两个 suite。** 验证 64 KiB 源码可完整传输、最大合法图片分块收取、取消后无残留 owned child、crash 后可继续新请求、请求切换丢弃旧代次、SIGPIPE 不终止 GUI、退出时回收。`git diff --check`。


### 任务 7：后台文档任务、批量导出与快照一致性

**文件：** 创建 `jobs.h/.cpp`、`export.h/.cpp`、`tests/host/test_reliefmath_jobs.cpp`、`tests/host/test_reliefmath_export.cpp`；扩展 `worker/main.cpp`、协议及 I/O 模块。`--io-worker` 与 `--render-worker` 是同一独立 ELF 的不同固定模式，图像布局不在 GUI 线程运行。

- [ ] **步骤 1：写导出计划与快照匹配失败测试。** `ExportFormat{Svg,Png,Latex}`、`ExportItem{Snapshot snapshot; Text basename; ExportFormat format;}`、`ExportPlan{Text directory; Array<ExportItem> items;}`；`make_export_plan(doc,ids,format,directory,plan,error)` 捕获开始时文本/样式快照并预先检查全部文件名。`export_snapshot(item,rendered,directory,error)` 核对请求、公式、generation 和完整快照，源码导出直接使用快照，渲染失败不得借用预览缓存。

  ```cpp
  rm::Snapshot before, after;
  before.formula.id = 8; before.formula.generation = 4;
  assert(before.formula.source.set("x^2"));
  after = before;
  assert(after.formula.source.set("x^3"));
  assert(!rm::same_snapshot(before, after));
  after = before; after.formula.style.padding += 1;
  assert(!rm::same_snapshot(before, after));
  assert(rm::same_snapshot(before, before));
  ```

  增加导出期间编辑、删除、切文档及 undo 使代次变化的用例；旧结果仍可标识为原导出快照结果，但不能被标为当前预览。文件命名固定为 `001-公式名.svg` 等顺序编号加净化名称；重名和截断后碰撞加入稳定 ID，拒绝 `..`、路径分隔符、控制符及目录逃逸。

- [ ] **步骤 2：分别运行 jobs/export suite 确认失败。** 不用启动 FLTK 即可测状态机与 fake worker。覆写已有文件必须有明确的逐项授权记录，不能默认覆盖。
- [ ] **步骤 3：实现后台 I/O 与恢复调度。** GUI 只负责对话框、快照与进度；后台加载验证临时 Document，确认没有更新的 load/open 请求后再提交。保存捕获 revision，完成后只有当前内容仍等于保存快照才清 dirty，编辑期间保存完成不错误清除新修改。60 秒定时触发独立恢复副本，长时间任务运行时合并重复恢复请求，允许正常退出等待或取消当前任务。
- [ ] **步骤 4：实现逐项导出。** 状态为 Queued/Rendering/Writing/Succeeded/Failed/Cancelled，按公式单请求运行；SVG/PNG 渲染后以同目录独占临时文件写入，fsync 后发布，拒绝跟随最终输出符号链接。目录使用 fd 锚定并校验目标类型；新建输出采用目标已验证的无覆盖发布方式，避免“先检查再 rename”覆盖竞态。取消保留 Succeeded，不提交当前未完成临时文件，列出 Failed/Cancelled；超时、磁盘不足和单项失败不把余项标成功。
- [ ] **步骤 5：运行 suite 并验证失败原子性。** 注入短写、ENOSPC、rename失败、同名输出外部出现、链接替换及取消；保存/恢复/导出互不覆盖。导出结束摘要中的成功数与实际磁盘文件数量一致；`git diff --check`。

### 任务 8：原生 FLTK 功能区、三栏与状态流程

**文件：** 创建 `main.cpp`、`window.h/.cpp`、`ui_layout.h/.cpp`、`files.cpp`、`editor_view.h/.cpp`、`preview.h/.cpp` 的最小可运行骨架；创建 `tests/host/test_reliefmath_ui.cpp`。本任务形成真实可启动 GUI，工具详细交互在任务 9 扩展。

- [ ] **步骤 1：先写布局/命令路由测试。** `OfficeLayout::compute(width,height,list_visible,preview_visible)` 返回 menu/ribbon/list/editor/preview/status 区域；`Command` 枚举覆盖实际 UI 操作，`command_state(command,document,selection,job_state)` 控制 enabled 状态。测试 1180×780、960×600、隐藏列表、隐藏预览和拖动极限，所有必要区域在客户区内、互不覆盖，最小窗口通过滚动访问所有 ribbon 分组。

  ```cpp
  const rm::OfficeLayout p = rm::OfficeLayout::compute(960, 600, true, true);
  assert(p.menu.height == 34);
  assert(p.ribbon.height == 100);
  assert(p.editor.width > 0 && p.preview.width > 0);
  assert(p.editor.x + p.editor.width <= p.preview.x);
  assert(p.status.y + p.status.height <= 600);
  ```

  `OfficeLayout`/`Rect`/`Command` 定义在 `ui_layout.h/.cpp`，不链接 FLTK 即可测布局。`Rect` 字段为 `int x,y,width,height`；`OfficeLayout` 成员为 menu/ribbon/list/editor/preview/status，均为 Rect；`compute(int,int,bool,bool)` 是静态函数，保持测试与界面读取相同布局结果。

- [ ] **步骤 2：运行 `sh tests/build/test-reliefmath.sh ui` 确认缺失布局失败。**
- [ ] **步骤 3：实现真实控件布局。** 初始化 `reliefos_fltk::init("reliefmath")`、`LC_NUMERIC=C`，仅使用标准 `Fl_Menu_Button`、`Fl_Button`、`Fl_Choice`、`Fl_Tabs/Fl_Group`、`Fl_Scroll`、`Fl_Hold_Browser`、`Fl_Tile`、`Fl_Text_Editor` 等。顶部菜单/快捷按钮、五个选项卡、带名称的分组功能区、底部状态栏，与现有 Office 间距/对齐/分组习惯一致；窗口内部不放重复应用大标题。允许在编辑/预览画布绘制内容，禁止自绘替换标准按钮盒型/主题。
- [ ] **步骤 4：连接文档及渲染状态。** 公式列表显示名称、成功/待渲染/错误/旧预览状态；新增/复制/删除/重命名/上下移动均用模型事务。文件菜单新建、打开、保存、另存为、最近文档、关闭退出连到后台任务，关闭 dirty 时保存/放弃/取消三路真实生效。编辑后约 200ms 合并渲染，在 edit 时立即将旧预览标为旧结果，响应只提交当前会话当前快照。状态栏显示当前项/总数、UTF-8 字符数、渲染错误和缩放。
- [ ] **步骤 5：通过 UI suite 和最小目标构建。** 任务 11 补齐组件后生产命令 `make O=out/xorg-vmware-debug PROFILE=debug -j4 app-reliefmath app-reliefmath-render` 必须编译此界面。任务 8 可以先用主机 FLTK 运行布局检查，但不能称为目标系统截图。审查逐个点按可见按钮，未绑定命令不得显示为可用；`git diff --check`。

### 任务 9：源码编辑、工具向导、预览与图像剪贴板

**文件：** 完成 `editor_view.h/.cpp`、`preview.h/.cpp`、`dialogs.cpp`，扩展 `window.cpp`、`tests/host/test_reliefmath_ui.cpp` 及 `docs/RELIEFMATH.md`。

- [ ] **步骤 1：先测试 UI 适配状态和输入次序。** 适配器 `EditorBridge` 提供 `apply_tool(tool,args,error)`、`next_placeholder(reverse)`、`set_source(id,text,error)` 和 `apply_edit(begin,end,text,error)`，桥接纯核心字节区间与 Fl_Text_Buffer。测试工具插入是一条 undo、列表切换保留选择、旧缓冲 callback 不写入新公式、补全替换正确前缀、IME 预编辑不触发结构工具快捷键，以及同一输入不被 FLTK 和自有历史重复撤销。
- [ ] **步骤 2：运行 UI/snippets suite 确认失败。** GUI suite 中用可控回调模拟编辑，不通过截图像素替代模型断言；真正中文 IME 在 QEMU 验证。
- [ ] **步骤 3：完成编辑器。** Fl_Text_Buffer 绑定行号、词法样式和查找高亮；不将控制序列混入源码。括号匹配、补全候选、选区包裹、Tab/Shift-Tab 占位跳转、撤销/重做、剪切复制粘贴、全选、查找/替换与替换全部全部落地。标准非模态查找或 modal 对话框需有真实焦点/关闭逻辑。UTF-8 字符计数、行列和错误区间一致；中文输入仍走现有 X11/FLTK IME 路径。
- [ ] **步骤 4：完成符号/向导/排版工具。** 按任务 4 catalog 生成有搜索、分类与预览说明的符号面板；矩阵可选择行列及定界符，分数/根式/上下限/cases/aligned 向导根据用户输入生成可回改 LaTeX。模板和收藏提供保存、使用、删除、重名处理；字号、行内/行间、颜色与留白操作改模型 Style 并重新渲染。所有工具引用同一兼容清单，不能插入后端不认识的命令。
- [ ] **步骤 5：完成预览与复制。** 画布使用结果 RGBA 合成在预览棋盘格上，缩放/适合窗口只调整视图，导出缩放单独选择。错误面板显示“上次成功预览，当前公式有错误”，点击行列回到对应源码；无成功结果时显示错误而非空白成功。源码复制用文本通道，复制渲染图片用已在目标 FLTK/X11 实测的图片剪贴板路径，成功必须由独立目标 clipboard consumer 接收并核对像素，不能以复制文件路径代替图片。
- [ ] **步骤 6：跑 suite 并检查两种窗口尺寸。** 为五页功能区列命令清单逐项操作，检查键盘焦点/tooltip/滚动/中文长标签/打开多个对话框/窗口关闭期间 job。实测后再记录截图，不把预览草图写成验收证据；`git diff --check`。

### 任务 10：导出与恢复的完整用户流程

**文件：** 完成 `files.cpp`、`dialogs.cpp`、`recovery.h/.cpp`、`export.h/.cpp`，扩展 document/jobs/export/UI 测试与用户文档。

- [ ] **步骤 1：编写流程失败测试。** 场景为“未保存新文档→保存→编辑→导出全部 SVG→第二项失败→取消余项→关闭选择取消→重新打开”；断言 dirty、原文档、已导出文件、任务摘要各自准确。另测“加载坏文件→原文保留”“外部改文件→保存冲突”“恢复副本→另存新路径”“导出期间编辑→文件仍对应开始快照”。
- [ ] **步骤 2：运行 document/jobs/export/UI suite 确认流程缺失失败。**
- [ ] **步骤 3：实现文档与恢复对话框。** 新建/打开之前询问 dirty 状态；保存失败不可继续退出或覆盖原文。60 秒恢复只对修改文档运行，显示可恢复文件名称/时间、恢复/保留/丢弃选项；恢复内容标 dirty 且建议新路径。外部冲突提供重新加载、另存为、取消，以及明确确认后的覆盖；后台任务结果返回时再次核对当前文档会话。
- [ ] **步骤 4：实现导出对话框。** 选择当前/所选/全部、SVG/PNG/LaTeX、目录、命名预览、缩放/DPI和覆盖策略，提前列出冲突及无效名称。进度展示当前公式和已完成数，取消只取消当前及余项；失败后能查看每项路径与原因，导出成功后能打开目录/再次导出。预览 stale 或出错时重新渲染所需快照，绝不悄悄输出旧图。
- [ ] **步骤 5：跑所有相关 suite。** 使用专有临时目录做多实例恢复、外部修改、只读目录、部分失败和取消；断言未授权覆盖从未发生、所有可见结果都有准确状态。`git diff --check`。

### 任务 11：组件、构建、资源、翻译和文件关联接入

**文件：** 修改文件清单中的现有构建/组件/staging/ownership/fileman/图标/翻译文件及关联测试；创建 `reliefmath.desktop`、`tests/build/test-reliefmath-integration.sh` 和 `tools/build/reliefmath-resources-stage.sh`。若标准 MIME 数据无既有通路，只新增 `system/rootfs/usr/share/mime/packages/reliefmath.xml` 并通过现有 stage 明确按组件选择，不让 console 混入本应用资源。

- [ ] **步骤 1：先扩展失败契约。** 在现有 components/metadata 测试中要求 `reliefmath` 位于 Office、默认 Xorg 启用、open_with=true、extensions=[".rmath"]；在 fileman boundary 测试中要求大小写 `.rmath/.RMATH` 正确路由，显式用户关联优先。ownership 测试覆盖 GUI、worker、字体、许可、MIME 和图标；console fixture 要证明这些都不存在。
- [ ] **步骤 2：跑契约确认失败。**

  ```sh
  TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath-integration.sh
  TMPDIR="$PWD/out/tmp" sh tests/build/test-fileman-boundary.sh
  ```

- [ ] **步骤 3：建立生产目标与资源成员。** 增加 `APP_RELIEFMATH` 组件，GUI 加到 `USERLAND_FLTK_APPS`；worker 源位于子目录，不能被 GUI 顶层通配符误编。Make 生成 `app-reliefmath-render` → `out/xorg-vmware-debug/userland/reliefmath-render.elf`，作为 GUI/组件 staging 明确依赖；worker 不另加一个可见菜单组件。渲染与 I/O 源明确列入成员签名，移除源文件或参数变化触发 relink；资源声明完整产品，删除某个字体/许可/worker 成员必须触发重建而非仅凭 stamp。测试探针不进入镜像。
- [ ] **步骤 4：正确 stage 和包归属。** GUI 路径 `/usr/lib/reliefos/apps/reliefmath/reliefmath.elf`，worker 路径 `/usr/lib/reliefos/apps/reliefmath/reliefmath-render.elf`，只读私有数学资源 `/usr/share/reliefmath/`，许可证 `/usr/share/licenses/reliefmath/`，入口 `/usr/bin/reliefmath`。包选择必须同时保留 worker/资源与库依赖，不造成默认镜像可运行但安装包漏资源。控制台 profile 排除该应用全部专有资产；不删公共字体和其他应用资源。
- [ ] **步骤 5：加入桌面、MIME、图标和中文。** 桌面文件的 Exec 为 `/usr/bin/reliefmath %f`，Categories 为 `Office;Math;`，Icon/WM_CLASS 与 `reliefos-reliefmath` 一致，声明 `application/x-reliefmath`。文件管理器路由显式启动本 X11 程序。图标遵守现有维护工具，生成 32px BMP 与 16/32/48/64/128/256 PNG；不要以 AI 模拟截图代替图标或运行证据。msgid/gettext 域继续使用既有 `leonos`，不修与本任务无关的既有品牌缺项。
- [ ] **步骤 6：运行配置和契约验证。**

  ```sh
  TMPDIR="$PWD/out/tmp" make regen-component-kconfig
  TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug olddefconfig
  TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath-integration.sh
  TMPDIR="$PWD/out/tmp" sh tests/build/test-components.sh
  TMPDIR="$PWD/out/tmp" sh tests/build/test-components-metadata.sh
  TMPDIR="$PWD/out/tmp" sh tests/build/test-apk-ownership.sh
  TMPDIR="$PWD/out/tmp" sh tests/build/test-fileman-boundary.sh
  TMPDIR="$PWD/out/tmp" sh tests/build/test-app-icons.sh
  TMPDIR="$PWD/out/tmp" sh tests/build/test-installer-stage.sh
  desktop-file-validate userland/apps/reliefmath/reliefmath.desktop
  git diff --check
  ```

  组件工具需要编译时先通过相应 Make 目标取得，不手工伪造环境或元数据。脚本成功退出且断言确实执行才记 PASS。

### 任务 12：完整测试、镜像回读、QEMU 验收和真实截图

**文件：** 创建 `tools/test_reliefmath_qemu.py`、`tools/tests/reliefmath_guest_verify.cpp`；补齐 host suites、`docs/RELIEFMATH.md` 的实测结果与截图索引。证据目录新建在 `result/reliefmath-<本次唯一标识>/`，不覆盖现有 result，不加入 Git。

- [ ] **步骤 1：先写独立来宾验证工具和失败检查。** guest verifier 提供 `prepare DIR` 创建原生样本文档/损坏文件及测试语料；`verify DIR` 读取来宾导出的实际文件，检查原生往返/顺序/选项、SVG 的真实 path 和无外链、PNG alpha/尺寸及源码快照。QEMU 脚本启动前核验输入镜像/内核/GUI/worker 哈希；缺少任何必需截图或磁盘验证结果即失败。源镜像使用私有副本，新目录必须 `exist_ok=False`。
- [ ] **步骤 2：完成主机回归与目标构建。** 所有命令直接写日志、检查退出码，不经可能令 make 遭 SIGPIPE 的筛选管道；不要运行同时写同一 O 的两个构建。

  ```sh
  TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath.sh
  TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmath-backend.sh --check-evidence
  TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug -j4 app-reliefmath app-reliefmath-render nls
  TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug -j4 image-vmdk
  TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug test
  git diff --check
  ```

  若既有全仓门禁失败，记录真实失败及与本任务的关系；尤其不要擅改音频产物约定、ABI golden 或无关翻译/Pages 链接。定向应用门禁与全仓结果分开报告，不能写“所有测试通过”。
- [ ] **步骤 3：回读新生成镜像内容。** 只读从实际 root 分区提取 GUI/worker、字体/许可、desktop、MIME、入口和图标，对照构建成员及 SHA256；用 ELF 工具递归核对实际动态依赖均在镜像。记录新生成 VMDK 的精确路径和摘要，以及是否真的从它启动；测试私有注入镜像不能冒充该 VMDK 已启动。
- [ ] **步骤 4：编写并运行 QEMU 测试入口。** 参数契约固定 `--image RAW --kernel ELF --output NEW_DIR [--recovery]`。镜像/内核路径从本次 Make 日志与实际输出取得，不猜测旧产物路径。继承现有 FLTK/QEMU Session 和磁盘准备机制，QMP socket 位于支持 Unix socket 的磁盘目录，不能用 /tmp、tmpfs 或 /mnt/d。只停止自己启动的子进程。

  ```sh
  TMPDIR="$PWD/out/tmp" python3 tools/test_reliefmath_qemu.py \
    --image "$RELIEFMATH_TEST_IMAGE" \
    --kernel "$RELIEFMATH_TEST_KERNEL" \
    --output "$RELIEFMATH_TEST_RESULT" --recovery
  ```

  运行前将上述三变量绑定到步骤 2/3 记录的真实文件和全新目录，并把完整实际 argv 写入证据；变量不是默认路径或成功占位。
- [ ] **步骤 5：逐项验证普通用户真实 GUI。** 中文启动、默认 gtk+ 控件、1180×780 与 960×600 排版、五页功能区；分数/根式/积分/矩阵/cases/aligned 向导及 Tab 占位；中文 IME 与 `\text{中文}`；补全/查找/替换/撤销重做；列表增删改名复制排序；拖动分栏、隐藏预览/列表和缩放；多公式保存重开、坏文件保留原文、外部冲突；错误保留旧预览且标识、修改后恢复；SVG/透明 PNG/源码及批量导出、部分失败、取消；图片剪贴板由另一个来宾程序读回；长公式及 worker crash/重启期间 GUI 可点击/输入。记录 UI 响应时间和任务耗时，不通过跳过 fsync 缩短耗时。
- [ ] **步骤 6：验证真实 60 秒恢复与磁盘结果。** 在私有测试实例修改后实际等待恢复计时，终止这个测试程序而非用户进程，重新启动并恢复到新文档；回读文件内容、名称顺序、导出 alpha 和 SVG 路径，确认原始镜像摘要不变、两 ELF/资源与验收前摘要一致。日志不把 GUI 启动字符串当全部功能通过。
- [ ] **步骤 7：交付原始截图及验证分层。** 必需截图：`reliefmath-editor.png`（中文主编辑器、多公式与复杂公式）、`reliefmath-structure.png`（矩阵/结构工具与真实预览）、`reliefmath-error-recovery.png`（错误定位、旧预览标识及恢复）、`reliefmath-export.png`（实际 SVG/PNG 导出结果）。通过 QMP/目标显示截取未经合成的原图；截图索引记录平台、镜像/ELF 摘要、公式输入、对应测试阶段。最终给用户可打开路径与图像预览，分别声明主机/编译/镜像/QEMU 的实测状态；明确“VMware 验证未完成，真实硬件未验证”。
- [ ] **步骤 8：最终审查与收尾。** 按已确认规格核对所有可见功能与限制，修复重要问题后重跑受影响测试。`git status --short --untracked-files=normal` 及 `git diff --check`，精确列出本任务文件；不提交、不推送、不建分支。更新规格/用户文档状态只能根据已取得证据，不预先勾选此任务。

## 规格覆盖矩阵

| 已确认要求 | 负责实现 | 必要证明 |
| --- | --- | --- |
| Office 排版、标准 FLTK gtk+、中文、五页功能区 | 8、9、11 | UI 几何测试、真实窗口两种尺寸截图与可见控件操作 |
| 多公式稳定 ID、原子编辑、撤销/重做 | 2、8 | model suite、保存/重开与历史分支 |
| 行号/高亮/括号/补全/IME/查找替换/占位 | 4、9 | snippets/UI suite、来宾 IME 与键盘操作 |
| 符号/搜索/模板/收藏与所有结构向导 | 4、5、9 | catalog 的每项生成公式均被 Renderer 渲染 |
| 离线数学子集、未知命令/环境错误、中文公式 | 1、4、5 | 固定有效/无效语料、位置断言及中文轮廓 |
| C++17 核心、局部 worker 依赖、字体许可 | 1、5、11 | 目标编译签名、NEEDED、来源/许可证及镜像回读 |
| 代次、旧响应过滤、错误保留旧预览 | 2、6、7、8、9 | 状态测试、编辑/切文档/crash/恢复来宾操作 |
| 非阻塞、取消、超时、受控重启 | 6、7、12 | fake worker 背压/取消测试及长任务 GUI 操作 |
| 版本化 .rmath、冲突、原子保存、恢复锁/60 秒 | 3、7、10、12 | 文档注入测试、多实例及真实计时恢复 |
| SVG 真矢量轮廓、PNG 透明、共用布局 | 1、5、7 | 独立 XML/PNG 检查与目标导出文件 |
| 当前/所选/全部导出、命名、快照、取消/部分失败 | 7、10、12 | export suite、磁盘文件与逐项结果一致 |
| 源码/图片分开的剪贴板 | 9、12 | 独立来宾 consumer 验证文本与像素 |
| 固定输入/嵌套/尺寸/像素上限 | 2、3、5、6 | 每个上限及上限加一、有界分帧与内存检查 |
| 默认 Xorg/Office、关联、图标、翻译，console 排除 | 11、12 | 组件/ownership/console fixtures、镜像及文件管理器启动 |
| 原始运行截图及验证层次 | 12 | 原图、哈希、命令记录；VMware 状态明示 |

## 开始执行前与最终完成检查

- [ ] 任务 1 的后端结论有主机及目标编译证据，不以候选库 README 代替实际编译。
- [ ] 所有计划接口在所属任务定义，测试入口和任务使用的名称/字节位置约定一致；对话框无死按钮。
- [ ] 兼容表内每个命令/环境均有成功或明确拒绝测试；工具不生成不支持语法。
- [ ] SVG 含中文的字形为矢量 path，PNG 真透明，预览和导出绑定相同快照；旧图无法误导出。
- [ ] 256/64 KiB/16 MiB/128/16,384/16,777,216 各项上限未被缩小或绕过。
- [ ] 所有实际使用的资源有精确来源与许可证，所有生产下载来自锁文件和显式 fetch。
- [ ] 主机、目标 ELF、镜像回读、QEMU 操作和截图的结果分开记录，无未经运行的通过声明。
- [ ] 未自动提交、推送、创建分支，未动内核/ABI golden/audio manifest/全局主题或无关文件。

## 执行方式交接

计划准备完毕后由用户选择执行方式，不把规格确认解释为提交/推送授权：

1. **子代理驱动（推荐）：** 每项由新子代理实现，按规格及代码质量两阶段审查；用户若选择，固定 **GPT-6-Luna、Max 推理强度**。调度前先核验实际子代理工具能指定这两项，不能静默换模型或把普通新聊天冒充受控子代理。
2. **当前会话内联执行：** 使用 executing-plans，按本计划依赖实施，并在后端可行性、首个可运行 GUI、完整导出及目标验收完成时报告证据。

若实际运行环境没有符合模型配置的子代理能力，说明限制并采用用户选定的可用方式，不未经授权以其他模型委派。两种方式的交付和验收标准相同。
