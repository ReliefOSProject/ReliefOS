# Kernel panic 文字控制台

致命异常和 `bugcheck_panic()` 统一输出文字诊断，不再绘制旧的 GUI 面板。

- 文本 VT：保留当前 VT 编号，从当前光标下一行输出，必要时按正常终端规则滚屏。
- 图形 VT：保留 VT 编号和原有桌面，从左上角第一行直接绘制文字。仅文字和空格占用的字符格覆盖黑底，其余 framebuffer 像素不变。无需 Xorg、窗口管理器或用户态终端参与。
- 图形 panic 到达屏幕底部时停止绘制，避免滚动或复制原有 GUI；完整诊断仍写入串口和当前 VT 历史。文本 VT 保持原有滚屏行为。
- 红色标题和原因、黄色异常解释、白色寄存器；每个新绘制字符的背景固定为黑色。串口和当前 VT 历史保留受控 ANSI SGR 属性，诊断字符串中的控制字符会被转义为问号。

输出包含异常向量和错误码、RIP/RSP/段寄存器/RFLAGS、CR0/CR2/CR3/CR4、缺页错误位及解释、完整 trap frame 中的 15 个通用寄存器、CPU/APIC、其他 CPU 停止数量、任务/PID、运行时间、内核版本、源码 ID 和构建时间。普通 scalar 异常入口未保存的通用寄存器会明确标注不可用；显式 panic 的 RIP 表示调用者。NMI、双重错误和 machine check 跳过任务及时间采集。没有无边界解引用栈帧的自动回溯；开发者可用该次构建的 `kernel.unstripped` 解析 RIP。

紧急路径关闭本地中断，只向已在线的其他 CPU 发送 NMI，并有界等待它们停机。随后绕过普通日志、驱动注册、显卡锁和动态内存分配。SVGA II 恢复 GFB tracing 并用有限次数的 FIFO 尝试发布画面；启动时已被禁用的 framebuffer 不会被重新启用，改走 VGA/COM1。内核地址范围以外的致命缺页直接进入 panic，支持的用户地址范围内的 demand paging/COW 和用户信号处理保持原有路径。

如果 CPU 无法响应 NMI、显卡已经失效或诊断代码/内存本身损坏，显示仍只能尽力完成；CPU 停止计数和串口可帮助判断。程序不会自动重启。

## 验证

以下命令均从 ReliefOS 仓库根目录运行，`TMPDIR` 放在 `out/`。

```sh
mkdir -p out/tmp
TMPDIR="$PWD/out/tmp" make -C kernel/reliefnt O="$PWD/out/panic-host" test-panic-console
TMPDIR="$PWD/out/tmp" make -C kernel/reliefnt O="$PWD/out/panic-sanitised" \
  HOST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  HOST_LDFLAGS='-fsanitize=address,undefined' test-panic-console
TMPDIR="$PWD/out/tmp" python3 tools/test_console_boot_policy.py
```

宿主测试编译实际 console、bugcheck、framebuffer 和 boot-logo 绘制代码，在带 guard 与非整齐 pitch 的 24/32 位 RAM framebuffer 上检查当前 VT、黑底首行、未覆盖的 GUI 像素、字符与空格黑底、显式换行/自动换行溢出裁剪、原有 TTY 内容、诊断寄存器、ANSI 控制字符、不可用字段、禁用映射以及 FIFO 成功/阻塞/非法索引；panic 阶段普通显卡锁、串口接口或分配器被调用会直接失败。

构建好带 Xorg 的 RAW 镜像后，依次运行：

```sh
TMPDIR="$PWD/out/tmp" python3 tools/test_panic_console_qemu.py \
  --build-dir out/kbuild-verified --output out/pt-check --mode tty
TMPDIR="$PWD/out/tmp" python3 tools/test_panic_console_qemu.py \
  --build-dir out/kbuild-verified --output out/pg-check --mode gui --hold-display-locks
```

