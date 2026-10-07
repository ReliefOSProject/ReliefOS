# Xorg + TWM + XDM 桌面后端实现计划

> **历史记录（2026-10-02）**：本文是 Xorg 桌面后端首次落地时的设计/实现记录，
> 文中描述的 TWM 会话与 Kconfig 后端选择均已不再是当前系统。当前默认窗口管理器为
> **IceWM**，会话与配置见 [`docs/XORG.md`](../../XORG.md)（`system/xorg/icewm/`、
> `ICEWM_PRIVCFG=/etc/reliefos/icewm`）。本文仅保留作历史存档。


> **面向 AI 代理的工作者：** 必需子技能：使用 subagent-driven-development（推荐）或 executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 在 `feature/xorg` 分支为 ReliefOS 增加默认原生桌面与 Alpine 官方 Xorg + TWM + XDM 后端的互斥选择，并让 XDM 登录成功后进入 TWM。

**架构：** Kconfig 产生唯一的后端选择；rootfs staging 将选择转换为不可执行的单行 policy marker、对应的 OpenRC 链接和 XDM/Xorg 配置。Xorg 模式让 tty1 的 `console-session` 以前台方式执行 `xdm -nodaemon -config ...`，XDM 负责 Xorg 登录并调用 TWM 会话，安装器 runtime 在 APK staging 前强制恢复原生 policy。内核子仓补齐 Xorg 所需的标准 Linux `/dev/tty0`、VT 和键盘 ioctl ABI。

**技术栈：** Kconfig/kconfig-frontends、GNU Make、POSIX `/bin/sh`、Alpine v3.24 x86_64/musl APK、BusyBox init/OpenRC/PAM、Linux fbdev/evdev/VT UAPI、C ABI probes、QEMU/UEFI。

**规格：** `docs/superpowers/specs/2026-10-02-xorg-desktop-design.md`

## 全局约束

- 新分支名为 `feature/xorg`，基线为当前 `main`。
- 默认必须是 `CONFIG_DESKTOP_BACKEND_RELIEFOS=y`；原有 `windowd`、`desktop.elf`、`sessiond` 启动行为保持不变。
- Xorg 后端符号为 `CONFIG_DESKTOP_BACKEND_XORG=y`，用户可见名称为 `Xorg + twm + xdm`；两个符号由 Kconfig `choice` 互斥。
- Xorg 使用 Alpine 官方 v3.24 x86_64/musl 包和同一 APKINDEX 解析出的完整依赖闭包；直接包包括 `xorg-server`、`xorg-server-common`、`twm`、`xdm`、`xterm`、`xf86-video-fbdev`、`xf86-input-evdev`、`xkeyboard-config`、`font-cursor-misc`、`font-misc-misc`。
- 只有 `make fetch` 可以联网；普通构建只消费已校验缓存并在缺包时提示具体 ID 和 `make fetch`。
- lock entry 的 `feature` 只能是 `base` 或 `xorg`，缺省为 `base`；未选 Xorg 时最终 rootfs 不得含任何 `feature=xorg` APK。
- Xorg 只使用 `/dev/fb0`、`/dev/input/event0`、`/dev/input/event1`、`/dev/tty0`/`tty1` 和标准 Linux/POSIX ABI；不得新增 Xorg 私有 syscall/ioctl 或通过脚本绕过 VT ABI。
- 任何 shell 脚本使用 POSIX `/bin/sh`；用户会话脚本不能向 root-only 日志写入，密码、Cookie 和 XDM authorization 不得出现在日志中。
- 每次源码修改后运行 `git diff --check`；分别报告源码检查、编译/打包、镜像生成和 QEMU 运行结果。

## 审查重点（Review Focus）

