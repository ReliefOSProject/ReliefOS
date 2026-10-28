# Network Controller（网络控制器）实施计划

用户选择图形网络管理器，指定名称 Network Controller；沿用 C++、FLTK 默认 gtk+
主题、中文、真实系统操作、异步响应和运行截图要求。用户此前已要求直接执行，
不等待计划批准。不提交、不推送、不新建分支、不委派，保留 installer 的现有修改。

## 设计

采用独立 `network-controller` 应用，普通 GUI 通过 sudo 或 root 密码建立受限
工作进程，root 账户直接进入。复用 OpenRC Controller 已验证的认证/分帧设计，
请求只允许固定的网络查询、配置及诊断操作，不接受任意命令或路径。

主窗口使用原生菜单、工具栏、网卡列表和“概览、配置、诊断、输出”标签页；
不放程序大标题。读取 Linux 网络 ioctl、现有公开网络查询接口、解析器和真实 DHCP 状态。
提供 DHCP、静态 IPv4、禁用、DNS 策略、有效路由/连接/租约/日志查看，
以及 Ping、DNS 查询、TCP 端口测试、子网计算和报告导出。
内核当前没有导出 `/proc/net`、ARP 和网卡总流量，隐藏流量字段并明确说明；
计数解析仅用于将来存在标准 `/proc/net/dev` 的环境，不显示虚假零值。

当前内核的可配置以太网接口是 eth0；持久化配置明确限定 eth0，
不伪造 Wi-Fi、IPv6、VPN 或任意 MTU 支持。网卡列表按实际 ioctl 返回呈现，
补充按真实接口索引查询，保证没有 IPv4 地址或禁用的网卡仍可查看。
IPv4 配置保存在 `/etc/reliefos/network.conf`（严格数据格式，不作为 shell 代码）。
仍由现有 `reliefos-dhcp` OpenRC 服务管理网络，DHCP 保留 supervise-daemon，
静态/禁用模式是一次性配置；启动模式由 OpenRC service value 保存以正确停止
旧模式。共享 profile 工具独立于可选 GUI 组件，resolver 仍由唯一 publisher 写入。
旧 DNS 配置在尚未创建新 profile 时继续生效。

## 文件分工与执行

- [x] `userland/apps/network-controller/model.{h,cpp}`、`storage.h`：IPv4/子网、
  profile、命令参数校验、分帧。先运行验证失败，再实现；拒绝注入、重复字段、
  无效网关、超长/破损帧，诊断目标不能成为命令选项。
- [x] `config.{h,cpp}`：固定目录下的原子保存、备份、恢复、冲突检测、符号链接
  拒绝；`tools/tests/network_controller_test.cpp` 和专用 shell 测试覆盖真实文件操作。
- [x] `session.{h,cpp}`、`worker.{h,cpp}`：认证、非阻塞管道、限额、取消、超时、
  EOF 清理、真实网卡与诊断。密码不进入参数、环境、文件或日志；GUI 不获得 root。
- [x] `system/rootfs/usr/lib/reliefos/network-profile`、`etc/init.d/reliefos-dhcp`、
  `publish-resolver`：数据校验和重启生效，保留默认 DHCP 和唯一 DNS 写入者。
  测试默认/静态/禁用、旧 DNS 兼容、恶意配置拒绝与 DHCP→静态→DHCP。
- [x] `main.cpp`、`window.{h,cpp}`、`dialogs.cpp`：布局、自适应、异步队列、
  配置脏状态、操作确认、输出/导出与中文。读取刷新不覆盖未保存编辑。
- [x] Kbuild、组件与 Kconfig、FLTK 清单、桌面文件、图标、中文及管理员指南：
  默认镜像、菜单、启动命令和可选组件闭环；不使用旧 netctl/windowd 协议。
- [x] 根 Makefile 构建应用、翻译和 VMDK；执行应用测试、组件/归属/桌面检查。
  专用 QEMU 副本中验证普通用户提权、中文 UI、DHCP、静态、DNS、重启持久化、
  诊断和关闭；保存真实截图。明确报告 VMware 验证未完成。

## 验证结果

最终构建为 `out/network-controller-20261028/image-build-10.log`。应用、翻译、
完整 VMDK 构建通过；模型/配置/工作进程、网络 profile、组件、APK 归属、图标、
桌面文件、中文资源检查通过。最终 ELF SHA-256：
`72567e303e2763aeccc6db1e84c609294bc36b50c6193e3d221c4e4768084ec3`。

最终 VMDK 的私有副本在 QEMU/KVM、UEFI、e1000 中运行：60 项原生检查通过；
普通用户 root 密码认证、root 直接进入、真实 GUI 保存应用、子网计算、默认及
最小窗口布局、正常关闭通过。正常执行来宾 poweroff 后再启动，13 项持久化检查
通过，确认静态地址、DNS、profile 及没有 DHCP 客户端。原 VMDK 哈希未变。
sudo 错误密码重试及正确密码、非法网关和 GUI Ping 在前一候选构建运行验证，
该候选与最终 ELF 仅两处网卡状态翻译文案不同。截图和逐轮记录保存在
`out/network-controller-qemu-final-accepted-20261029/` 与
`out/network-controller-20261028/verification.md`。

项目全局 `make test` 因既有的 audio-hda 产物缺失停止；品牌检查还有 24 处
已在未修改 HEAD 中复现的旧路径。未修改 audio manifest 或扩大修复范围，
不能描述为全部项目测试通过。QMP 的 ACPI 按钮未响应，正常关机验收使用来宾
内的 poweroff；未宣称修复 ACPI 按钮。VMware 验证未完成。

## 重点失败场景

1. 静态地址/掩码/网关不匹配：保存前拒绝，已有配置不变。
2. 编辑期间外部文件变动或符号链接：保存/恢复拒绝，不覆盖或跟随。
3. DHCP 与静态切换：先停止旧 DHCP，服务状态与真实运行模式一致，无第二客户端。
4. DNS/连接超时或取消、窗口关闭：事件循环仍响应，子进程被回收。
5. 配置已保存但应用失败：分别报告结果，保留备份和日志，不谎报联网成功。
