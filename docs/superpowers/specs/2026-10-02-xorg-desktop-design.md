# ReliefOS Xorg + TWM + XDM 桌面后端设计规格

> **历史记录（2026-10-02）**：本文是 Xorg 桌面后端首次落地时的设计/实现记录，
> 文中描述的 TWM 会话与 Kconfig 后端选择均已不再是当前系统。当前默认窗口管理器为
> **IceWM**，会话与配置见 [`docs/XORG.md`](../../XORG.md)（`system/xorg/icewm/`、
> `ICEWM_PRIVCFG=/etc/reliefos/icewm`）。本文仅保留作历史存档。


## 目标

为 ReliefOS 增加一个可由 Kconfig 选择的桌面后端：

1. 默认的 `ReliefOS Desktop + desktopd`。用户可见名称保留为该名称，实际实现继续使用现有的 `windowd`、`desktop.elf` 和 `sessiond`，不新增名为 `desktopd` 的守护进程。
2. Alpine 官方 x86_64/musl 二进制提供的 `Xorg + twm + xdm`。系统启动后，tty1 由 xdm 显示图形登录页；认证成功后，xdm 的用户会话脚本启动 twm。

默认配置必须保持当前 ReliefOS 图形启动行为不变。Xorg 后端只承诺一个可登录、可退出的最小 X11 会话；现有 ReliefOS 原生 GUI 应用不在本次迁移范围内。

## 已确认的上下文

- 当前启动链是 BusyBox init → OpenRC → 六个 `console-session` respawn；tty1 的 `console-session` 启动 `login.elf --graphical-session`，其他 tty 使用文本 getty。
- 原生图形服务由 `reliefos-windowd`、`reliefos-session` 和 `reliefos-imd` 等 OpenRC 链接组成。仓库中没有独立的 `desktopd` 程序。
- 生产构建只从 `configs/dependencies.lock.json` 消费经过 `make fetch` 校验的缓存。普通构建不得隐式联网。
- 内核提供 `/dev/fb0`、`/dev/input/event0`、`/dev/input/event1`、`/dev/tty1` 至 `/dev/tty6`，以及当前已覆盖的 Linux fbdev、evdev、VT 和控制终端接口；没有可供本任务依赖的 DRM/KMS 或 udev 枚举路径。Xorg 还会按 Linux ABI 打开 `/dev/tty0` 并使用 `VT_OPENQRY`、`VT_GETMODE`、`VT_SETMODE`、`VT_RELDISP`、`KDGKBMODE` 和 `KDSKBMODE`，这些接口必须在内核子仓中补齐，不能靠包装脚本绕过。
- 现有 PAM 配置没有 `xdm` 服务项，`other` 规则会拒绝未声明的服务；XDM 后端必须显式提供 `/etc/pam.d/xdm`。

## 方案与边界

### 选定方案

Xorg 后端不新增 OpenRC display-manager 服务，而是在 tty1 的既有 `console-session` 进程中以前台方式启动 xdm。包装脚本使用 POSIX `/bin/sh`，执行 `xdm -nodaemon -config /etc/reliefos/xdm.conf`；因此 tty1 在 xdm 运行期间不会再启动 getty，xdm 退出后同一个 respawn 才恢复文本登录。这避免了 OpenRC xdm 服务与 tty1 getty 同时争抢控制终端。

XDM 的配置文件放在 `/etc/reliefos/`，不覆盖 Alpine 包本身的文件：

- `xdm.conf` 将 Xservers、session 和错误日志路径指向 ReliefOS 的固定文件。
- `xdm-Xservers` 只启动 `Xorg :0 -config /etc/X11/xorg.conf vt1 -keeptty`，由标准 VT_ACTIVATE/VT_WAITACTIVE 流程切换到 tty1。
- `xdm-session` 以已认证用户身份执行 `twm -f /etc/reliefos/twmrc`，并可启动一个 `xterm` 作为初始客户端；会话结束时返回 xdm。
- setup/reset 使用无副作用的本地脚本或 `/bin/true`，不依赖 Alpine 默认 `xconsole`、`/dev/console` 或不存在的 `xsm`。

这使 Xorg 只拥有 tty1、fbdev 和两个 evdev 设备；tty2 至 tty6 继续提供文本登录。`console-session` 在首次尝试前写入现有 graphical-session marker，Xorg/xdm 退出后保留该 marker 并进入 getty，因此失败不会触发 BusyBox respawn 的快速重启循环。

### 不包含的方案

