# ReliefOS Xorg/VMware 图形会话任务 — 交接文档

> 交接时间:2026-10-02(内核版本串 `ReliefNT reliefos 5.0.0 2026-10-02 06:36:36`)
> 本文只陈述任务、现状、证据与阻碍,不规定后续做法。

---

## 1. 任务与验收要求

**总目标**:在 Kconfig `CONFIG_DESKTOP_BACKEND_XORG=y` 下,修复 VMware 图形会话的启动、VT/TTY 所有权、输入路由与明显性能瓶颈,使 tty1 的 XDM/Xorg/TWM 成为唯一前台图形会话、tty2–tty6 保持独立文本终端,同时保持 Linux VT/KD/evdev、POSIX 进程/终端与错误返回语义。

任务方给出的已确认根因范围(原始规格):自动路径 `console-session tty1 -> reliefos-xdm -> xdm -> Xorg.wrap -> Xorg` 在 Xorg init 失败;`pty_vt_switch()` 缺 VT_RELDISP 状态机;全局 evdev 流需要正确的 VT 通知而非私有过滤;`svga_update()` 同步轮询 BUSY;任务退出日志重复;`reliefos-xdm` 的 `set -e`/`chmod 0622`/事件顺序问题。

三条工作线:
- **VT/input ABI**:`kernel/reliefnt/kernel/reliefnt/pty.c`、`syscall.c`、`input.c`、`include/uapi/linux/kd.h` + 测试 `tools/tests/linux_pty_test.c`、`tools/tests/xorg_vt_handoff_test.c`、`tools/tests/evdev_grab_test.c`、`tools/test_xorg_vt_handoff.py`、`tools/test_xorg_abi.py`,文档 `docs/TTY_VT.md`、`docs/ABI.md`
- **XDM/Xorg 会话线**:`system/rootfs/usr/lib/reliefos/console-session`、`system/xorg/reliefos-xdm`、`system/xorg/xdm-session`、`system/xorg/xdm-Xservers` + 测试 `tools/test_console_boot_policy.py`、`tests/build/test-desktop-backend.sh`、`tools/test_xdm_launcher.py`,文档 `docs/XORG.md`
- **性能线**:`kernel/reliefnt/drivers/bootstrap/framebuffer.c`、`svga/device.c`、退出日志去重 + 测试 `tools/tests/svga_test.c`、`tools/test_svga.py`、`docs/SVGA3D.md`

**验收要点**(逐字保留):
- tty1 图形化时无文本 getty 抢键盘/帧缓冲;tty2 输入绝不进入 TWM/xterm;
- Ctrl+Alt+F1/F2 触发 LeaveVT/EnterVT+repaint;
- VT/KD/EVIOCGRAB/PTY-OFD 通过标准 ABI 回归且无私有 ioctl;
- 自动 XDM 失败要给出具体 Xorg 致命阶段+日志并回落 tty1 文本登录;
- 成功要按真实顺序记录 PAM→TWM→xterm 生命周期;
- 普通帧缓冲 present 不再轮询 SVGA BUSY;
- **"只有完成独立镜像构建并实际执行 VMware 图形、输入和 VT 操作后,才宣称 VMware 下'正常使用';仅有源码/编译/QEMU 结果的部分明确标注未完成。"**
- 最终报告用简体中文,按项标注状态(源码已检查/已修改/已编译/已在虚拟机运行验证)。

---

## 2. 工作区与环境约束(仍然生效)

- **工作区**:本工作区是 `/home/xiaobai/Projects/Projects/ReliefOS_Xorg`(分支 `feature/xorg`)。另一个 Codex Agent 在 `/home/xiaobai/Projects/Projects/ReliefOS` 工作,两者独立,**只修改本工作区文件**。
- **`third_party/cmd`**:已按用户要求彻底移除(2026-10-03):submodule 注册、gitlink、检出目录、构建接线与全部现行引用一并清除,原先 index 里的 staged deletions 随之销毁。
- **reliefos-check-lock / fetch**:用户明示想办法破开 reliefos-check-lock,直接 make fetch;fetch 走代理 **端口 12334**。
- **TMPDIR**:`/tmp` 是几乎满的 tmpfs,构建/测试必须 `TMPDIR=/home/xiaobai/Projects/Projects/ReliefOS_Xorg/build/tmp`。
- **AGENT.md 约定**:内核代码 Doxygen `/** @brief @param @return */`;源码改动后跑 `git diff --check`;QEMU QMP socket 不放 /mnt/d 下;报告简体中文、结论先行;除非用户要求不提交(2026-10-03 用户已授权提交并推送本次改动)。
- 构建命令习惯形式:`make -C /home/xiaobai/Projects/Projects/ReliefOS_Xorg O=out/xorg-vmware-debug olddefconfig`(注意必须带 `-C`,shell cwd 会漂移)→ `make O=out/xorg-vmware-debug -j8 kernel userland image-vmdk`;全量门禁 `TMPDIR=…/build/tmp make O=out/xorg-vmware-debug test`。