- **内核 VT ABI 不完整：** Xorg 打开 `/dev/tty0` 后调用 `VT_OPENQRY`、`VT_GETMODE/SETMODE`、`VT_RELDISP`、`KDGKBMODE/KDSKBMODE` 应成功，并在退出时恢复 `KD_TEXT`/`VT_AUTO`；由内核 ABI 测试和 QEMU Xorg 启动测试钉住。
- **后端 policy 混淆：** 安装后 Xorg root 保留 `xorg` marker 和 Xorg APK，installer runtime 必须恢复 `reliefos` marker、inittab 和原生 runlevel；由 rootfs/installer fixture 测试钉住。
- **APK 依赖遗漏：** 每个 Xorg ELF 的 DT_NEEDED 必须有唯一 provider，APK 数据库和签名 transaction 必须包含完整闭包；由 lock closure、offline stage 和 ELF provider 测试钉住。
- **XDM/TWM 会话身份与日志：** PAM 认证后的用户会话必须启动 TWM，不能因为用户无权写 `/var/log` 而退出；由 XDM session contract 和 QEMU 登录测试钉住。
- **启动失败快速 respawn/设备争用：** tty1 同一 `console-session` 前台阻塞在 xdm，失败后 graphical marker 导致后续进入 getty，且不会同时启动 windowd/sessiond；由启动脚本测试和 QEMU 失败恢复测试钉住。

## 文件结构与职责

**修改：**

- `Kconfig`、`configs/default.conf`：桌面后端 choice 与默认值。
- `tools/host/manifest/reliefos-deps.c`、`tests/build/test-deps.sh`、`configs/dependencies.lock.json`：`feature` 字段校验/查询和 Alpine Xorg/TWM/XDM 完整锁闭包。
- `mk/apk.mk`、`tools/build/apk-stage.sh`、`tests/build/test-apk-stage-selection.sh`：按 raw root 后端 marker 选择 `base` 或 `base+xorg` APK，并保持签名、数据库和所有权。
- `mk/rootfs.mk`、`tools/build/rootfs-stage.sh`、`system/rootfs/usr/lib/reliefos/console-session`、`tests/build/test-desktop-backend.sh`、`tools/test_console_boot_policy.py`：生成 backend marker、条件服务链接和 tty1 XDM 启动/失败恢复。
- `tools/build/installer-stage.sh`、`tests/build/test-installer-stage.sh`：为 installer runtime 恢复 native marker/inittab/runlevels 并移除 Xorg 专用 policy。
- `kernel/reliefnt` gitlink 及其子仓 `drivers/bootstrap/storage/storage_vfs.c`、`kernel/reliefnt/pty.c`、`kernel/reliefnt/syscall.c`、`kernel/reliefnt/include/reliefnt/pty.h`、`include/uapi/linux/vt.h`、`include/uapi/linux/kd.h`：标准 `/dev/tty0` 与 VT/键盘 ioctl 实现。
- `docs/ABI.md`、`docs/APK_PREPARATION.md`：ABI、官方包来源和离线构建说明。

**创建：**

- `tests/build/test-desktop-backend.sh`、`tests/build/test-apk-stage-selection.sh`：shell contract fixtures。
- `system/xorg/xorg.conf`：固定 fbdev/evdev/vt1 配置。
- `system/xorg/reliefos-xdm`、`system/xorg/xdm.conf`、`system/xorg/xdm-Xservers`、`system/xorg/xdm-session`、`system/xorg/twmrc`、`system/xorg/pam-xdm`：XDM 前台包装、Xservers、用户会话、TWM 菜单和 PAM policy。
- `tools/tests/xorg_vt_abi_test.c`、`tools/test_xorg_abi.py`：标准 VT/键盘/fbdev/evdev ABI probe。
- `tools/test_xorg_qemu.py`：QEMU 中 XDM 登录→TWM→退出和失败恢复的运行验证。

### 任务 1：增加互斥桌面后端 Kconfig

**文件：**
- 修改：`Kconfig`
- 修改：`configs/default.conf`
- 创建：`tests/build/test-desktop-backend.sh`

- [ ] **步骤 1：编写失败的 Kconfig contract test**

测试必须检查以下固定符号和默认值，并用真实 Kconfig front-end 验证 choice：