每次仅启动一个 4 GiB、双 CPU 的 QEMU/KVM q35 + SVGA II 客机。脚本复制 RAW，合并公开的 test/test 测试账户，回读核对新内核，然后正常进入 XDM/TTY2 或 IceWM。GDB 只修改私有虚拟机内存，在 `console_display_tick` 入口注入一次访问未映射地址的指令；产品内核没有增加 panic 测试后门。GUI 用例还将普通 console/SVGA 锁标成占用，证明紧急输出不等待它们。测试核对当前 VT、全部缺页诊断、其他 CPU 停机计数、真实截图 OCR，以及停机后画面不被覆盖；源镜像前后哈希必须相同。

2026-10-21 初版全屏黑底验证记录（后续覆盖模式的记录见下文）：

- `out/panic-text/test-native.log`、`test-sanitised.log`：实际生产代码宿主测试各 107,201 项检查通过，ASan/UBSan 无报错。
- `out/panic-text/test-console-policy.log`：原有 14 项控制台测试通过；补齐其对实际 boot-logo painter 和 CPU 数量接口的链接。原有 boot-logo RAM 测试 9,696,607 项检查通过。
- `out/panic-text/kernel-build.log`：Kbuild 内核构建成功。`image-vmdk-build.log`：RAW/VMDK 构建成功；正式 RAW 内核回读与上述客机测试二进制相同，`image-compare.log` 确认 RAW/VMDK 内容一致。
- `out/ptf/result.json`、`out/pg/result.json`：最终 TTY2 / IceWM 故障注入均通过，另一颗 CPU 停机计数均为 1/1，画面稳定，私有测试源镜像在各自测试期间保持不变。GUI 场景包含占用普通 console/SVGA 锁的故障模拟。
- 两场景测试内核 SHA-256 均为 `a0b8e95683dddce1907ef2a0c6bb4ddfa9f2523af26f14846578d7472efd259b`。截图分别为 `out/ptf/panic.png`、`out/pg/panic.png`；汇总及正式镜像回读在 `out/panic-text/verification.json`。`out/pt` 为地址保护补强前的首次 TTY 验证，最终证据采用 `out/ptf`。

2026-10-21 按新要求调整为字符格黑底覆盖模式：

- `out/panic-overlay/test-red.log`：先确认旧实现会破坏未覆盖的 GUI 像素；新实现通过 `test-native.log`、`test-sanitised.log` 各 124,345 项检查，ASan/UBSan 无报错。新增用例核对绘制前不清屏、文字/空格黑底、其余像素及 pitch/guard 不变、小屏幕换行和自动折行不滚动 GUI，以及文本 TTY 继续正常滚屏。
- `out/panic-overlay/test-console-policy.log`：14 项原有控制台回归通过。
- `out/panic-overlay/kernel-build.log`、`image-vmdk-build.log`：内核及 RAW/VMDK 构建成功；镜像内核回读匹配，RAW/VMDK 内容一致。
- `out/pgo/result.json`：IceWM 中真实缺页异常、普通 console/SVGA 锁占用场景通过，其他 CPU 停机 1/1。GDB 在其他 CPU 已停机且尚未绘制 panic 时捕获原画面；逐像素核对文字区域之外的 **876,288 个像素**全部保持不变，其中 **227,837 个非黑色 GUI 像素**被保留。字符格内仅包含黑底及诊断文字颜色，画面停机后稳定。
- 当前截图：`out/pgo/panic.png`；汇总：`out/panic-overlay/verification.json`。当前内核 SHA-256：`8baae2515f68a897890b60dcf31e6c235ecc463a77eca8ca12c4133883f45ff5`。

以上验证在提交前完成，测试报告中的源码 ID 为当时工作树基线 `eb87965`；已验证二进制以上一段 SHA-256 为准。VMware 验证未完成；物理硬件未验证。
