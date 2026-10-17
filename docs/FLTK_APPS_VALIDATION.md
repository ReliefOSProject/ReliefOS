# FLTK 前端迁移验证报告

> 状态：acceptance-8 的应用交互、acceptance-9 的图片内容补验及正式镜像字节核对已完成。完整新装 E2E 在文件复制阶段超时；标准 VGA 条纹问题尚未定位，VMware 验证未完成。

本报告前面的 FLTK 验收章节记录该阶段的历史构建与运行证据；后续 XDM 改动重建了应用和镜像，最新核对见末节。`build/`、`out/tmp/` 下的截图、日志和 JSON 仅保存在本机，不随源码提交。

## 范围

本次迁移覆盖 12 个自有 X11 图形前端：`calc`、`osver`、`fileman`、`taskmgr`、`minesweeper`、`leonmmcoset`、`xiaobai`、`paint`、`imageview`、`doomlauncher`、`doom` 和 `installer`。这些前端使用 C++17 与 FLTK 1.3.11；应用行为模型仍以 C 实现，TTY 启动与控制路径保留。编译配置使用 `-fno-exceptions -fno-rtti`，并将目标链接到目标 musl、FLTK、libstdc++ 和 gcc runtime。

最新翻译检查日志 `out/tmp/fltk-translation-delivery.log` 统计了 **292 个直接 `T()` 字串，0 个未翻译**。本次目标应用 ELF 不再依赖 Motif `libXm` 或 Xt `libXt`。该结论只针对以上 12 个自有应用；第三方 `nedit` 仍单独保留它原有的 `libXm` runtime 依赖。