- 不启用 `xdm-openrc`，不改变默认 OpenRC display-manager 服务拓扑。
- 不使用 `xinit` 作为登录管理器，不用自写登录界面替代 xdm。
- 不引入 DRM/KMS、Mesa modesetting、Wayland、libinput 或 Xorg 私有 syscall/ioctl；需要的是标准 Linux `/dev/tty0` 当前 VT 别名和标准 VT/键盘 ioctl 语义。
- 不把现有 ReliefOS GUI 应用改造成 X11 客户端。

## Kconfig 与配置数据流

顶层 `Kconfig` 新增互斥 `choice`：

- `DESKTOP_BACKEND_RELIEFOS`：提示 `ReliefOS Desktop + desktopd`，默认值为 `y`。
- `DESKTOP_BACKEND_XORG`：提示 `Xorg + twm + xdm`。

`configs/default.conf` 显式保存原生后端的默认选择。Kconfig front-end、生成的 `.config`、autoconf 和 Make 依赖图是唯一配置来源；不在 Makefile、C 常量或启动脚本中维护第二套默认值。

rootfs staging 从 `.config` 生成 `/etc/reliefos/desktop-backend`，内容严格为单行 `reliefos` 或 `xorg`，读取方只能精确匹配这两个值，不能 `source` 或执行该文件。未知或多行值使 staging 失败；运行时遇到缺失/未知值时记录错误并进入文本登录。

## APK 与离线 staging

Xorg 后端使用 Alpine 官方 v3.24 x86_64/musl APK。直接选择的包为：

- `xorg-server`
- `xorg-server-common`
- `twm`
- `xdm`
- `xterm`
- `xf86-video-fbdev`
- `xf86-input-evdev`
- `xkeyboard-config`
- `font-cursor-misc`
- `font-misc-misc`

直接包版本以当前 v3.24 main/community APKINDEX 的锁定结果为准（当前索引包括 `xorg-server 21.1.24-r0`、`twm 1.0.13.1-r0`、`xdm 1.1.17-r1`、`xterm 410-r0`、`xf86-video-fbdev 0.5.0-r6`、`xf86-input-evdev 2.11.0-r0`、`xkeyboard-config 2.47-r0`、`font-cursor-misc 1.0.4-r1` 和 `font-misc-misc 1.1.3-r1`）。实现时必须从同一 APKINDEX 解析完整的包名、版本、URL、SHA-256、目录、许可证和签名信息，把所有传递包逐一写入锁文件；不得只锁定上述顶层包。

`reliefos-deps` 的锁 schema 增加可选 `feature` 字段，合法值只有 `base` 和 `xorg`，缺省为 `base`。`upstream-apk.sh` 继续验证并缓存所有 `alpine-*` 包；`apk-stage.sh` 根据 raw root 中精确的后端标记只把 `base` 包或 `base+xorg` 包加入离线 APK transaction。这样原生 rootfs 不包含 Xorg 包，Xorg rootfs 保留完整依赖闭包、原始 APK 元数据、签名和数据库所有权。

缺少锁定缓存时，构建必须指出具体依赖 ID 和 `make fetch` 修复命令；生产构建不得自行下载或从网络解析依赖。

## Rootfs、安装器与 OpenRC policy

原生后端：

- `/etc/reliefos/desktop-backend` 为 `reliefos`。
- 保留 `default/reliefos-windowd` 和 `default/reliefos-session` 链接。
- `console-session tty1` 继续执行 `login.elf --graphical-session`。

Xorg 后端：

- `/etc/reliefos/desktop-backend` 为 `xorg`。
- 删除安装后 rootfs 的 `default/reliefos-windowd` 和 `default/reliefos-session` 链接，但保留服务脚本和原生程序供默认构建与安装器使用。
- raw root 额外包含 `/etc/X11/xorg.conf`、`/etc/reliefos/xdm.conf`、`/etc/reliefos/xdm-Xservers`、`/etc/reliefos/xdm-session`、`/etc/reliefos/twmrc`、xdm 包装脚本和 `/etc/pam.d/xdm`。
- tty1 仍由 `console-session` respawn，但其 xorg 分支阻塞在前台 xdm 上；xorg 分支结束后再进入文本 getty。tty2 至 tty6 不变。
- 不启动 `windowd`、`sessiond` 或第二个图形服务器。

安装器 runtime 无条件恢复原生 policy：在其 runtime raw root 中写回 `reliefos` 标记、原始 `inittab` 和 `desktop-session`，恢复两个原生 default runlevel 链接，并移除 Xorg 专用启动文件与 PAM 项。安装后的目标 root 保留用户选择的 `xorg` 标记和包闭包；安装器 runtime 与目标 root 必须分别通过 APK transaction 打包。