```sh
grep -q '^config DESKTOP_BACKEND_RELIEFOS$' Kconfig
grep -q '^config DESKTOP_BACKEND_XORG$' Kconfig
grep -q '^CONFIG_DESKTOP_BACKEND_RELIEFOS=y$' configs/default.conf
! grep -q '^CONFIG_DESKTOP_BACKEND_XORG=y$' configs/default.conf
make -s O="$work/default" defconfig
grep -q '^CONFIG_DESKTOP_BACKEND_RELIEFOS=y$' "$work/default/config/.config"
```

另外生成只包含 `CONFIG_DESKTOP_BACKEND_XORG=y` 的 fixture，运行 `olddefconfig`，断言最终 `.config` 只保留 Xorg choice。

- [ ] **步骤 2：运行测试确认失败**

运行：`sh tests/build/test-desktop-backend.sh`

预期：FAIL，指出缺少两个 Kconfig 符号或默认 choice。

- [ ] **步骤 3：实现 Kconfig choice**

在顶层 Build/桌面区域加入 `choice`，固定符号名和 prompt；`DESKTOP_BACKEND_RELIEFOS` 为默认值，help 文案说明 `desktopd` 是现有 `windowd`/`desktop.elf`/`sessiond` 的用户可见名称，Xorg 只提供最小 X11 会话。`configs/default.conf` 显式写入原生后端为 `y`。

- [ ] **步骤 4：运行测试确认通过**

运行：`sh tests/build/test-desktop-backend.sh`

预期：Kconfig、默认值和 choice 互斥 fixture 全部 PASS。

- [ ] **步骤 5：Commit**

```bash
git add Kconfig configs/default.conf tests/build/test-desktop-backend.sh
git commit -m "feat: add desktop backend choice"
```

**接口：** 后续任务只读取 `CONFIG_DESKTOP_BACKEND_RELIEFOS`/`CONFIG_DESKTOP_BACKEND_XORG`；不得另造同义变量。

### 任务 2：扩展依赖锁 schema 并锁定 Alpine Xorg 闭包

**文件：**
- 修改：`tools/host/manifest/reliefos-deps.c`
- 修改：`tests/build/test-deps.sh`
- 修改：`configs/dependencies.lock.json`

- [ ] **步骤 1：编写失败的 feature 查询和校验测试**

在 `tests/build/test-deps.sh` 加入 fixture：

```sh
expect_output_is 'xorg entry reports feature' xorg \
    "$deps" --lock "$lock" --id alpine-xdm --print feature
expect_output_is 'unmarked entry defaults to base' base \
    "$deps" --lock "$lock" --id alpine-openrc --print feature
expect_failure 'invalid feature is rejected' \
    "$deps" --lock "$(entry_lock '{"id":"a","kind":"apk","version":"1","url":"https://x/a.apk","sha256":"'$D'","directory":"a-1","license_in_source":".PKGINFO","feature":"desktop"}')" --check
```

- [ ] **步骤 2：运行测试确认失败**

运行：`make -s O=out/xorg-plan-test test-build`

预期：FAIL，`feature` 查询未知或现有锁文件没有 `alpine-xdm`。

- [ ] **步骤 3：实现 `feature` 字段**

在 C 锁解析器中允许可选 `feature` 字符串，合法值仅为 `base`/`xorg`；缺省查询返回 `base`，未知字段和值继续拒绝。更新 `--help` 和 JSON schema 校验分支，不改变现有字段输出。

- [ ] **步骤 4：从同一 Alpine v3.24 main/community APKINDEX 生成闭包**

加入直接包及递归解析出的所有 `D:` 和 `so:` provider，至少固定以下版本：`xorg-server=21.1.24-r0`、`xorg-server-common=21.1.24-r0`、`twm=1.0.13.1-r0`、`xdm=1.1.17-r1`、`xterm=410-r0`、`xf86-video-fbdev=0.5.0-r6`、`xf86-input-evdev=2.11.0-r0`、`xkeyboard-config=2.47-r0`、`font-cursor-misc=1.0.4-r1`、`font-misc-misc=1.1.3-r1`。每个新增 entry 写入实际 URL、SHA-256、directory、license/来源说明和 `feature=xorg`；任何 Xorg ELF 的 SONAME 不得没有唯一 provider。

