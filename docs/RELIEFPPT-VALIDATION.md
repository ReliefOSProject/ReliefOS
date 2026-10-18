# ReliefPPT 首版验证与格式

日期：2026-10-18。工作分支：`rewrite/desktop-apps`；基线提交：`316f0ffd843ba54d1f4197ef448796babb623e4b`。使用说明见 [RELIEFPPT.md](RELIEFPPT.md)。

## RPPT v1 文件布局

所有整数及 IEEE-754 binary32 浮点数均显式编码为小端；不直接写入 C++ 结构体。字符串使用 `u32` 字节长度加 UTF-8 内容，不含结尾 NUL。读取拒绝非法 UTF-8、内嵌 NUL、截断、尾随数据、未知枚举、非有限坐标及越界的交叉引用。

16 字节文件头：

| 偏移 | 类型 | 内容 |
| ---: | --- | --- |
| 0 | 4 字节 | ASCII `RPPT` |
| 4 | u16 | 版本，必须为 1 |
| 6 | u16 | 标志，必须为 0 |
| 8 | u64 | 载荷长度，必须等于文件长度减 16 |

载荷按以下顺序排列：

1. 页面宽、高（各 `f32`），下一个对象 ID（`u32`），文档修订号（`u64`），标题、作者（字符串）。
2. 图片数（`u32`）；每张图片为宽、高、RGBA 字节数（各 `u32`）及像素数据。像素按行排列，每像素为红、绿、蓝、alpha 四个字节；字节数必须等于 `width * height * 4`。
3. 幻灯片数（`u32`）；每张幻灯片为背景 RGB（`u32`）、备注（字符串）、对象数（`u32`），再按从底到顶的顺序存储对象。
4. 每个对象的固定字段为 ID（`u32`），种类、字体族、样式标志、对齐方式（各 `u8`），x/y/宽/高（各 `f32`），文字/填充/边框 RGB（各 `u32`），字号/线宽（各 `f32`），图片索引（`u32`）；随后存储文字（字符串）。固定字段共 48 字节，不含文字长度字段。

种类 0–5 分别为文字、矩形、椭圆、直线、箭头、图片；字体族 0–2 分别为宋体、无衬线、等宽；对齐 0–2 分别为左、中、右。样式标志位 0–5 依次为粗体、斜体、下划线、项目符号、填充、边框，保留位 6–7 必须为 0。RGB 为 `0x00RRGGBB`。非图片对象的图片索引为 `UINT32_MAX`；图片索引指向同文件的图片表。对象 ID 在全文唯一。

解码先建立并完整校验候选文档，成功后才替换输出。文件、计数、UTF-8、图片尺寸及总 RGBA 长度均在分配前检查；最终使用共享模型校验器复核几何、ID 和引用。

## 保存和恢复边界

文件冲突戳记录设备、inode、大小、mtime 与 ctime（含纳秒）。读取使用不跟随符号链接的文件描述符，比较读取前后的元数据。保存使用同目录独占临时文件、0600 权限、完整写入、文件同步、提交和父目录同步；常规失败会清理临时文件并保留目标，已有文件保留独立回滚副本。

冲突检测为乐观检查：它检测检查时已经发生的外部修改。POSIX `renameat` 不提供以 inode/时间戳为条件的比较交换，非合作写者仍可能在最后检查与提交之间竞争。目录同步失败会尝试回滚；若存储同时拒绝回滚，程序报告错误并保留隐藏的 `.rppt-old-*` 副本供恢复，不能承诺在持续 I/O 故障下目标一定不变。

恢复目录为 `$XDG_STATE_HOME/reliefppt/recovery`，未配置绝对 XDG 路径时使用 `$HOME/.local/state/reliefppt/recovery`。应用和恢复目录叶节点要求当前用户所有，权限为 0700；恢复文件及锁为 0600。唯一会话路径配合整个活跃会话期间持有的 sidecar `flock`。定时器每 60 秒捕获未保存的文档，包括仍在输入框中的文字及备注；恢复后要求另存为新文档。扫描时锁定并完整解码候选，按 mtime、ctime（均含纳秒）及路径择取最新有效副本，跳过损坏或仍被活跃实例锁定的文件；恢复后只有新快照成功写入才清理上一代。时间来源是文件系统墙上时钟，精确时间戳相同时由路径确定次序。

原生文件最多 96 MiB，图片及对象数量等上限见使用说明。PNG/HTML 共享原子字节写入 API，输出及已有目标的回滚副本上限为 256 MiB；该上限不放宽原生文件的 96 MiB 限制。