---

## 3. 当前代码改动清单

### 3.1 superproject(`git status` 实测)

已修改:
- `docs/SVGA3D.md`、`docs/TTY_VT.md`
- `system/rootfs/usr/lib/reliefos/console-session`(tty1 图形会话排他/tty2-6 文本回落逻辑)
- `system/xorg/reliefos-xdm`(自动 XDM 生命周期:失败时记录 Xorg 致命阶段+日志并回落 tty1 文本登录;chmod/事件顺序修复)
- `system/xorg/xdm-session`(PAM→TWM→xterm 事件顺序;uid0 拒绝 `refusing to run TWM/xterm as root`;僵尸回收 `( : ) & wait $!`)
- `system/xorg/xdm-Xservers`(`/usr/bin/Xorg :0 -config /etc/X11/xorg.conf -nolisten tcp vt1 -keeptty`)
- `tests/build/test-deps.sh`、`tools/test_console_boot_policy.py`、`tools/test_svga.py`
- `tools/tests/descriptor_table_test.c`、`ioctl_cloexec_table_test.c`、`linux_pty_test.c`、`svga_test.c`
- `userland/apps/abittest/main.c`

新增(未跟踪):
- 测试:`tools/test_exit_logging.py`、`tools/test_xdm_launcher.py`、`tools/test_xorg_vm_qemu.py`、`tools/test_xorg_vt_handoff.py`
- C 测试:`tools/tests/evdev_grab_test.c`、`tools/tests/task_exit_logging_test.c`、`tools/tests/xorg_vt_handoff_test.c`
- 一次性诊断脚本:`tools/xorg_diag.py`、`tools/xorg_greeter_probe.py`

既有相关脚本(已跟踪,未改或此前已改):`tools/test_xorg_qemu.py`、`tools/test_xorg_abi.py`。

### 3.2 kernel/reliefnt 子模块(`git status` 实测)

- `drivers/bootstrap/framebuffer.c` — **已修**:`framebuffer_vmware_probe()` 在 SVGA REG_ENABLE=0 时保留 loader 模式(OVMF VBE 扫描输出几何),只在 device mode 有效且 ENABLE=1 时采用设备几何;修掉控制台文字重影(上电默认 640x480 pitch 2560 双绘)。
- `drivers/bootstrap/svga/device.c` — 异步 present(FIFO UPDATE + SYNC doorbell);REG_BUSY 轮询只留给 fence/mode-set/shutdown(普通 present 不轮询 BUSY)。
- `include/uapi/linux/kd.h` — VT/KD ABI 常量。
- `kernel/reliefnt/pty.c` — VT_RELDISP 状态机(`pty_vt_switch` 的 release pending/swtch、`pty_vt_commit_switch`、`pty_vt_set_mode/reset_mode`、acquire 信号);VT trace 日志(见 §6.1 的 trace 字符串)。
- `kernel/reliefnt/syscall.c` — VT/KD ioctl 分发(VT_SETMODE/VT_RELDISP/VT_ACTIVATE/KDSETMODE 等,含 ctty 或 CAP_SYS_TTY_CONFIG 门);FBIOPUT_VSCREENINFO 的 VT 门已移除,门只留给 `RELIEFOS_FBIOBLIT`(**已修**,Xorg 因此能起);退出日志去重。
- `kernel/reliefnt/input.c` + `include/reliefnt/input.h` — evdev 记录带 `graphical_vt` 标签、`input_evdev_read_vt(..., number)` 过滤、`input_set_graphical_vt()`。
- `kernel/reliefnt/sched/sched.c` + `include/reliefnt/sched.h` — `sched_set_task_identity`(uid0 → `cap_effective = cap_bset`;非 root 全零)、`task_clear_identity`。
- `kernel/exec/userland.c`、`include/reliefnt/pty.h`、`include/reliefnt/userland.h`、`tools/host/manifest/reliefos-deps.c` — 配套(退出日志、ABI 面、依赖清单)。