- [ ] **步骤 5：运行 lock/closure 测试确认通过**

运行：`make -s O=out/xorg-plan-test test-build`

预期：`reliefos-deps --check`、feature 查询、排序、fetch-list、APK URL/SHA 校验和 ELF provider closure 全部 PASS。

- [ ] **步骤 6：Commit**

```bash
git add tools/host/manifest/reliefos-deps.c tests/build/test-deps.sh configs/dependencies.lock.json
git commit -m "build: lock xorg twm xdm apk closure"
```

**接口：** `--print feature` 对缺省字段返回 `base`；后续 APK staging 通过同一个 deps 工具读取 feature，不解析 JSON 私有格式。

### 任务 3：实现 rootfs backend policy、XDM 文件和启动分支

**文件：**
- 修改：`mk/rootfs.mk`
- 修改：`tools/build/rootfs-stage.sh`
- 修改：`system/rootfs/usr/lib/reliefos/console-session`
- 创建：`system/xorg/xorg.conf`
- 创建：`system/xorg/reliefos-xdm`
- 创建：`system/xorg/xdm.conf`
- 创建：`system/xorg/xdm-Xservers`
- 创建：`system/xorg/xdm-session`
- 创建：`system/xorg/twmrc`
- 创建：`system/xorg/pam-xdm`
- 修改：`tests/build/test-desktop-backend.sh`
- 修改：`tools/test_console_boot_policy.py`

- [ ] **步骤 1：编写失败的 rootfs/启动 contract test**

用两个最小 `.config` fixture 调用真实 `rootfs-stage.sh` 计划/发布路径，断言：

```sh
grep -qx reliefos "$native/etc/reliefos/desktop-backend"
grep -qx xorg "$xorg/etc/reliefos/desktop-backend"
test -L "$native/etc/runlevels/default/reliefos-windowd"
test -L "$native/etc/runlevels/default/reliefos-session"
test ! -e "$xorg/etc/runlevels/default/reliefos-windowd"
test ! -e "$xorg/etc/runlevels/default/reliefos-session"
test -f "$xorg/etc/X11/xorg.conf"
test -f "$xorg/etc/reliefos/xdm.conf"
test -f "$xorg/etc/reliefos/xdm-Xservers"
test -f "$xorg/etc/reliefos/xdm-session"
test -f "$xorg/etc/pam.d/xdm"
```

启动脚本测试必须断言 xorg 分支执行 `xdm -nodaemon -config`、读取 marker 而不是 `source`、写入 graphical marker、错误值进入 getty，且不使用 `-novtswitch`。

- [ ] **步骤 2：运行测试确认失败**

运行：`sh tests/build/test-desktop-backend.sh && python3 -m unittest tools.test_console_boot_policy`

预期：FAIL，缺少 marker、XDM 文件和后端分支。

- [ ] **步骤 3：实现 backend staging policy**

从 `.config` 精确选择一个 backend，生成单行 marker；Xorg 模式删除两个 `default/reliefos-*` 链接并记录 Xorg 文件，native 模式保留现有树。未知配置让 staging 失败。将 `system/xorg/*` 和 `pam-xdm` 只在 Xorg 计划中写入。

- [ ] **步骤 4：实现 POSIX XDM/Xorg/TWM 文件**

固定接口和行为：