## 编译

```sh
TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug -j4 app-reliefppt app-fileman nls
```

目标构建退出 0，日志为 `build/reliefppt-dev/build-delivery.log`。全部 ReliefPPT 翻译单元另通过目标工具链 `-Wall -Wextra -Werror -fsyntax-only` 检查，日志为 `target-syntax-delivery.log`；该检查把既有 SDK 头作为系统头处理。普通构建仍会输出既有 SDK endian 宏的括号警告。临时诊断代码已移除。

最终 `out/xorg-vmware-debug/userland/reliefppt.elf` SHA-256：

```text
6fa7dd13c8801ee8cb556d7a6f87ff966c695ea05ad15720d4843740b2e1cc55
```

## 宿主及集成测试

```sh
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefppt.sh
```

模型、文档和几何测试使用 `-Wall -Wextra -Werror -fno-exceptions -fno-rtti`、ASan/UBSan；另有分配失败回归。覆盖候选提交原子性、撤销分支修订号、图片共享与回收、图层及多选几何、UTF-8 换行、RPPT 往返及畸形输入、文件冲突和恢复锁。最终退出 0，日志为 `build/reliefppt-dev/document/final-host-green.log`。额外回归实际写入恰好 256 MiB、替换同样大小的目标、拒绝 256 MiB + 1，确认原生加载仍拒绝大于 96 MiB 的输入且完整 Deck 不变。Linker-wrap 回归强制 `renameat2` 返回 `EOPNOTSUPP`、`ENOSYS`、`EINVAL`，确认 `linkat` 回退及新出现目标不被覆盖。无生产测试钩子。

相关集成测试为 `test-components.sh`、`test-fileman-boundary.sh`、`test-app-icons.sh`、`test-desktop-backend.sh`、`test-installer-stage.sh`、`test-apk-ownership.sh`。对应日志位于 `build/reliefppt-dev/*-green.log`；验证默认办公组件、控制台排除、原生文件关联及图标接入。

## QEMU 系统内验收

```sh
TMPDIR="$PWD/out/tmp" python3 tools/test_reliefppt_qemu.py \
  --image build/fltk-migration/desktop-ui-compact-fileman-10/guest-private.raw \
  --kernel build/kernel-throughput-5g/final/kernel.sys \
  --output build/reliefppt-dev/qemu-delivery --recovery
```

环境为 KVM/QEMU、2 vCPU、4 GiB、VMware VGA、USB tablet、1280×800。每次运行使用既有 Xorg 镜像的私有克隆，校验目标 ELF 和运行依赖，再注入程序、翻译、图标及测试夹具并读回 SHA-256。该环境不能代替 VMware 验证。

最终完整运行退出 0；`build/reliefppt-dev/qemu-delivery/result.json` 为 `passed: true`，记录源镜像不变、目标 ELF 与本报告 SHA-256 一致。测试覆盖中文剪贴板及连续数字输入、最小/恢复窗口中的输入框边界和 Esc 焦点、字号/属性、图片移动缩放、对象剪贴板和撤销重做、幻灯片复制/重排、保存重开、撤销后新分支关闭提示、PNG/BMP 插入、文字裁剪、备注、F5/Shift+F5、翻页/结束页/B 黑屏/退出。每次启动还由外部 X11 探针验证 WM_CLASS、图标通道和透明度。

恢复用例两次分别等待真实 60 秒定时器（脚本各等待 62 秒），均让仍在编辑的文本框经历 SIGKILL。第一次恢复后继续追加第二条文字，再自动保存、崩溃、恢复并另存；独立回读同时检查 `Recovery marker 2468` 和 `Recovery marker 8642`，并确认嵌入图片及图形仍在。

关闭 QEMU 后，使用 debugfs 从磁盘提取八份实际产物，在 VM 外用独立 C++ 验证器或图片/HTML 检查器核对：

| 文件 | 核对内容 |
| --- | --- |
| `ppt-fixture.rppt` | 精确中文、换行、连续数字、字号/几何、四页顺序、图层及图片 |
| `acceptance.png` | 960×540、编辑后的中英文内容及非空像素 |
| `acceptance.html` | 四张内嵌 PNG，首张与 PNG 导出一致，无备注和外部资源 |
| `branch-a.rppt`、`branch.rppt` | 撤销分支分别保留精确 A、B，关闭取消没有丢失编辑 |
| `images.rppt` | PNG/BMP 的尺寸、像素通道与 PNG 透明度 |
| `overflow.png` | 超长文字裁剪后，后续橙色图形仍被绘制 |
| `recovered.rppt` | 两次崩溃后的两轮修改和原有对象 |

