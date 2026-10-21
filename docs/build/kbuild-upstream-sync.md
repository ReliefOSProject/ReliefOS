# Kbuild 同分支更新集成记录

已在原迁移工作树合并 `origin/rewrite/desktop-apps` 的两个新增提交，并把新增
生产编译规则迁入固定的 Linux 6.12 Kbuild。两个应用的功能 C/C++ 源码与远端
提交保持一致；以下记录提交前的验收结果。

## 合并与保留边界

| 项目 | 本次结果 |
| --- | --- |
| 原父仓基线 | `b3ea05834f4b0c2798a0d2f50235cae29d6c888d` |
| 新父仓基线 | `00d2c680bc9c96070d116c9df1f925c0bcb66910` |
| 新增提交 | `e67a0a0d` ReliefPlayer；`00d2c680` ReliefCapture |
| 内核子仓 | HEAD/gitlink 仍为 `10b489986baeece8f89520e975beeb88d47b29b4`，原 Kbuild 迁移保留 |
| 工作区 | 复用附加工作树；原工作区的本地分支和六项既有状态保持原样 |

合并前归档父仓 366 项、子仓 170 项迁移状态、补丁、当前文件和源码摘要。
父仓基线快进后，仅两个重叠路径需恢复迁移语义：`AGENT.md` 三方合并，
同时保留新 UI 约定和 Kbuild 约定；旧用户态 Make 片段继续删除，其新增参数
全部迁入当前产品适配层。其余非远端变更路径逐文件核对字节不变。
备份和 `merge-receipt.json` 位于 `out/kbuild-upstream-sync/`。

## 构建图变化

- `userland/apps/reliefplayer/Kbuild` 登记六个 C++ 对象，保留旧排序和 minimp3 头目录。
- `userland/apps/reliefcapture/Kbuild` 登记十个 C++ 对象，保留旧排序和直接 X11 链接。
- X11 开发阶段把 `libX11.so`、`libX11.so.6` 列为完整性成员，缺失任一个会调用实际生产者恢复。
- 组件配置、包归属、图标、翻译、Fileman 文件关联和 IceWM 快捷键保留远端内容。
- 独立测试夹具维持测试边界；两个新 QEMU 工具增加 `--build-dir`，共用 FLTK
  探针也接收该参数。原默认值和功能断言保留，未用旧目录的产物替代新构建。

添加描述前，真实构建因新应用缺少 Kbuild 退出 2；补齐后两个应用编译链接退出 0。
原生 `.cmd`、归档对象顺序、minimp3 头依赖及 ELF `DT_NEEDED` 均已检查。
移除 `libX11.so.6` 后，构建恢复了原字节和原符号链接目标。

## 编译、打包与主机测试

最终产品仍在工作树 `out/kbuild-verified/`，文件名和公开路径规则不变。
默认配置选择 66 个组件应用，其中 23 个使用 FLTK；原有辅助程序及 worker 照常构建。
本次日志位于 `out/kbuild-upstream-sync/`。

| 层次 | 结果 | 证据 |
| --- | --- | --- |
| 完整产品链 | `make all` 退出 0，SDK、208 包桌面根、VMDK、Live ISO、Installer ISO 均完成 | `all.log` |
| 稳定重建 | 4,086 个对象时间戳不变，无编译/归档/链接；12 项关键产物字节与时间戳不变 | `noop.log`、`noop-final.log`、`noop-identities.json` |
| SDK 消费 | 路径含空格重定位；真实 C/API 静态、动态链接均通过 | `sdk-relocation.log` |
| 编辑器导出 | 486 条实际原生命令的目录、源文件和输出均存在；包含播放器 6 条、截图工具 10 条 | `editor-export.log`、`editor-validation.log` |
| 完整生产增量 | 源码、头、UAPI、C 参数、链接脚本、同路径 AR、缺失对象/产物/导出头恢复均通过 | `incremental.log` |
| 相关主机回归 | 播放器、播放会话/排空、截图模型/PNG/X11、两应用归属、组件、FLTK、X11 阶段、Fileman、图标和桌面后端共 13 个脚本通过 | `host-results-0..3.json` |
| 镜像回读 | 内核、两个应用、14 个图标、入口、中文 MO、快捷键与 X11 库匹配当前产物 | `image-readback.log`、`image-readback/result.json` |