FLTK 官方 [1.3.11 Unicode 与 UTF-8 说明](https://www.fltk.org/doc-1.3/unicode.html)描述了 FLTK 的 UTF-8 文本接口；完整 API 可查阅 [FLTK 1.3.11 文档](https://www.fltk.org/doc-1.3/index.html)。本报告中的探针验证应用所用的文本输入、粘贴和列表选择路径，不代表完整输入法或所有 Unicode 行为已通过验证。

## FLTK 验收阶段的目标构建与 ELF

`out/tmp/fltk-all-final-build.log` 记录了全 12 应用构建成功（exit 0）。之后 fileman 刷新逻辑去除了重复整窗重绘，并修复进度回调重入；`out/tmp/fltk-fileman-verified-build2.log` 记录该修复版本重新编译成功（exit 0）。最新的 `out/tmp/fltk-fileman-launch-build.log` 还记录了入口修正构建成功（exit 0）：`spawn_app` 仅在应用名精确为 `installer` 且没有路径参数时传入 `--graphical`。

对该阶段 ELF 使用 `readelf` 和 SHA-256 检查，12 个目标均使用 `/lib/ld-musl-x86_64.so.1`，动态依赖不含宿主 sysroot 路径、glibc、`libXm` 或 `libXt`。目标 musl loader `--list` 在该阶段 `rootfs/managed` runtime 路径中解析了完整依赖闭包：11 个普通应用各解析 31 个依赖对象，DOOM 解析 32 个；合计 32 个唯一 SONAME，没有未解析项。独立复核日志 `out/tmp/fltk-elf-delivery.log` 确认 12 个验收 ELF 的 SHA 与审计一致，且无宿主路径、glibc、Motif 或 Xt 依赖；完整结果见 `build/fltk-migration/elf-audit.json`（本机记录）。`libXm` 与 `libXt` 仍为第三方 `nedit` 保留，不属于这 12 个应用的闭包。

| 应用 | FLTK 验收阶段 ELF SHA-256 |
| --- | --- |
| calc | `cb6ada809fa68fae0c000d674d15804a123985aa97e189e2cc71d427b2c6ec02` |
| osver | `629951fb393e15e8feb1ecf9e31b5acfaa2634ac76431dce060abca8d08a75a1` |
| fileman | `c2afb408cdebdedf6cae67297d4e67484c105812cc5d10252e12ecc2ce2e3044` |
| taskmgr | `bbcaaa0e3b29bdce841e851fa7411ee0c966558cb82cb40024a7e59883712781` |
| minesweeper | `c83b2183a5f5d5436e2cb6f0973a364d855a3538e8b09391b8b89726d57ccc02` |
| leonmmcoset | `652667d31b7cd530aa08760a816db45c85dbf2c608c346316abdbd150f984d5a` |
| xiaobai | `1af9101b984296757f7e447ce494865212791783777fe9c91b828c57d83a9c08` |
| paint | `b0ee36f139a28fb7f6b0f64ea647c035f89383227067fd8708d9fd142a158e15` |
| imageview | `c28584e4ddb3f3daab2cbff79b90057081439cfbd75b1a4e1a9cdbeb7e9577bc` |
| doomlauncher | `28041f7f4f5c339c8f89ae5446823930607e478eb6be065a29dfe571265e7f89` |
| doom | `ab6e5732b2cf2643b52a5f1fa97a8a2fc1de67b3012d0bedcf51ae81fc4262be` |
| installer | `7c9ad84a7b5798a99b2892a5270b7449ba2fd4246f6db10d4c10e7e8564ed6f7` |

可在构建后复核：

```sh
for app in calc osver fileman taskmgr minesweeper leonmmcoset xiaobai paint imageview doomlauncher doom installer; do
    elf="out/xorg-vmware-debug/userland/$app.elf"
    sha256sum "$elf"
    readelf -l -d "$elf" | rg 'interpreter|NEEDED|ld-musl'
done
```

## FLTK 验收阶段的镜像与安装器 payload 字节核对

该阶段最终 `make -j8 O=out/xorg-vmware-debug PROFILE=debug image-vmdk installer` 构建成功（exit 0，日志：`out/tmp/fltk-images-final-build.log`）。只读审计见 `build/fltk-migration/image-audit.json`（本机记录），记录的 80 项检查全部通过：`qemu-img compare` 对 raw/VMDK 返回 exit 0（内容相同）；GPT 第 2 分区从 sector 264192 开始，2,228,224 sectors，分区原始字节与 `disk-root.ext4` 的 `cmp` 返回 exit 0。raw root 中 11 个普通图形应用 ELF、正式 ISO runtime 中 12 个 ELF（包含 installer）、以及 ISO 的 `/install/root` 安装目标 payload 中 11 个 ELF，全部逐个回读并与该阶段 userland ELF SHA-256 相同。installer 只在 ISO runtime 中；raw root 和 `/install/root` payload 均没有 installer ELF。

ISO 审计只用 `xorriso` 抽取 `/install/root.fat`（exit 0），再通过 `debugfs` 定点回读文件；没有解包整个 rootfs，也没有修改正式镜像。12 个应用依赖的 32 个唯一 SONAME 都与该阶段 `rootfs/managed` 内容一致。逐项核对还确认 FLTK 1.3.11、libstdc++、libgcc_s、目标 musl libc、`libreliefos`、中文 MO 和 SimSun 字体与该阶段构建相同。baseline 派生的 acceptance-5 私有 root snapshot 中字体没有被 runner 注入；该字体 SHA-256 与正式 ISO runtime 相同：`1526ac24375f51f6eb73bc2d3f8072dbe4a80a3a65217677c9d9a84f67dab2ab`。

| FLTK 验收阶段产物（历史镜像） | 大小（字节） | SHA-256 |
| --- | ---: | --- |
| `reliefos.raw` | 1277165568 | `6b3064f702a80d38b0ced09da52ad79a8d8a3082c2d877e6446e3f53fb1df37d` |
| `reliefos.vmdk` | 967376896 | `d3f058de6446b8bb891d8300d0359baeac5116ffd791577cc6a7f6dfee290222` |
| `reliefos-installer.iso` | 1664972800 | `02bc68c5d65af00d7d9a5ae5fee415f205ab5cb4749194225982c6a8e051c6c1` |

这是镜像文件与选定 guest 文件的字节核对，不等同于完成安装或物理硬件验证；GUI 运行结果见 acceptance-8/9，完整新装的超时限制见下文。

## 主机测试

最新交付轮主机检查记录如下：

- `out/tmp/fltk-backends-delivery.log`：29 项测试全部通过（`OK`）。
- `out/tmp/fltk-models-delivery.log`：C 行为模型和前端集成模型通过，覆盖计算器、任务管理器、扫雷、图像查看、画图、DOOM launcher 与 installer 页面流程。
- `out/tmp/fltk-translation-delivery.log`：292 个直接 `T()` 字串，0 个未翻译。
- `out/tmp/fltk-elf-delivery.log`：12 个最终 ELF 与审计哈希匹配；目标为 x86_64 musl+FLTK/C++，没有宿主路径、glibc、Motif 或 Xt 依赖。
- `out/tmp/fltk-brand-new-files-final.log`：品牌门禁包含新增迁移源文件的独立临时 index 审计通过；内核使用独立 index，真实 index 未改变。
- `out/tmp/fltk-test-nls-build-final.log`、`out/tmp/fltk-test-nls-extract-final.log`、`out/tmp/fltk-test-userland-graph-final.log`、`out/tmp/fltk-devroot-final.log`、`out/tmp/fltk-license-final.log`：分别记录 NLS、构建图、devroot staging 和许可证检查通过。
- `out/tmp/fltk-fileman-boundary-final.log`、`out/tmp/fltk-fileman-operations-final.log`：fileman 路径边界与文件操作主机测试通过。
- `out/tmp/fltk-doom-refill-build.log`、`out/tmp/fltk-doom-refill-run.log`：DOOM 音频 refill 的独立 ASan/UBSan 主机测试通过；它不替代目标音频设备播放测试。

相关构建门禁可这样重跑：

```sh
TMPDIR="$PWD/out/tmp" sh tests/build/test-motif-apps.sh
TMPDIR="$PWD/out/tmp" sh tests/build/test-userland-graph.sh
TMPDIR="$PWD/out/tmp" sh tests/build/test-brand-identity.sh
TMPDIR="$PWD/out/tmp" sh tests/build/test-x11-development-stage.sh
```

## 目标系统 GUI 验收

`build/fltk-migration/acceptance-8/run.json` 记录了 2026-10-16 UTC 完成的 QEMU GUI 验收。12 个应用均启动，`failures` 为空；9 个配置了预期中文标签的应用，其标签识别检查全部为 true。`calc` 完成 `3 + 4 =` 并显示 7。目标 `fc-match` 返回 `宋体,SimSun`，字体文件为 `/usr/share/fonts/reliefos/simsun.ttc`，字符集覆盖 U+4E2D（“中”）。FLTK 探针通过 `Fl_Input` 与 `Fl_Secret_Input` 的 UTF-8 粘贴、`Fl_Browser` 中文条目选择，以及 `copy_input` 容量接受/拒绝且拒绝后内容不变。截图、OCR、串口和回读证据保存在 `build/fltk-migration/acceptance-8/`（本机记录）；baseline 源镜像前后 SHA-256 一致，12 个注入 ELF 的来源、注入及 readback SHA 均与该阶段构建相同。

fileman 在中文路径上完成重命名、复制、地址导航和粘贴，guest helper 的 UTF-8 文件比较输出 `FLTK_FILEMAN_VERIFY PASS`。它还打开了 installer 子窗口：串口 buffered 日志在父进程退出后复核，确认独立 child PID 339，fileman parent PID 332；helper 的 UTF-8 文件 `cmp` 校验通过。关闭子窗口后 fileman 仍在运行。paint 通过红色像素绘制、未保存保护、取消与保存；Save As 选择器中的中文标签、中文文件名和中文目录均可见，保存 helper 输出 `FLTK_PAINT_VERIFY PASS`。installer 在只读模式完成页面 `0 → 1 → 2 → 3 → 4 → 3 → 2 → 1 → 0` 的往返；未选磁盘，也未启动安装。

DOOM 截图确认真实游戏帧已显示；runner 按预设时限结束进程，exit status 143（SIGTERM）是预期超时结果，不计作应用失败。QEMU 没有 PCM 音频设备，因此这不验证目标声卡播放。其余应用也都留下窗口和串口证据。OCR 结论仅针对 9 个预期标签的识别检查，不代表所有界面文字均经人工逐项确认。

**图片程序补验：** 人工复核发现 acceptance-8 的 `leonmmcoset`、`xiaobai` 截图只有窗口边框，客户端尚未首绘；旧检测被底层终端文字变化误触发。因此，acceptance-8 的自动通过不作为这两款程序已显示图片的证据。runner 改为按 C `fit_rect` 和 floor 最近邻缩放规则生成预期图片，再比对真实客户端像素。旧的透出背景截图匹配率分别仅为 0.004172 和 0.000026，低于 0.995 的通过门槛。

定向 `build/fltk-migration/acceptance-9/run.json`（本机记录） 随后对这两款程序通过（0 failures、均 exit 0）：实际 PNG 匹配率均为 **0.999827**，图像区域为客户端 `[11, 8, 738, 738]`；源 PNG、注入文件和 guest readback SHA 一致，baseline 前后不变。人工查看原始截图也确认了真实图片内容。补验没有修改产品代码或正式镜像。

acceptance-8 使用当时的 runner 版本。验收之后只对测试 harness 增加了 child window closed 状态清理，以及父窗口只读状态栏的 focus 清理；没有改动产品代码，harness `py_compile` 已通过。报告使用 acceptance-8 的实际运行证据，不把之后的 harness 清理表述为该次运行的一部分。

## ISO 窗口测试、完整安装与未验证项

该阶段正式 ISO 的 QEMU 直接启动窗口测试通过（exit 0）：日志为 `out/tmp/fltk-iso-final.log`，截图和串口记录位于 `out/tmp/fltk-iso-final/installer/`。这只是启动并检查 installer 窗口，不代表完成了磁盘安装。标准 VGA 的 800×600 测试仍失败：`out/tmp/fltk-iso-ui.log` 记录页检测未通过，截图呈现条纹画面；证据在 `out/tmp/fltk-iso-ui/installer/`。原因尚未定位，不能归因于本次迁移或判为既有问题。

**完整新装 E2E 未通过（超时）。** 使用正式 ISO 和全新 8 GiB 临时磁盘运行 `installer-e2e-2`，控制进程最终 exit 1：`wait_install` 在其 2400 秒等待上限后报 `AssertionError: Installation timed out`。最后截图约 92%，仍在复制 `/usr/share/vim/vim92/syntax/`，没有到达 Finish 页。截图与日志保存在 `build/fltk-migration/installer-e2e-2/`（本机记录） 和 `out/tmp/fltk-installer-e2e2.log`；结构化结果见 `build/fltk-migration/installer-e2e-result.json`（本机记录）。测试控制进程已关闭其 QEMU。

该轮已经通过第 0 至第 8 页导航、账户表单校验，以及 Xorg 默认勾选、取消、后退返回保留和重新启用的断言。它没有完成账户/引导最终写入或安装目标的 ELF 读回校验，因此不能作为完整新装成功的证据。此前首次尝试在复制阶段收到来源未知的 SIGTERM，亦未完成。尚无旧前端的同配置完整安装对照，不能排除性能回退，也不把本次超时归因于某个已定位根因。

**VMware 验证未完成。** 仍未验证：真实 VMware Workstation/ESXi 与物理硬件；完整拼音输入法/IME；升级/update 流程；物理音频设备上的 DOOM 播放。安装器 TTY 路径保留并有主机模型测试，本轮没有跑完整的客机 TTY 安装。QEMU 无 PCM 设备，主机 refill 测试不能替代声卡验收。

本次用户态迁移不涉及内核、ABI golden 文件或 audio manifest；当前报告未声称完成完整新装或硬件验收。

12 个前端的实际界面缩略图见 `build/fltk-migration/apps-preview.png`（本机记录）（图片程序使用 acceptance-9 的有效截图）。


## XDM 配置调整后的镜像核对（2026-10-17）

后续调整把 XDM 配置与脚本移到上游默认路径，恢复 Alpine XDM 1.1.17 的灰底、Blue3 greeting 和 Xorg 标志资源；Xsetup 不再设置旧壁纸，安装器和已安装桌面均由原生 `icewm-session` / `icewmbg` 管理主题背景。当前 Win95 主题未指定背景，采用 IceWM 原生纯黑回退。

`TMPDIR="$PWD/out/tmp" make -j4 O=out/xorg-vmware-debug image-vmdk installer` 完成（exit 0，日志 `out/tmp/xdm-gray-theme-build.log`）。这次重建后的 12 个应用 ELF 已重新检查为目标 musl、FLTK/C++，无 glibc、Motif、Xt 或宿主路径依赖；RAW、ISO runtime 与安装目标 payload 的 34 项应用 ELF 回读均与本轮构建逐字节相同。记录位于 `out/tmp/desktop-precommit-images.json`；上面的历史 ELF 和镜像哈希不代表这批重建产物。

| XDM 调整后产物 | 大小（字节） | SHA-256 |
| --- | ---: | --- |
| `reliefos.raw` | 1277165568 | `acf264fef1bba8897c4b0c97b83d79ec131ee2276fd55d2f7a554f5d64347ceb` |
| `reliefos.vmdk` | 953286656 | `ddeba0ba134a23a45787e7c091f884c94614433784f92a9813c6f60f97d32cfc` |
| `reliefos-installer.iso` | 1631221760 | `d1ffbb46e027eb36d082218e451baab95c0c37e517ab5d5375d565e717caa0a9` |

另有 42 项 XDM 配置/脚本内容回读通过：标准路径和源码一致，旧配置路径及旧壁纸未打包，console profile 不包含 XDM，RAW/VMDK 内容一致。记录位于 `build/xdm-gray-theme/image-audit.json` 和 `out/tmp/xdm-gray-theme-image-audit.log`。

本轮定向 QEMU 检查使用新 RAW 和 VMware SVGA 模拟设备，完成灰色 XDM 登录、原生 IceWM/background 启动及中文 fileman 首绘；完整 1280×800 截图位于 `build/xdm-gray-theme/qemu/xdm.png`、`build/xdm-gray-theme/qemu/icewm-fileman-desktop.png`，结果为 `build/xdm-gray-theme/qemu/result.json`，捕获程序 exit 0。截图经过人工查看，确认中文控件实际显示。其余应用的交互验收仍以前述 FLTK 阶段的记录为准。

提交前重新运行 XDM/控制台/画面识别单元检查，46 项通过；安装器与任务管理器主机检查 42 项通过，复制读取聚合的 ASan/UBSan 检查通过。C 行为模型、C++ 构建图、devroot、桌面/安装器 staging、APK ownership 与许可证检查亦通过。

更早的完整 VT/XDM QEMU suite 在登录、切换与退出后，于日志收集的 `xdm.log changed prefix during the run` 断言失败；串口中混入内核日志，尚未完成该完整 suite 的通过验证。本轮定向截图通过不替代该测试，也不替代前述完整新装、升级或硬件验证。VMware 验证未完成。

## Fileman 文件名搜索（2026-10-17）

Fileman 新增地址栏右侧“搜索”按钮、编辑菜单入口与 `Ctrl+F`。默认在当前目录和子目录按名称子串搜索文件/文件夹；可修改起点、关闭递归、显示隐藏项或切换英文字母大小写匹配。中文 UTF-8 名称保持原字节。搜索结果使用独立的完整路径，不复用主列表的 64 项容量。

搜索由 FLTK 每 10ms 分批推进，每批最多处理 32 次目录读取；停止和关闭窗口会回收全部扫描目录句柄，停止保留已找到的结果，重新搜索会清空旧结果。递归不跟随子目录符号链接。默认结果上限 1000、扫描上限 100 万项；达到全局限制会明确停止，权限、路径长度和深度限制导致的局部遗漏则显示“搜索完成（部分）”与跳过计数。路径不作截断，超长根路径/查询报错。

结果支持打开和打开所在文件夹；后者在主列表包含该项时选中并滚动到它。主列表仍保留原有的 64 项呈现上限及隐藏项设置，定位不保证能高亮被这些设置排除的条目，搜索列表本身仍可直接打开完整路径。

编译：`TMPDIR="$PWD/out/tmp" make -j4 O=out/xorg-vmware-debug PROFILE=release app-fileman nls` 通过，日志为 `out/tmp/fileman-search-final-build.log`。这是目录名含 debug 的显式 release 构建；本轮没有修改内核。

主机检查：`test-fileman-search.sh`、原有 `test-fileman-boundary.sh` / `test-fileman-operations.sh`、`test-nls-build.sh` / `test-nls-extract.sh` 均通过。搜索使用真实目录和 ASan/UBSan，覆盖中文与 ASCII 大小写、同名完整路径、隐藏项、80 项目录、符号链接环、取消/重启、FD 回收、长度与资源上限。本机以 uid 1000 验证了无权限子目录跳过而兄弟目录继续；root 运行该用例时会明确输出 SKIP。

GUI 回归还发现并修复了两个键盘集成问题：关闭对话框后恢复文件列表焦点，使 Ctrl+L 等原有操作继续可用；主窗口的 Ctrl+F 会被文件列表和输入控件提前吞掉，因此在主窗口的地址栏、目录/文件列表和只读输出控件处理按键之前统一响应搜索快捷键。搜索窗口内的输入控件仍使用各自原有行为。

早期 `build/fltk-migration/fileman-search-1/` 至 `fileman-search-5/` 保留失败与诊断记录，其中混合中英文路径、稀疏标签和蓝底白字选中行存在 OCR 漏读；人工查看原始截图确认实际路径与选中行。路径 OCR 作为辅助证据，名称/完整路径的精确行为由真实文件系统主机测试验证；定位另外验证选中行颜色和文件名，Ctrl+F 重开另外验证真实中文标签，避免仅凭画面变化误判。第 5 轮还暴露了上述 Ctrl+F 吞键问题；第 6 轮候选测试主动中止，未计为通过。

最终目标系统验收为 `build/fltk-migration/fileman-search-7/run.json`，runner exit 0、`failures: []`、Fileman exit 0；日志为 `out/tmp/fileman-search-qemu-7.log`。使用 QEMU/KVM、2 vCPU、4 GiB 内存和 VMware SVGA 模拟设备，内核显式使用已验证的 release ELF `build/kernel-throughput-5g/final/kernel.sys`，未重新构建或修改内核。可用以下命令重跑（output 必须为新目录）：

```sh
TMPDIR="$PWD/out/tmp" python3 -u tools/test_fltk_apps_qemu.py --apps fileman \
  --kernel build/kernel-throughput-5g/final/kernel.sys \
  --probe-elf build/fltk-migration/acceptance-9/fltk_ui_probe.elf \
  --output build/fltk-migration/fileman-search-new
```

客机验证了递归 1 条匹配、禁用递归 0 条、80 条结果、中文完整路径显示、定位父目录并选中目标（选中行蓝色检测区域占比 1.0）；扫描 1090 项后实际停止，再查询 `/tmp` 完成且为 0 条，没有沿用旧结果。关闭后文件列表焦点下 Ctrl+F 成功重开；地址栏焦点下 Ctrl+F 成功打开，随后按 Enter 打开目录搜索结果。原有中文重命名、复制粘贴、导航以及独立 installer 子窗口启动/关闭均通过，文件系统 helper 输出 `FLTK_FILEMAN_VERIFY PASS`。取消保留已有结果由主机模型覆盖；这轮客机停止用例使用无匹配查询，不将其表述为 GUI 保留结果的端到端验收。

人工查看本轮搜索与重开窗口截图，确认中文标签和名称实际可见。最终 Fileman ELF SHA-256 为 `5d449795d46424d44949da2dd5ba7af8c8f582fb9c37bd9e6d787aea47927a6e`，生成的 zh_CN catalog 为 `f95efec44ea6a36144776b42dd97421e9e28c17f1b88af9edcefedde7b31ee9e`；注入值、客机文件系统 readback 与当前构建一致。源 baseline 镜像及 QEMU snapshot 的私有基础镜像运行前后哈希不变，核对记录为 `out/tmp/fileman-search-final-evidence.json`。这轮仅验证 Fileman 及所需子窗口，其他应用仍以前面的独立记录为准。

正式 RAW/VMDK/ISO 尚未重新打包。VMware 验证未完成；私有 QEMU 验收不能替代真实 VMware、物理设备或完整中文输入法验证。


## Fileman / Taskmgr 原生界面整理（2026-10-17）

两款程序继续使用原有 FLTK `gtk+` scheme、SimSun 中文字体及原生控件，共享主题未修改。界面采用紧凑的工具布局，没有窗口内程序大标题或常驻说明段落。帮助放在控件 tooltip，标签页各自展示本页的数据和操作。

Fileman 默认 960×600、最小 800×480。菜单下是导航/地址/搜索行和分组文件操作栏，其余主要区域留给文件夹树与名称/类型列表；底部仅一行操作状态与条目计数，耗时操作在同一行显示真实进度。所选完整路径可在状态栏 tooltip 及原有详情窗口查看，搜索结果仍保留独立的完整路径输出。按钮状态反映当前选择与剪贴板，双击打开与 Ctrl/Shift 多选保留；Ctrl+L、Ctrl+F、F5、Alt+Up 在主窗口控件上统一处理，文本输入中的复制/剪切/粘贴由原控件处理。原有打开方式、默认关联、快捷方式、属性与权限、重命名、删除、复制/剪切/粘贴、批量选择、归档、设置、隐藏项与递归搜索均保留，后端 C 文件未改动。

Taskmgr 默认 1000×660、最小 800×520。全局仅保留菜单、标签页和状态栏；进程页使用对齐的七列表格与刷新/详情/结束任务操作，性能页独立展示图表、每核占用和内存/运行时间/任务统计，启动项页包含用户选择、启用/禁用/移除及一个服务管理器入口。全局资源卡、重复页标题、说明行和重复菜单项已移除。保留进程树次序、按 PID 跟踪选择、菜单/右键操作、Enter/Delete/F5、确认框及 500ms 采样。性能滚动区按 ABI 最大 64 核心计算内容高度，并保持、钳制缩放后的滚动位置；这不等于已在 64 vCPU 硬件实测。结束任务及移除启动项在确认前保存 PID 或 UID/条目 ID，避免定时刷新改变操作对象。

本阶段的目标构建与 NLS 构建通过；紧凑布局最终共同构建为 `TMPDIR="$PWD/out/tmp" make -j4 O=out/xorg-vmware-debug PROFILE=release app-fileman app-taskmgr nls`，日志 `out/tmp/desktop-ui-compact-final-build.log`（exit 0）。主机测试 `test-fileman-search.sh`（ASan/UBSan）、`test-fileman-boundary.sh`、`test-fileman-operations.sh`、`test-motif-apps.sh`、`test-nls-build.sh`、`test-nls-extract.sh` 均通过，日志位于 `out/tmp/desktop-ui-*-test.log` / `desktop-ui-nls-build.log`。Taskmgr 模型检查覆盖格式、利用率、历史环和树次序，不将其表述为启动项修改的 GUI 端到端测试。

交互验证发现并修正了 Taskmgr 的实际缩放与选择问题：布局使用 resize 传入的新尺寸，关闭与手动布局冲突的自动缩放，进程与启动项浏览器显式使用 `FL_HOLD_BROWSER`。Fileman 文件夹树启用 FLTK 原生双击回调条件，且仅在左键释放的双击事件切换展开状态，解决重复双击已选行不能展开/收起的问题；依据为 [Fl_Browser_ 原生实现](https://github.com/fltk/fltk/blob/branch-1.3/src/Fl_Browser_.cxx)。

测试记录真实 ConfigureNotify 几何，允许 USB tablet 拖动最多 2 像素量化误差，并另行断言最小尺寸不低于应用声明值。OCR 避开边框、tooltip 和 native focus 虚线；原始截图不经这些 OCR 预处理。中文路径通过原生剪贴板保存/恢复，不能用只支持 ASCII 的 QMP 键入模拟中文。Taskmgr 测试限定独立临时 sleep applet 的串口 PID，在 PID 列及详情/确认框核对名称与完整数字，排除父 PID 和较长数字；只有确认后收到同一 PID 非零退出状态并看到列表更新，才判定结束任务成功。GPU 当前值单独裁区域，允许有效百分比或原生 N/A。

Taskmgr 最终客机结果为 `build/fltk-migration/desktop-ui-compact-taskmgr-4/run.json`，runner exit 0、Taskmgr exit 0、`failures: []`。使用 QEMU/KVM、2 vCPU、4 GiB 和 VMware SVGA 模拟设备；验证了七列与对应 CPU/内存值、精确 PID 的详情、结束确认与定时列表刷新：独立 sleep applet 的 PID 为 310，客机快照名称为 busybox，确认后记录 `FLTK_TASKMGR_WORKER_EXIT pid=310 status=143`，随后从列表消失。性能页的 CPU/内存百分比、两个核心读数、总/已用/可用内存、uptime 和 GPU N/A 均按独立区域识别，跨 500ms 采样周期后仍有效；启动项页验证 test 用户、控件、列表区域与单一服务管理器入口。本轮没有启用、禁用或移除现有启动项，不能作为这些修改操作的 GUI 端到端验收。

Fileman 紧凑布局回归曾发现关闭搜索窗口、定位父目录后仍显示旧画面的实际问题。诊断记录确认模型、地址控件和列表内容已更新；单独或组合请求全窗重绘与 flush 均未解决。仅将顶层窗口从 `Fl_Double_Window` 换为原生 `Fl_Window` 的对照中，地址、64 条目录内容与目标选中行均正确显示，其他控件、布局和搜索模态窗保持相同。最终采用该顶层组件，移除了无效的强制 flush；此证据不用于断言具体 X server 或内核根因。`desktop-ui-compact-fileman-9` 的失败只剩选中行 OCR 裁剪带入下一行，原始截图显示目标已正确选中；测试现只裁实际首行，原图保留，蓝底白字转换仅用于 OCR。

Fileman 最终客机结果为 `build/fltk-migration/desktop-ui-compact-fileman-10/run.json`，runner exit 0、Fileman exit 0、`failures: []`，日志为 `out/tmp/desktop-ui-compact-fileman-qemu-10.log`。验证了文件夹树双击收起/展开、新建目录、双击打开与 Alt+Up、中文重命名、复制粘贴、独立 installer 子窗口打开/关闭，以及搜索递归 1 条、非递归 0 条、80 条结果、定位父目录并选中目标（选中区域蓝色占比 1.0）。扫描 1121 项后实际停止，再查 `/tmp` 完成且为 0 条；Ctrl+F 重开与 Enter 打开目录结果均通过，文件系统 helper 输出 `FLTK_FILEMAN_VERIFY PASS`。真实缩放记录为最小 800×481、恢复 960×599、最大化 1280×754，均符合声明最小值与最多 2 像素拖动量化容差。

最终 Fileman ELF SHA-256 为 `b4ec0c2cbad8d5daedcee1edea5513ff6ef4ee196202c3d4dc0fa27258005188`，Taskmgr 为 `2838d6ddf670ec8bc6935fb7f24b3abe32b37054129d5548269db9d3900aebca`，zh_CN catalog 为 `99e62c17017ffc3ad67d91b63552293a162651d504213b8640f05ea2d0b3ec4c`。各应用最终通过轮次的注入文件、客机 readback 与当前相应构建一致；全部注入文件 readback 与当时记录值一致。核对记录为 `out/tmp/desktop-ui-final-evidence.json`。内核仍使用 release `build/kernel-throughput-5g/final/kernel.sys`，SHA-256 为 `f6a8193fdae96a097820e4eecd052d43d5cf0ed036be581d5433c4976afe73b7`；本轮未修改内核。源 baseline 与 QEMU snapshot 使用的私有基础磁盘运行前后哈希不变。

两轮紧凑布局定向测试使用 `--skip-input-probe`，跳过项在 manifest 中明确记录；共享字体检查仍执行。共享 UTF-8 输入、密码框粘贴、浏览器选择和 `copy_input` 容量边界已在 `desktop-ui-fileman-3` 完整输入探针通过，共享 `fltk_support.h` 此后未修改，不将其表述为完整中文输入法验收。

交付截图取自实际 QEMU 客户区：Fileman 使用 `desktop-ui-compact-fileman-10/app-fileman-restored.client.png`，Taskmgr 使用 `desktop-ui-compact-taskmgr-4/app-taskmgr-processes.client.png` 和 `app-taskmgr-performance-after-timer.client.png`；仅裁剪客户区，没有修改显示内容。正式 RAW/VMDK/ISO 未重新打包。启动项修改、真实 VMware 与物理设备仍不属于上述 GUI 通过范围。VMware 验证未完成。
