# TTY / VT 改造交接与验收记录

> 历史验证记录：以下路径、命令和结果对应记录时的构建链。当前 Kbuild 接口与验收见 [Kbuild 迁移记录](build/kbuild-migration.md)。

更新日期：2026-09-23。分支：`feature/new-tty`。功能与接口说明见 [TTY_VT.md](TTY_VT.md)。

改造与 QEMU 验收已完成。本文件替代此前的暂停交接；默认 GRUB、图形登录、VT_WAITACTIVE 及实际安装/启动均已验证。

## 已完成的改造

- 永久 tty1～tty6 复用 PTY 的 termios、控制终端、会话、前台进程组和挂断逻辑；动态 pts 从 7 开始，串口独立。
- Ctrl+Alt+F1～F6、每 VT 64 KiB 文本历史、VT_GETSTATE/ACTIVATE/WAITACTIVE、KDGETMODE/KDSETMODE、权限检查及 leader 退出恢复文本。
- init 管理六路 console-session。tty1 按 desktop-session 标记启动一次图形会话，桌面结束后进入 getty；其他 VT 使用认证登录。
- 删除旧 LEONOS_BOOT_MODE/startup 分叉、桌面/安装器 OpenRC 重复启动和旧图形启动画面。安装器沿用同一 VT 架构；安装介质的文本 VT 提供本地救援 shell，安装后恢复普通 getty。
- 图形 login 与 installer 显式选择图形入口，避免继承 tty1 stdin 后误进文本模式。
- 补齐 `/proc/self/fd/N` 的已命名文件/终端链接，使 musl ttyname 与 BusyBox tty 能识别 ttyN。
- 默认 EFI 从仓库固定 GRUB 模块生成；不依赖旧 out 目录中的临时 EFI。

## 审查中修复的关键问题

1. 隐藏旧文本光标后再输出/重放，避免首字符被光标恢复逻辑覆盖。
2. 键盘 IRQ 只排队，VT 切换和历史重放在内核 execution transaction 中执行。
3. 图形提交改为 LEONOS_FBIOBLIT，在同一事务中校验活动控制 VT、用户源地址并复制像素；inactive 的 EAGAIN 直接返回。
4. evdev 按事件产生时的图形 VT 过滤，避免暂停的 windowd 重放文本密码；切回时同步按钮、坐标和修饰键状态。
5. 显示代数记录连续切出/切回，即使 Desktop 暂停期间错过状态变化也会完整重绘。
6. Desktop 在文本 VT 激活时继续处理窗口 IPC。IPC 短写保留帧尾并通过 flush 续传，防止 socket 填满后半帧超时丢失，造成返回桌面后输入永久失效。新增回归在修复前失败，修复后通过；QEMU 将桌面暂停 8 秒后仍可登录。

已完成独立代码审查并修复发现的阻塞问题。未通过删除认证、取消安全输入或放宽 VT 权限规避失败。

## 验证证据

所有路径均相对仓库根目录；build/out 是本地生成物，不随源码提交。

| 范围 | 命令/证据 | 结果 |
| --- | --- | --- |
| 正式构建 | `make -j8 all`；`build/vt-all-build-verified.log`、`build/vt-all-build-docs.log` | 通过，kernel/userland/runtime/SDK/APK/raw/VMDK/ISO/installer 已生成 |
| 完整构建契约测试 | `make -j8 test`；`build/vt-make-test-verified.log` | 通过，退出码 0 |
| 定向回归 | `python3 -m unittest tools.test_console_boot_policy tools.test_runtime_responsiveness tools.test_procfs_taskmgr tools.test_installer_setup tools.test_installer_input`；`build/vt-regression-verified.log` | 32 项通过 |
| IPC Linux 参考 | `python3 tools/test_unix_ipc.py`；`build/vt-ipc-linux.log` | 通过，真实 Unix stream/SCM_RIGHTS、凭据和断线语义 |
| PTY/rootfs/ioctl | `tools/test_linux_pty.py`、`tools/test_linux_rootfs.py`、`tools/test_linux_ioctl_cloexec.py` | 通过；rootfs 10 项、ioctl Linux 参考 38 项，未把 ioctl 的未运行 guest 分支算通过 |
| 普通镜像 | `VT_OUTPUT=build/vt-qemu-ipc-fixed python3 tools/test_vt_qemu.py` | 通过：图形认证、F1～F6、文本认证、stdio/tty、STOP/CONT 后恢复、桌面退出恢复 getty、退出重生 |
| 纯文本镜像与 guest 探针 | `build/vt-fixture-verified/disk.raw`；`build/vt-qemu-text-verified.log` 与对应 serial.log | 通过：六路 getty、真实 syscall 权限/等待/前台组/控制终端/代数/绘制错误路径 |
| 安装器 TUI | `python3 tools/test_vt_installer_qemu.py --output build/vt-installer-tui-verified --tui` | 通过：六路 shell、安装器文本入口及磁盘选择 |
| 安装器 GUI 实际安装 | `python3 tools/test_vt_installer_qemu.py --output build/vt-install-acceptance-final --install`；同名 .log | 通过，退出码 0；完成格式化、系统复制、账户配置和 EFI 安装 |
| 安装后启动 | `python3 tools/test_vt_installed_qemu.py --disk build/vt-install-acceptance-final/scratch.raw --output build/vt-installed-acceptance-final`；同名 .log | 通过：EFI 磁盘启动、六路 VT、root/普通用户认证、UID 1000、tty2、返回图形界面 |

普通镜像截图在 `build/vt-qemu-ipc-fixed/`，包括暂停后重绘、图形登录成功、tty1 桌面退出和 tty2 退出后的 getty。安装测试仅创建和格式化各自新建的 4 GiB scratch.raw；没有操作主机磁盘。

## 旧输出目录的增量构建兼容

旧 `O=out` 的 `kernel.c.o.d` 仍可能引用已删除的 `boot_splash.h`，导致 Make 在重新编译前报缺失目标。`mk/kernel.mk` 为该旧头文件保留空依赖目标，并为 C、汇编启用 `-MP`；依赖参数纳入编译签名，使旧目录自动重新生成依赖文件，无需清理输出目录。

`tests/build/test-incremental.sh` 新增旧依赖迁移、后续头文件删除和恢复后无重复编译/链接的回归。原规则在旧依赖迁移用例中复现了同一错误，日志为 `build/vt-stale-dependency-red.log`。

## 提交与保留边界

- `837a932 refactor: 删除旧版图形启动画面`（上一 Agent）。
- `14574c1 fix(boot): 使用仓库固定的 GRUB EFI 模块生成镜像`。
- `98da7a4 fix(ipc): 保留非阻塞连接的短写帧并支持续传`。
- `832a4ef feat(tty): 统一六路虚拟终端与图形安装会话`。
- 后续 test(tty) 提交保存验收脚本与本记录；未推送远端。

仍有明确边界：VT_SETMODE 的进程式 release/acquire 协议未实现；文本历史有 64 KiB 上限；原始 fbdev mmap 不支持 VT 撤销，桌面必须使用原子提交接口；匿名 pipe/socket 的完整 procfs magic-link 语义不在此次范围内。运行验证平台为 QEMU/KVM、host CPU、OVMF、标准 VGA，尚未进行 VMware 验收。
