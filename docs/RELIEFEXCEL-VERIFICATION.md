# ReliefExcel 验证记录

2026-10-17。本记录对应 ReliefExcel 首版实现及相关测试。没有修改内核、XDM/IceWM 主题、音频支持声明或 ABI golden。

## 编译

- C++17，沿用 FLTK 1.3、musl 及现有 C++ ABI；禁用 exceptions/RTTI。
- `TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug -j4 app-reliefexcel app-fileman nls`：exit 0。
- 目标 ELF 及翻译的 SHA-256 记录在每次 QEMU 测试的 `prepare.json`，包含注入后读回核对。
- 最终 ReliefExcel ELF SHA-256：`ac5976fc2c6547cfa8ba99dcfab9b465bd10f7b151e1628931b152c4b818a6b0`。
- UI 源文件额外通过主机 `-Wall -Wextra -Werror -fsyntax-only` 检查；目标构建仍有既有上游 musl/mimalloc 警告。

## 主机测试

- `tests/build/test-reliefexcel.sh`：storage / model / formula / document 四组 ASan/UBSan 全部通过。
- 回归覆盖：文本/数组别名扩容、UTF-8、稀疏编辑和历史、样式局部修改、排序边界、筛选、结构变更、引用变换、跨表/重叠剪切、撤销重做、取消原子性、SUMIF 公式区域、通配符转义、惰性 IF、循环和错误值、损坏文件、保存冲突、CSV 引号/换行/前导零/公式形似文本、恢复目录和独立恢复文件预留。
- components、APK ownership、installer stage、Fileman boundary、FLTK apps 集成测试通过；另外独立审查执行的 rootfs stage / APK stage selection 通过。
- 没有把音频/ABI 等既有失败项视为本次通过项。
- 恢复文件使用独立随机名称，预留回归验证旧 PID 命名文件不会被覆盖；sidecar 的 POSIX `F_SETLK` 锁通过 fork 竞争、释放重取、权限检查和符号链接保护回归。

## 性能

固定 10,000×20 数据单元格，另加三个跨表汇总公式。优化前主机原生保存/加载约 6.31/1.43 秒；32 KiB 缓冲后约 0.045/0.087 秒。整个基准的 write/read 系统调用从各约 280 万次降至 570/220 次。数字属于该主机的优化构建，不代表 VMware 性能。

`tools/tests/reliefexcel_scale_benchmark.cpp` 可重建 CSV/native 固定样本。GUI 验收使用 debug 目标程序，`qemu-delivery` 打开大表至 ready 约 3.91 秒（含测试启动器等待）。

## 运行

采用私有磁盘副本，注入目标 ELF/翻译并逐文件哈希读回；原磁盘不写入，未重建发布 ISO。

- `build/reliefexcel-dev/qemu-ui-2`：中文模板、跨表汇总、中文剪贴板、SUM、绝对/相对引用复制、公式栏 Esc、限定矩形排序、筛选清除、冻结、保存重开。取回 `.rexl`：3 张表、85 个单元格，独立验证器通过。
- `build/reliefexcel-dev/qemu-smoke/result.json`：自动打开 200,006 个单元格，修改、保存、读回验证通过；原始镜像哈希不变。
- `build/reliefexcel-dev/qemu-final`：剪切后内部/外部引用跟随、撤销/重做；SIGKILL 后恢复并保存，取回 3 个单元格的文件验证通过。
- `build/reliefexcel-dev/qemu-acceptance`：启动恢复提示与恢复文件回读通过；完整编辑核对发现短操作中的事件轮询会吞掉连续输入。随后把进度事件轮询限制为最多每 80 ms 一次，短编辑不在提交中处理后续输入。
- `build/reliefexcel-dev/qemu-delivery/result.json`：上述修复后的完整自动验收通过。中文模板、剪贴板、SUM、绝对/相对引用复制、公式栏 Esc、连续快速输入 1–9、限定矩形排序、筛选清除、冻结、保存重开均完成。独立回读验证 3 张表、94 个单元格；另验证修改保存后的 200,006 个单元格。源镜像哈希不变。截图见该目录的 `ledger.png`、`summary.png`。
- `build/reliefexcel-dev/qemu-recovery-unique`：目标系统内写入随机名称、0600 权限的恢复文件，取回 3 个单元格独立验证通过。
- `build/reliefexcel-dev/qemu-recovery-lock/result.json`：恢复副本及 sidecar 写入成功；原实例仍持锁时，第二实例正常打开空白工作簿，没有提示或接管活跃副本。随后强制结束原实例，保留副本供最终构建恢复验证。
- `build/reliefexcel-dev/qemu-final-recovery/result.json`：最终 ELF 启动后自动显示恢复提示；恢复、另存为、主机取回文件核对均通过（3 个单元格，值为 5、6、11）。正常保存后恢复目录为空，副本与锁均已清理。Ctrl+Alt+F2 切换控制台再返回，不会开始单元格编辑。最终中文模板截图：该目录的 `ledger.png`、`summary.png`。

完整编辑/大表验收使用 `f0850ab258a68682410ebb260c12e4299030318fe54193eefef91c8f15ca811b`；其后的更改限定为恢复副本预留、跨进程锁和清理，以及 F2 修饰键判断。最终构建额外执行了对应恢复/切换控制台验证，未把早期完整验收误报为对最终 ELF 的重复全量验收。

验收工具：`tools/test_reliefexcel_qemu.py`；独立文件内容核对工具：`tools/tests/reliefexcel_guest_verify.cpp`。测试桌面通过 QEMU vmware VGA 运行，不等于真实 VMware 验证。

**VMware 验证未完成。**