公开 `make test` 仍退出 2：普通及 ASan/UBSan 宿主测试通过，随后在原有
`component policy mismatch: ['soundctl']` 处停止。没有宣称整个公开套件通过。
ABI golden、音频 manifest、Pages 和原内核 signalfd 既有问题仍按初次记录保留。

## 实际来宾运行

从本次 RAW 派生私有测试账户镜像，加入公开 test/test 夹具；没有替换其中的
生产程序、内核、运行库、主题或快捷键。应用工具再克隆该测试镜像，注入测试
探针和当前相同产物，并回读核对。生产 RAW/VMDK 均保持原样。

- ReliefPlayer：QEMU/KVM、Xorg、IceWM、HDA。WAV/MP3 播放、暂停、定位、音量、
  结束、错误文件和三次正常退出通过；实际 PCM 录音检查到 153 个双声道 WAV
  有效频率窗口及 118 个单声道 MP3 重采样窗口，无 underrun 恢复。
  见 `out/kp2/result.json` 和 `player-qemu-green.log`。
- ReliefCapture：快捷键、区域/窗口/全屏、延时、取消与重入、标注、撤销、剪贴板、
  PNG 保存和退出通过；源像素误差比例 0，独立剪贴板读取匹配，五份 PNG 已回收核对。
  见 `out/kc1/result.json` 和 `capture-qemu.log`。
- 已实际查看播放器 WAV/MP3 和截图编辑器的真实截图，中文控件完整可见。
  截图位于 `out/kp2/reliefplayer-{wav,mp3}.png`、
  `out/kc1/reliefcapture-{annotated,fullscreen-editor}.png`。
- Live/Installer ISO 内的内核回读摘要均匹配本次原生内核，见 `iso-readback/result.json`。
- 公共 `make test-smoke` 退出 0：VMDK、Live ISO、Installer ISO 均到达内核
  boot complete 和 PID 1；三份完整串口日志未发现 panic、bugcheck 或 fatal。
  见 `media-smoke.log` 和 `out/kbuild-verified/logs/smoke-{disk,live,installer}.log`。

第一次播放器 QA 在启动 VM 前发现共用探针仍引用旧输出目录，记录保留为
`player-qemu.log`；修复参数传递后才完成上述验收，未降低任何功能断言。

## 源码和残留复核

生产构建和应用验收前后 3,536 个源文件摘要一致；构建期间三个 QA 路径适配
文件的有意变更单独记录，并保留原始快照。最后文档与计划更新另行记录，
最终核对包含这些新文件，共 3,537 项。独立审查已接受新增构建规则、QA 路径
适配和分层报告，未发现需要修复的遗漏。

`source-audit-final.log` 覆盖父仓 375 项、子仓 170 项 tracked/new 改动，两仓
各 137 个上游引擎/许可证文件摘要一致。旧入口检索共 190 条文字命中：164 条
历史记录、11 条删除/迁移说明、3 条子仓旧基线和 12 条运行时候选状态成员；
没有活跃旧规则。受跟踪 diff 和新文件空白、修改脚本语法均通过，源码目录
没有新增构建产物。功能 C/C++/汇编、ABI golden 和音频策略相对各自新基线没有改动。

VMware 验证未完成，物理硬件未验证。三介质检查只覆盖启动至内核和 PID 1，
本次没有执行安装器格式化、安装及安装后启动的完整交互流程。
两仓提交与推送仍按子仓先行，再更新父仓 gitlink 的顺序进行。