---

## 4. 构建与验证设施现状

- **构建产物**:`out/xorg-vmware-debug/`,`images/reliefos.raw` 与 `images/reliefos.vmdk` 均为 2026-10-02 21:12 构建(build4,含 VT trace + 上述全部内核修复)。`.config` 已确认 `CONFIG_DESKTOP_BACKEND_XORG=y`。
- **QEMU 验证通道**:`-vga vmware` + OVMF + snapshot=on + serial + QMP;`tools/test_installer_accounts_qemu.Probe`(click/key/text + settle sleep)、`wait_log`、`wait_text`、`ocr`(tesseract psm=6);帧 1280x800。guest 凭据:test/test(uid 1000)、root/root(来自 `system/test-accounts/` yescrypt 哈希)。无串口 getty,调试靠登录 tty1 文本登录后把命令 echo 到 /dev/ttyS0,用 `=== TAG BEGIN/END ===` 标记包住输出收集。
- **guest 内 XDM 栈**:Alpine `xdm-1.1.17-r1.apk`(注意文件名不是 `alpine-xdm-*`);xlogin 的 Return 绑定 `accept-login()` **编译在 libXdmGreet.so 内**,不依赖 app-defaults Login 文件(apk 只带 Chooser app-defaults,已实测不影响);两段式登录:Login 字段→Return→Password 字段→Return;横幅 "This is an unsecure session"。
- **PAM**:`etc/pam.d/xdm`(pam_nologin/pam_shells/common-auth=pam_faildelay+pam_unix/common-account/common-password/common-session);`unix_chkpwd` stage 为 4755。
- **xdm 配置**:`authDir /var/lib/xdm`、`errorLogFile /var/log/xdm.log`、`servers /etc/reliefos/xdm-Xservers`、`authorize: true`、`session /usr/lib/reliefos/xdm-session`、setup/startup/reset 全 `/bin/true`、`requestPort: 0`。
- **xorg.conf**:fbdev `/dev/fb0`、evdev event0/event1、`DontVTSwitch off`、`AutoAddDevices off`、DefaultDepth 24。
- **fixture 测试(全绿,已跑)**:test_svga 6 OK、test_xorg_vt_handoff PASS(含 EVIOCGRAB)、test_linux_pty PASS、test_exit_logging PASS、test_xdm_launcher 11 OK、test-deps 49/49。

---

## 5. 已解决 / 已验证项

1. **Xorg `FBIOPUT_VSCREENINFO: Operation not permitted` → AddScreen/ScreenInit failed**(已修):内核 FBIOPUT 门要求"控制中活动 VT"(Xorg 无 ctty),门已收窄到 `RELIEFOS_FBIOBLIT`。修复后 Xorg 完整起动并渲染 xlogin greeter(截图 `build/xorg-vm/g0.png` 等)。
2. **文本控制台文字重影**(已修):SVGA REG_ENABLE=0 时采用上电默认几何导致 pitch 错误双绘;framebuffer.c 区分 loader/device 几何后控制台干净。
3. **镜像配置错位**(已修):out 目录名是 xorg 但 .config 曾是 RELIEFOS 后端(greeter 实为 login.elf);已翻转 `CONFIG_DESKTOP_BACKEND_XORG=y` + olddefconfig + 重建。
4. **greeter 两段式输入被证实可用**:探针截图 `g1-after-name.png` 显示"Login: test"→Return 后 **Password 字段正常聚焦**。旧假设"缺 `/usr/share/X11/app-defaults/Login` 导致 Return 不翻页"已被推翻。
5. **auto XDM 失败回落**(脚本侧已实现):`reliefos-xdm` 记录 `xdm exit status=`/`xorg log present bytes=`/`tty1 restored to text login`;`xdm-session` 拒绝 uid0 会话(`refusing to run TWM/xterm as root`)。
6. **退出日志去重**、**异步 SVGA present** 代码已就位并编入镜像(性能验收项的代码部分)。

---

## 6. 现场证据与未解问题(核心阻碍)

### 6.1 悬案一:Xorg 的 VT_SETMODE(VT_PROCESS)/KDSETMODE(KD_GRAPHICS)从未到达内核