- `xorg.conf` 声明 `fbdev` `/dev/fb0`、evdev event0/event1 和 vt1；不依赖 DRM/KMS/udev。
- `reliefos-xdm` 由 root tty1 进程检查必需文件、准备 `/var/lib/xdm`，记录 root 可写的 `/var/log/xorg-session.log`，以前台方式 `exec /usr/bin/xdm -nodaemon -config /etc/reliefos/xdm.conf`；返回状态后保留 marker 并让调用方进入 getty。
- `xdm.conf` 将 Xservers 指向 `/etc/reliefos/xdm-Xservers`、session 指向 `/usr/lib/reliefos/xdm-session`，setup/reset 使用无副作用脚本或 `/bin/true`，避免默认 `xconsole`/`xsm`。
- `xdm-Xservers` 唯一 server 行为是 `:0 local /usr/bin/Xorg :0 -config /etc/X11/xorg.conf vt1 -keeptty`。
- `xdm-session` 运行于已认证用户，不能写 root-only 日志；启动 `/usr/bin/xterm` 后台进程和前台 `/usr/bin/twm -f /etc/reliefos/twmrc`，TWM 退出时清理 xterm 并返回 xdm。
- `twmrc` 使用 `fixed` 字体并提供 xterm、退出和基本窗口操作菜单。
- `pam-xdm` 包含 `common-auth`、`common-account`、`common-session`，沿用现有 PAM policy。

- [ ] **步骤 5：运行 contract/regression 测试确认通过**

运行：`sh tests/build/test-desktop-backend.sh && python3 -m unittest tools.test_console_boot_policy`

预期：native/Xorg marker、服务链接、XDM 参数、POSIX shell、失败 marker 和原生启动回归全部 PASS。

- [ ] **步骤 6：Commit**

```bash
git add mk/rootfs.mk tools/build/rootfs-stage.sh system/rootfs/usr/lib/reliefos/console-session system/xorg tests/build/test-desktop-backend.sh tools/test_console_boot_policy.py
git commit -m "feat: stage xdm twm xorg desktop backend"
```

**接口：** `desktop-backend` 是非脚本单行 marker；`reliefos-xdm` 只由 tty1 `console-session` 调用；用户会话日志写 `$HOME/.xsession-errors` 或 `$XDG_CACHE_HOME`，不直接写 `/var/log`。

### 任务 4：让 APK staging 按 backend 选择可选包

**文件：**
- 修改：`mk/apk.mk`
- 修改：`tools/build/apk-stage.sh`
- 创建：`tests/build/test-apk-stage-selection.sh`

- [ ] **步骤 1：编写失败的 selection test**

用 fixture upstream package 目录和最小 lock，分别设置 raw marker `reliefos`/`xorg`，运行 staging 的 dry-run/请求列表，断言：

```sh
grep -qx alpine-openrc "$native_requests"
! grep -q alpine-xdm "$native_requests"
grep -qx alpine-xdm "$xorg_requests"
grep -q '/lib/apk/db/installed' "$xorg_root_manifest"
```

测试还要拒绝缺失、空值、多行和未知 marker，并检查缺少 xorg archive 的错误包含 `make fetch`。

- [ ] **步骤 2：运行测试确认失败**

运行：`sh tests/build/test-apk-stage-selection.sh`

预期：FAIL，现有 staging 无法读取 feature 或会把 Xorg archive 装入 native root。

- [ ] **步骤 3：扩展 `apk-stage.sh` 接口**

把调用参数扩展为包含 `RELIEFOS_DEPS` 和 `RELIEFOS_LOCK`；从 raw `/etc/reliefos/desktop-backend` 精确读取 backend，查询 lock entry 的 feature，始终加入 `base`，Xorg 时再加入 `xorg`。过滤 upstream archive 删除、`upstream-requests`、local repository/index 和 `apk add` 参数时使用同一组，保持原始签名、依赖、所有权和 installed database。

- [ ] **步骤 4：更新 Make 依赖图**

在 `mk/apk.mk` 和 `mk/images.mk` 传递 deps/lock，令 backend marker 和 lock feature 变化触发 staging 重建；`UPSTREAM_APK_ROOT` 仍可缓存全部已验证包，但 native managed root 不得包含 xorg 包。

- [ ] **步骤 5：运行 selection/ownership 测试确认通过**