## Xorg、XDM 与 TWM 运行时

`/etc/X11/xorg.conf` 固定使用 `fbdev` 驱动和 `/dev/fb0`，并为键盘、鼠标固定配置 `evdev` `/dev/input/event0` 与 `/dev/input/event1`；ServerLayout 显式绑定 `vt1` 使用的显示设备。不得依赖 udev、DRM/KMS 或设备枚举顺序。

为满足 Xorg 的标准 Linux VT 初始化路径，内核子仓新增 `/dev/tty0` 当前虚拟终端别名，并实现 `VT_OPENQRY`、`VT_GETMODE`、`VT_SETMODE`、`VT_RELDISP`、`KDGKBMODE`、`KDSKBMODE` 及已有 `VT_GETSTATE`/`VT_ACTIVATE`/`VT_WAITACTIVE`/`KDSETMODE` 的错误、权限和恢复语义。`/dev/tty0` 只解析为当前活动 VT，不暴露 ReliefOS 私有设备接口；相应 UAPI、用户库、导出清单、ABI 文档和测试必须同步更新。

`reliefos-xdm-session` 只使用 POSIX shell 和标准路径：检查 `$DISPLAY`、日志目录和必需可执行文件，不假设 xdm 派生的用户进程仍持有 `/dev/tty1`；Xservers 配置负责把 Xorg 固定到 vt1。脚本以现有用户身份设置 `TWMRC`/`XDG_SESSION_TYPE`，启动 twm，并按需启动 xterm。脚本不得把密码、会话 cookie 或 XDM 授权数据写入日志。

`/etc/pam.d/xdm` 至少包含 `common-auth`、`common-account` 和 `common-session`，沿用现有 PAM 密码验证、账户限制和会话环境策略。登录失败由 xdm 继续显示登录页；登录成功后由 xdm 执行会话脚本。

## ABI、权限与错误处理

- Xorg、xdm、twm 和 xterm 只消费 Linux/POSIX ELF/musl、进程、文件、控制终端、VT、fbdev、evdev、信号、PAM 和 Unix 权限接口。
- 不新增 Xorg 专用 syscall、ioctl 或内核私有头；发现缺失标准 ABI 时，按公共头、内核、运行库、导出清单、测试和 ABI 文档的闭环补齐。
- 非活动 VT 或不具备图形控制权时，fbdev/VT 操作返回标准 errno；用户态记录错误并退出到文本登录，不能无限重试。
- xdm、Xorg 或 TWM 退出后，包装脚本记录退出状态并释放临时 fd；下一次 `console-session` respawn 可以安全启动 getty。
- XDM 登录页不提供 root 交互 shell 作为默认会话；会话身份来自 PAM 已认证用户。

## 验收标准

### 配置与 staging

- 新输出目录 `make defconfig` 只选择 `DESKTOP_BACKEND_RELIEFOS=y`。
- `make menuconfig` 可在两个后端间切换，生成配置不同时启用两个符号。
- 原生 rootfs 不含 Xorg/TWM/XDM 运行时包并保留原生 default runlevel；Xorg rootfs 含完整 APK 闭包、固定 Xorg/xdm 配置和 `xorg` 标记，并移除两个原生图形链接。
- 缺少任一锁定包时离线 staging 失败并提示 `make fetch`。
- installer runtime 在任一目标后端选择下都使用原生标记、inittab 和服务链接。

### 运行验证

- QEMU 图形启动后，tty1 显示 xdm 登录页；错误密码留在 xdm；正确登录后进入 twm，twm root menu 可启动 xterm。
- 退出 twm/xdm 后，tty1 恢复文本登录，tty2 至 tty6 仍可登录。
- Xorg 日志证明使用 `/dev/fb0`、event0、event1 和 vt1；没有启动 `windowd` 或 `sessiond`。
- 原生后端仍能启动现有 ReliefOS 图形桌面；切换后端不会出现两个图形服务器同时运行。
- 缺失设备、PAM 配置或后端标记时，日志给出明确阶段和 errno/状态，系统回到可用文本登录。

## 文档与验证产物

新增/更新文档必须说明 Kconfig 选项、Alpine 官方 APK 锁定与 `make fetch` 入口、XDM/TWM 最小会话边界、安装器例外、Linux ABI 限制，以及 QEMU 实际验证的平台和未覆盖的硬件显示范围。任何只完成源码、编译、打包或镜像生成的结果，都必须分别标记为未完成运行验证。