内核 VT trace 字符串(便于在 serial.log 里检索):`VT mode vt=%u mode=%d relsig=%d acqsig=%d`、`VT switch request %u -> %u`、`VT release signal sent for vt=%u`、`VT switch refused: vt=%u owns the display in graphics`、`VT active %u graphics=%d`、`VT acquire signal sent for vt=%u`、`VT graphics vt=%u enabled=%d`。

实测(`build/xorg-vm/serial.log`,15 条 VT trace 全列):
- **没有一条** `VT mode vt=1` 或 `VT graphics vt=1 enabled=1`——对比 tty2–tty6 的 `VT graphics vt=N enabled=0` 轨迹完全正常(30.66s 启动期与 92.9–95.1s X 重启期两批)。
- `44.769202 VT switch request 1 -> 2` 紧跟 `44.761307 VT active 2 graphics=0`(printk 时间戳微序颠倒属正常),**中间没有任何 release/acquire 信号**——vt1 的 vt_mode 仍是 VT_AUTO,Ctrl+Alt+F2 是瞬时切换,LeaveVT/EnterVT+repaint 握手不存在。
- 可见后果:控制台把键入文本回显画进 X 帧缓冲(帧左上角残留 "test");KD_GRAPHICS 从未生效。

嫌疑集中于 `kernel/reliefnt/kernel/reliefnt/syscall.c:6875-6943`(VT ioctl 分发):`task_pty_endpoint_for_fd`(syscall.c:1106)只认 `TASK_PTY_ENDPOINT_SLAVE` 端点、`/dev/ttyN` open 走 `task_pty_endpoint_path`(syscall.c:5241+)、以及 `task->controlling_pty_id != id` + `CAP_SYS_TTY_CONFIG` 门三者如何对 Xorg(无 ctty 的服务进程,fd 来自 `/dev/tty1` 或 `-keeptty`)成立。注意 `sched_set_task_identity`(sched.c:3290)对 uid0 赋全能力(`cap_effective = cap_bset`),若 Xorg 以 root 运行则 CAP 门本不该拦;失败点也可能根本不在这道门(例如 fd 未解析为 SLAVE 端点时直接 ENOTTY,ioctl 根本没进 VT 分支)。**本机没有现成的 Xorg 侧 errno 证据**——Xorg.0.log 对 VT_SETMODE/KDSETMODE 的失败沉默,是否被 xorg 静默吞掉未确认。

### 6.2 悬案二:输入跨 VT 泄漏(tty2 键击进入 tty1 的 xlogin)

`/tmp/xorg-probe.log` XDM-LOG 段的事件序列:
```
xdm info (pid 271): Starting X server on :0
xdm error (pid 271): cannot make authentication file /var/lib/xdm/authdir/authfiles/A:0-XXXXXX: Invalid argument
… Xorg 正常起动,xkbcomp 警告 …
xdm info (pid 277): sourcing /bin/true          (Xsetup_0)
xdm error (pid 277): pam_authenticate failure: Authentication failure
xdm info (pid 277): sourcing /bin/true
xdm info (pid 286): executing session /usr/lib/reliefos/xdm-session
xdm-session: refusing to run TWM/xterm as root  ← 我们的 root 拒绝保护挡下
xdm info (pid 277): sourcing /bin/true          (Xreset)
xdm info (pid 271): Starting X server on :0     ← X server 重启
```
serial.log 侧的 unix_chkpwd 时间线(`build/xorg-vm/serial.log:1004-1023` 附近):
- `46.690 unix_chkpwd[279]: password check failed for user (test)`(pid 279 被 277=xdm 收割)——xlogin 里 test 用户一次失败认证;
- `57.1-57.2` 同时起两个 unix_chkpwd:**280 被 pid 260(tty2 的 console-session)收割、281 被 pid 277(xdm)收割,双双 exit 0**;
- 随后 xdm 以 **root** 身份执行了 xdm-session(被脚本拒绝)。

即:tty2 上输入的 root/root 同时喂给了 tty2 的登录和 tty1 的 xlogin——两个 PAM 消费者在 80ms 内各自收到同一串键击。这与 input.c 的 `graphical_vt` 标签/`input_evdev_read_vt` 过滤/全局 evdev 流归属直接相关,是"tty2 输入绝不进入 TWM/xterm"验收项的失败现场。

### 6.3 悬案三:xdm auth 文件 mkstemp EINVAL