运行：`sh tests/build/test-apk-stage-selection.sh && make -s O=out/xorg-plan-test test-apk`

预期：native/Xorg 包集合正确、签名和 installed database 完整、未知 marker/缺包错误可诊断。

- [ ] **步骤 6：Commit**

```bash
git add mk/apk.mk mk/images.mk tools/build/apk-stage.sh tests/build/test-apk-stage-selection.sh
git commit -m "build: select xorg apk packages by desktop backend"
```

**接口：** `apk-stage.sh` 的 backend 选择只依赖 raw marker 和 `reliefos-deps --print feature`；不得从环境变量或 shell 代码推断后端。

### 任务 5：补齐 ReliefNT 标准 Linux VT/键盘 ABI

**文件：**
- 修改（内核子仓并提交子仓 commit）：`kernel/reliefnt/drivers/bootstrap/storage/storage_vfs.c`
- 修改：`kernel/reliefnt/kernel/reliefnt/pty.c`
- 修改：`kernel/reliefnt/kernel/reliefnt/syscall.c`
- 修改：`kernel/reliefnt/kernel/reliefnt/include/reliefnt/pty.h`
- 修改：`kernel/reliefnt/include/uapi/linux/vt.h`
- 修改：`kernel/reliefnt/include/uapi/linux/kd.h`
- 创建/修改：`kernel/reliefnt/tools/tests/vt_xorg_abi_test.c`（按子仓测试布局）
- 修改：`kernel/reliefnt` gitlink
- 创建：`tools/tests/xorg_vt_abi_test.c`
- 创建：`tools/test_xorg_abi.py`
- 修改：`docs/ABI.md`

- [ ] **步骤 1：编写失败的 ABI probe/test**

probe 必须只包含公共 `<fcntl.h>`、`<linux/vt.h>`、`<linux/kd.h>`、`<sys/ioctl.h>`、`<unistd.h>`，打开 `/dev/tty0`，并验证：

```c
int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
assert(ioctl(fd, VT_GETSTATE, &state) == 0);
assert(ioctl(fd, VT_OPENQRY, &free_vt) == 0 && free_vt >= 1 && free_vt <= 6);
assert(ioctl(fd, VT_GETMODE, &mode) == 0 && mode.mode == VT_AUTO);
assert(ioctl(fd, VT_SETMODE, &process_mode) == 0);
assert(ioctl(fd, VT_RELDISP, 1) == 0);
assert(ioctl(fd, KDGKBMODE, &kbmode) == 0);
assert(ioctl(fd, KDSKBMODE, kbmode) == 0);
```

静态 source check 必须拒绝 ReliefOS 私有 ioctl、数字 ioctl 和手写 UAPI 结构；子仓 kernel test 覆盖 `/dev/tty0` 映射、活动 VT、权限拒绝、graphics/text 恢复和 VT_PROCESS → VT_AUTO 生命周期。

- [ ] **步骤 2：运行测试确认失败**

运行：`python3 tools/test_xorg_abi.py --source-only` 和子仓对应 ABI test。

预期：FAIL，当前 devfs 没有 `tty0`，VT/键盘 ioctl 未实现。

- [ ] **步骤 3：实现标准 devfs/PTY 映射**

在 `storage_vfs.c` 暴露 `/dev/tty0`，在 `pty_lookup_vt_path`/`task_pty_endpoint_path` 将它解析为当前活动 VT 的同一 slave endpoint；不新增私有设备名。所有公共/私有函数同步 Doxygen 注释。

- [ ] **步骤 4：实现 VT/键盘 ioctl 语义**

在 `pty.c` 保存每个 VT 的 `vt_mode` 和键盘模式，`syscall.c` 对标准命令执行用户指针检查、控制终端/`CAP_SYS_TTY_CONFIG` 权限、活动 VT 检查和标准 errno。实现 `VT_OPENQRY`、`VT_GETMODE`、`VT_SETMODE`、`VT_RELDISP`、`KDGKBMODE`、`KDSKBMODE`，并保持已有 `VT_GETSTATE`、`VT_ACTIVATE`、`VT_WAITACTIVE`、`KDGETMODE`、`KDSETMODE` 的兼容行为；进程退出/失去控制终端恢复 `VT_AUTO` 和 `KD_TEXT`。