各文件大小与 SHA-256 记录在 `qemu-delivery/artifact-hashes.json`，完整运行日志为 `qemu-delivery.log`。

最终截图均位于 `build/reliefppt-dev/qemu-delivery/`，已查看实际像素：

| 截图 | 内容 |
| --- | --- |
| `reliefppt-document.png` | 中文编辑器、功能区、缩略图和画布 |
| `reliefppt-minimum.png`、`reliefppt-restored.png` | 最小/恢复尺寸下仍在编辑的原生输入框 |
| `reliefppt-object-properties.png` | 位置与大小对话框 |
| `reliefppt-speaker-notes.png` | 中文备注 |
| `reliefppt-slideshow-second.png`、`reliefppt-slideshow-last.png` | 全屏中文及 Sans/Mono 字体回退 |
| `reliefppt-slideshow-black.png` | B 黑屏；测试读取实际黑色像素 |
| `reliefppt-recovered-second-generation.png` | 双次崩溃后的恢复文档 |

源镜像 SHA-256：

```text
c8a4240459c7981f5b5154158e865717f390d6d57be3e7755c5f960bec1be9ec
```

既有内核 SHA-256：

```text
f6a8193fdae96a097820e4eecd052d43d5cf0ed036be581d5433c4976afe73b7
```

较早运行首先停于测试 OCR 将 `94%` 识别为 `948`；调整裁剪及数值检查后发现真实 GUI 缺陷：窗口管理器缩放后焦点回到画布，Escape 清空选择却未结束输入。程序已改为将活动编辑期间的文字按键直接交给输入框；原用例随后通过字号及属性操作。PNG 首次导出另暴露 `renameat2(RENAME_NOREPLACE)` 返回 `EOPNOTSUPP` 时未进入标准 `linkat` 回退；这些失败日志保留，均不作为完整验收通过证据。随后一次运行的 Shift+F5 大标题 OCR 误读，已用所选页独有正文校验；插图测试的逐键绝对路径又触发上游 FLTK 选择框中途切目录，现使用其正常相对文件名输入，并继续由实际图片回读验证。`qemu-system` 的完整一代恢复验收退出 0，独立回读八份产物通过，但使用修复前 ELF，仅作为中间证据。最终审查发现多代恢复选择旧副本的 P2，已增加最新有效副本选择、成功后清理和双崩溃回归；最终验收必须使用本节列出的重建 ELF。

## 离线 HTML 浏览器检查

使用实际从 guest 磁盘提取的 `acceptance.html`，通过 Playwright 操作已安装 Chromium：检查四张内嵌 PNG 完整解码为 960×540、方向键/Home/End/PageUp/空格和按钮翻页、B 黑屏及全屏切换，无 JavaScript 错误或网络请求。测试程序为 `tools/tests/reliefppt_html_verify.cjs`，可通过 `PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH` 指定已有浏览器，`NODE_PATH` 指定 Playwright 所在目录。

最终浏览器检查退出 0，日志为 `build/reliefppt-dev/html-browser-delivery.log`，截图为 `qemu-delivery/html-browser.png`。输出为 `ReliefPPT offline HTML browser tests passed: embedded images, navigation, black screen, fullscreen, no network`。使用前述最终 ELF 生成的实际导出文件。

## 独立审查

模型/几何和桌面集成审查通过；文档审查中的两项文件系统保证已按本报告说明限定并记录裁定。GUI 预审的三项问题与实际窗口缩放后 Esc 焦点缺陷已修复。唯一一次最终广泛审查记录为 `build/reliefppt-dev/final-review.md`，其多代恢复 P2 的实现与测试见 `final-fix-report.md`；最终定向复审 `final-re-review.md` 将该 P2 标记为 ADDRESSED，静态规格/质量通过，无新增可复现回归；其当时尚未结束的双代 QEMU 验收随后已按本报告完成。时间戳排序边界仍如上披露。

## 验证范围

未重建发布 ISO；未修改内核、ABI golden、音频语义、XDM/IceWM 主题和背景。首版不支持 PPT/PPTX；HTML 为图片化分享格式，不可重新编辑。中文显示和 UTF-8 剪贴板输入由系统内验收覆盖，未逐个测试第三方输入法。**VMware 验证未完成。**