`cannot make authentication file /var/lib/xdm/authdir/authfiles/A:0-XXXXXX: Invalid argument`,每次 X server 启动都出现。xdm 对此只是 error 后继续(authorize: true 时的后果未评估)。`xorg_diag.py` 里的 MKTEMP-TEST 段**未捕获到输出**(`/tmp/xorg-diag.log` 无 MKTEMP 行,探针日志也没有),该问题现场材料不全。

### 6.4 悬案四(噪音项,定性未完)

Xorg.0.log(`/tmp/xorg-probe.log` XORG-LOG 段)同时存在:
- `Keyboard0 evdev: device file is duplicate. Ignoring.` / `PreInit returned 8`(xorg.conf 显式 evdev + 默认键盘布局重复);
- `couldn't open module kbd`、libinput 缺失、`[config] failed to initialise udev`;
- `FBIOBLANK: Not a tty` 与大量 `FBIOPUTCMAP: Not a tty`(我们的 fb ioctl 对这两类请求返回 Not a tty;Linux fbdev 语义下它们不该受 VT/终端约束——与 §3.2 里 FBIOPUT 门同族的历史遗留,影响色彩映射与屏保,不影响 greeter 渲染)。
- X 重启的连带效应:X server 重启瞬间 tty3–tty6 的 console-session/login 全部 exit 0 后被 `Scheduling for restart`(serial.log 92.9–95.1),原因未定位。

### 6.5 测试脚本自身限制

- `tools/test_xorg_vm_qemu.py`(主验收场景脚本)的 `greeter_login()` 目前用 `wait_text(..., "password:")` 等待密码阶段,**必然超时**:`g1-after-name.png` 明明显示密码框已聚焦,但 tesseract(psm=6)读不出粗体衬线 "Password:" 标签(实测 OCR 输出为 `'test\nThis is an unsecure session\nLogin: test\nN\n'`)。该脚本因此停在 greeter 密码等待,**尚未跑通完整场景**。GREETER_NEEDLE="unsecure"(会话开始后消失)可用;`wait_session` 用亮像素 >2% of 160x90 判图形会话可用。
- `tools/test_xorg_qemu.py`(生命周期顺序校验)期望的事件序列:`desktop-backend=xorg → xdm started on vt1 → PAM authentication accepted → twm started for uid≠0 → xterm started → xdm session ended → tty1 restored to text login`,用法 `python3 tools/test_xorg_qemu.py --log build/xorg-vm/xdm-evidence.log --image out/xorg-vmware-debug/images/reliefos.vmdk`,尚未跑过。

---

## 7. 证据与产物文件索引

| 位置 | 内容 |
|---|---|
| `build/xorg-vm/serial.log` | 完整串口日志(15 条 VT trace、unix_chkpwd 时间线) |
| `build/xorg-vm/*.png` | greeter 各阶段截图:g0、g1-after-name、g1b-after-tab、tty2-after-switch、probe-tty2-prompt/password/shell、last-frame |
| `/tmp/xorg-probe.log` | greeter 探针运行日志(902 行;XDM-LOG/XORG-LOG 段、OCR 文本) |
| `/tmp/xorg-diag.log` | 诊断日志(VARLOG-LIST/XDM-LOG/XORG-LOG/XORG-FIRST;无 MKTEMP 段) |
| `/tmp/xorg-build4.log` | 最近一次重建成功日志 |
| `tools/xorg_greeter_probe.py` | 一次性交互探针(逐键输入 + OCR 快照 + 收集 XDM-LOG/XORG-LOG) |
| `tools/xorg_diag.py` | 一次性诊断脚本 |
| `out/xorg-vmware-debug/images/reliefos.{raw,vmdk}` | 当前镜像(21:12 build4) |

---

## 8. 报告与交付要求(最终产出格式)

- 简体中文、结论先行;逐项标注:源码已检查 / 已修改 / 已编译打包 / 已在 QEMU 验证 / 已在 VMware 验证。
- **VMware 相关一律标注"未完成"**——从未做过独立 VMware 实机验证(图形、输入、VT 操作),QEMU 结果不得表述为"VMware 下正常使用"。
- 收尾检查:`git diff --check`、`git status --short`(superproject + kernel/reliefnt 子模块)。
- 2026-10-03 用户已授权提交并推送本次改动;除此之外不主动提交。
- 环境适配需在报告中注明:reliefos-check-lock 绕过并直接 `make fetch`(代理 12334)。