- [ ] **步骤 5：运行 ABI/内核回归测试确认通过**

运行：子仓内核测试、`python3 tools/test_xorg_abi.py --source-only`、`make -s O=out/xorg-plan-test kernel`。

预期：probe source-check PASS，内核编译和 VT/keyboard 生命周期测试 PASS，现有 `tools/test_console_boot_policy.py` 不回归。

- [ ] **步骤 6：Commit 子仓与主仓 gitlink**

```bash
git -C kernel/reliefnt add drivers/bootstrap/storage/storage_vfs.c kernel/reliefnt/pty.c kernel/reliefnt/syscall.c kernel/reliefnt/include/reliefnt/pty.h include/uapi/linux/vt.h include/uapi/linux/kd.h tools/tests/vt_xorg_abi_test.c
git -C kernel/reliefnt commit -m "feat: complete Linux VT ABI for Xorg"
git add kernel/reliefnt tools/tests/xorg_vt_abi_test.c tools/test_xorg_abi.py docs/ABI.md
git commit -m "feat: expose standard VT ABI for xorg"
```

**接口：** `/dev/tty0` 只表示当前活动 VT；Xorg 通过标准 UAPI 完成 VT ownership，不得让用户态依赖 `RELIEFOS_*` 常量。

### 任务 6：恢复 installer runtime 的原生 policy

**文件：**
- 修改：`tools/build/installer-stage.sh`
- 修改：`tests/build/test-installer-stage.sh`

- [ ] **步骤 1：编写失败的 installer fixture**

从 Xorg raw root fixture 开始运行 installer staging，断言安装后目标 root 保留：

```sh
grep -qx xorg installed-root/etc/reliefos/desktop-backend
test -e installed-root/etc/X11/xorg.conf
```

同时断言 installer runtime 恢复：

```sh
grep -qx reliefos runtime-root/etc/reliefos/desktop-backend
cmp system/rootfs/etc/inittab runtime-root/etc/inittab
test -L runtime-root/etc/runlevels/default/reliefos-windowd
test -L runtime-root/etc/runlevels/default/reliefos-session
test ! -e runtime-root/etc/reliefos/xdm.conf
```

- [ ] **步骤 2：运行测试确认失败**

运行：`sh tests/build/test-installer-stage.sh`

预期：FAIL，runtime root 继承 `xorg` marker 或 Xorg runlevel policy。

- [ ] **步骤 3：实现 runtime policy 覆盖**

在 `runtime-raw` 进入 `apk-stage.sh` 前写回 `reliefos` marker、原始 `inittab`、`desktop-session` 和两个 native default runlevel 链接；删除 Xorg 专用配置、脚本和 `pam/xdm`。`installed-raw` 只替换 installer 应用/库，不改变用户选择的 Xorg policy。

- [ ] **步骤 4：运行 installer/rootfs 回归测试**

运行：`sh tests/build/test-installer-stage.sh && sh tests/build/test-desktop-backend.sh`

预期：目标 root 与 installer runtime 的 marker、服务、inittab 和 APK 包集合各自正确。

- [ ] **步骤 5：Commit**

```bash
git add tools/build/installer-stage.sh tests/build/test-installer-stage.sh
git commit -m "build: keep installer runtime on native desktop"
```

### 任务 7：文档、离线验证和 QEMU 图形验收

**文件：**
- 创建/修改：`docs/XORG.md`
- 修改：`docs/APK_PREPARATION.md`
- 创建：`tools/test_xorg_qemu.py`
- 修改：`mk/tests.mk` 或现有测试聚合入口
- 修改：`tests/build/test-deps.sh`、`tests/build/test-apk-stage-selection.sh`（若需要闭包断言）

- [ ] **步骤 1：编写失败的 QEMU/文档验收入口**

`tools/test_xorg_qemu.py` 先验证镜像/日志存在，再解析以下事件顺序：

```text
desktop-backend=xorg
xdm started on vt1
PAM authentication accepted
twm started for uid=<non-root>
xterm started
xdm session ended
tty1 restored to text login
```

失败密码必须没有 `PAM authentication accepted`，且日志不得包含密码内容。native fixture 必须没有 Xorg 包或 xdm start 事件。

- [ ] **步骤 2：运行测试确认失败**

运行：`python3 tools/test_xorg_qemu.py --source-only`

预期：FAIL，脚本/文档/镜像验收入口尚不存在。

- [ ] **步骤 3：补充文档和测试聚合**

`docs/XORG.md` 记录 Kconfig 选择、官方 v3.24 APK/`make fetch`、XDM/TWM 会话、PAM、标准 ABI、安装器例外、QEMU 证据和未覆盖硬件范围。`docs/APK_PREPARATION.md` 更新 v3.24 community xdm/sessreg/xterm 与 feature 过滤说明。将 lock、rootfs、ABI、QEMU contract 纳入合适的 `make test`/`test-smoke` 入口。

- [ ] **步骤 4：运行静态/构建验证**

运行：

```bash
git diff --check
make -s O=out/xorg-test test-build
make -s O=out/xorg-test test
make -s O=out/xorg-test CONFIG_DESKTOP_BACKEND_XORG=y olddefconfig
make -s O=out/xorg-test CONFIG_DESKTOP_BACKEND_XORG=y image-vmdk
```

预期：Kconfig、lock、APK selection、rootfs、ABI 和构建测试 PASS；Xorg VMDK 生成完成但尚未算作运行通过。

- [ ] **步骤 5：运行 QEMU 图形验证**

运行：`python3 tools/test_xorg_qemu.py --image out/xorg-test/images/reliefos.vmdk --qmp <workspace-supported-socket>`，并按 AGENT.md 使用支持 Unix socket 的工作区路径。验证 QEMU 中 xdm 登录页、成功登录进入 twm、TWM 退出恢复 tty1、tty2–tty6 文本登录、Xorg 使用 fbdev/evdev、windowd/sessiond 未启动；记录实际 QEMU 平台和日志路径。

- [ ] **步骤 6：Commit 文档和验收工具**

```bash
git add docs/XORG.md docs/APK_PREPARATION.md tools/test_xorg_qemu.py mk/tests.mk tests/build/test-deps.sh tests/build/test-apk-stage-selection.sh
git commit -m "test: verify xdm twm xorg desktop backend"
```

### 任务 8：整分支验证和交付审查

**文件：**
- 修改：仅在验证发现问题时修改对应实现/测试文件。

- [ ] **步骤 1：运行完整定向验证**

运行：`git diff --check`、`python3 -m unittest tools.test_console_boot_policy`、所有新增 `tests/build/test-*.sh`、`make -s O=out/xorg-test test-build` 和 `make -s O=out/xorg-test test`。

- [ ] **步骤 2：运行 native/Xorg 配置矩阵**

分别使用 `make O=out/native defconfig` 与 `make O=out/xorg CONFIG_DESKTOP_BACKEND_XORG=y olddefconfig`，检查 `.config`、raw/managed root、APK manifest、runlevel、marker 和 installer runtime 的差异。

- [ ] **步骤 3：运行最终 QEMU 证据检查**

确认 QEMU 日志包含 XDM login → PAM → TWM → exit → tty1 text 的顺序，记录任何尚未在 VMware/真实硬件验证的限制；不得把静态检查或镜像存在误报为图形运行成功。

- [ ] **步骤 4：Commit/交付前状态检查**

运行：`git status --short --branch`、`git log --oneline --decorate -12`，确认只包含本任务文件、子仓 gitlink 指向已提交的 ABI 修复，且没有用户既有修改被覆盖。

