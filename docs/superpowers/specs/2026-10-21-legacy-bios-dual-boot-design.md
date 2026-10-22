# ReliefOS Legacy BIOS / UEFI 双启动设计

日期：2026-10-21。用户于 2026-10-22 确认此设计和实现计划，选择 Native，当前正在实现。

## 目标与边界

同一份默认 Installer ISO、Live ISO 和 VMDK 均可分别通过 x86 Legacy BIOS 与
x86_64 UEFI 启动。同一次默认全新安装的系统在两种固件下都能独立启动。
安装器的 FLTK 图形界面与 TTY 界面提供相同的两个 bootloader 选项，默认均启用，
允许只安装一种或两种都不安装。两种都不安装时，在现有安装确认步骤明确提示
需要用户自行配置引导，不偷偷补装其中一种。输出文件名及 `O/images/` 路径保持。

Legacy BIOS 支持的是带长模式的 x86_64 CPU，不扩展为 32 位内核。不增加 Secure
Boot、IA32 UEFI、MBR 分区表安装、网络启动或其他架构支持。这些能力不能由
BIOS/UEFI 双启动测试推断。

## 路线选择

推荐沿用 GRUB2、Multiboot2 与 GPT：现有内核和配置可共用，新增 BIOS 入口、
GPT BIOS Boot 分区和 GRUB i386-pc 载荷即可形成两条标准启动链。

另一个可行方案是 MBR 分区表加 FAT ESP，但会引入磁盘容量、主分区数量和
既有 GPT 更新兼容问题。拆成 BIOS/UEFI 两套独立发行镜像则无法满足同一默认
镜像在两种固件下启动的要求。均不采用。

## 标准和依赖

- GRUB 负责启动扇区、core image、BIOS 磁盘访问和固件交接；不手工发明 GRUB
  扇区补丁、私有 boot protocol 或固定 blocklist 文件安装器。
- 内核遵循 Multiboot2 的 i386 protected-mode 与 EFI amd64 交接约定。
- 磁盘使用标准 GPT、保护 MBR、BIOS Boot 类型
  `21686148-6449-6E6F-744E-656564454649`、ESP 和 ext4。
- 用户态使用 POSIX 文件/进程接口、Linux block ioctl、标准 `/proc` 和 `/sys`
  信息，实际执行上游 GRUB 工具；缺失的 Linux ABI 必须按公开语义实现，不能
  对工具伪造成功或增加专用内核安装 syscall。
- 保留已验证的现有 GRUB 2.12 EFI 模块，避免重新引入已知 EFI relocator 故障。
  BIOS 模块与 BIOS 安装工具从锁定的同源 GRUB 2.12 输入生成；宿主工具和
  musl 来宾工具各有独立第三方构建目录，采用 GRUB 原生构建系统。
  来源、版本、摘要和许可证进入依赖清单，只有 `make fetch` 可以联网。
  若上游构建需要额外宿主依赖，`doctor` 和当前构建文档同步说明，不能隐式下载。
- 自研 C/C++/ASM 继续由目录本地 Kbuild 声明和编译，不修改上游 Kbuild 引擎。

## 内核双入口

在 `arch/x86_64/boot/` 中增加独立 BIOS 汇编入口，ELF 默认入口指向这段 32 位
protected-mode 代码。Multiboot2 EFI amd64 entry tag 继续指向现有 64 位入口。
EFI 专属 header tag 和信息请求设置正确的可选属性，不能要求 BIOS 提供 EFI
服务；不把两种入口的寄存器和页表假设混在一起。

BIOS 入口在使用栈和段寄存器前建立自己的必要状态，保留 magic 和信息地址，
清方向标志、禁用中断，检查长模式支持，安装 GDT、引导栈和静态临时页表，
启用 PAE/long mode 后远跳至 64 位代码。临时页表覆盖低 4 GiB，包含内核、
交接信息和低地址模块；所有引导页表、GDT 和栈纳入内核保留区。进入 C 之前
遵守 x86_64 调用约定和栈对齐，之后转入相同的内核初始化流程。

EFI 路径不经过 32 位 trampoline；保留当前 EFI 服务使用方式。交接信息地址
在 64 位入口与 C 参数之间使用指针宽度，不能截断 EFI 提供的地址。
BIOS 路径使用普通 Multiboot2 memory map、ACPI RSDP 与 framebuffer 标签，
不会调用空 EFI system table。标签解析校验总长、标签边界、entry size 和
计数，拒绝截断和除零输入。无可用图形 framebuffer 时保留 TTY/串口诊断。
不支持的 CPU 在早期给出可识别错误并停止，不继续执行到随机异常。

## 新镜像和新安装的磁盘布局

统一采用 1 MiB 对齐的三分区 GPT：

| 分区 | 内容 | 文件系统 | 用途 |
| --- | --- | --- | --- |
| 1 | 1 MiB BIOS Boot | 不格式化、不挂载 | GRUB BIOS core image |
| 2 | 至少 128 MiB ESP | FAT32 | 共用内核、GRUB 配置/资源与可选 EFI loader |
| 3 | 系统根分区 | ext4 | 操作系统、应用和用户数据 |

VMDK 对应 raw 与 Installer 自动分区共用这一契约；根分区不能吞掉 BIOS Boot
空间、备份 GPT 或对齐余量。使用实际扇区大小和溢出检查，拒绝尚未支持的布局。
即使用户不安装某个 loader，也保留统一分区布局，便于以后安装另一种引导。

ESP/root 的既有镜像分区 UUID 保留其角色，不因编号变化交换。运行时使用
PARTUUID 和已发现的分区，不依赖 `/dev/sda1` 是 ESP、`sda2` 是 root。
GRUB 菜单沿用引导器定位到的 boot 分区，取消每个菜单项中强制 `(hd0,gpt1)`
的逻辑；不同磁盘顺序和 NVMe 分区命名必须覆盖。

磁盘镜像在私有临时目录中生成，通过上游 GRUB BIOS 安装工具把 boot code
写入保护 MBR 的引导区域，把 core image 嵌入专属 BIOS Boot 分区，再转换为
VMDK。保持 rootless 宿主构建边界：若上游工具无法在普通 raw 文件上正确工作，
构建应失败并在实现中解决该工具边界，不能退回修改宿主磁盘或手工拼 GRUB。
验证 GPT 两份表、保护 MBR、所有分区范围、文件系统内容和 GRUB 写入边界后
才发布 raw/VMDK。

## ISO

Installer 与 Live ISO 同时包含标准 BIOS El Torito no-emulation 入口和
UEFI El Torito FAT 启动入口。使用相同的内核和现有安装 root 模块，两条链都
进入对应菜单。BIOS 使用 GRUB i386-pc、biosdisk、iso9660、Multiboot2 与 VBE
相关模块；UEFI 使用现有 EFI 模块。两平台各自列出完整构建输入和缺失检查，
不能把 EFI 模块用于 BIOS，也不能只检查目录非空。

本次 ISO 验收包含虚拟光驱启动；不由此声称任意主板的 USB 写盘启动已验证。
ISO 体积、El Torito 加载字段、boot catalog 和 root 模块低地址放置都需要检查。

## 安装器和更新

全新安装设置增加 `skip_bios`、`skip_uefi` 两个零值即启用的内部选项。
图形界面和 TTY 共用验证和执行逻辑；选项、最终确认、执行进度和结果必须一致，
中文翻译及键盘操作同步补齐。Xorg 默认选择和现有账户、手动分区功能保留。

| 新安装选择 | BIOS MBR/core | EFI/BOOT/BOOTX64.EFI | 共用内核与 GRUB 配置 |
| --- | --- | --- | --- |
| 默认两者 | 安装 | 安装 | 安装 |
| 仅 BIOS | 安装 | 不安装 | 安装 |
| 仅 UEFI | 不安装 | 安装 | 安装 |
| 两者均不选 | 不安装 | 不安装 | 安装，并提示需要自行引导 |

ESP 在所有新布局中仍承担共用 boot 文件的存储角色；仅 BIOS 安装不意味着
删除 ESP。自动布局只保留一个实现，供预览/手动页自动分区和最终安装调用，
去掉当前两份自动分区逻辑。手动分区可识别/设置 BIOS Boot 类型且禁止格式化
该分区；选择 BIOS loader 时，缺少足够且合法的 BIOS Boot 分区应在写入之前
报错，不能借用普通文件系统空间。安装目标必须是所选整盘，而非某个分区节点。

安装前检查选中 loader 的全部载荷、上游工具、分区大小与可用性。复制共享内核
及配置、按选择发布 EFI loader、调用上游 `grub-install --target=i386-pc`
完成 BIOS 安装。使用参数数组启动工具，不构造可注入的 shell 命令串；检查
退出状态和实际写入结果。必要时 `fsync`/`sync` 并卸载目标；任何步骤失败均不得
显示安装成功。磁盘原地写入不能承诺断电原子性，但必须留下准确阶段和恢复说明。

旧系统更新不重分区、不改 ESP/root 编号。默认保留已安装的引导模式，界面明确
显示检测结果；用户可以显式要求增加某种 loader。旧 UEFI 双分区盘缺少 BIOS
Boot 分区时，只更新原有 UEFI 引导；添加 BIOS 的请求在预检阶段说明需要用户
先准备 BIOS Boot 分区，不能自动缩容或覆盖数据。更新时未选中的现有 loader
不删除，既有回滚内核/配置发布顺序保留。磁盘默认支持两种固件适用于全新安装，
不会被解释成对旧盘静默执行分区迁移。

现有 shell 引导安装工具、GUI/TTY 安装和更新代码收敛到同一 loader 安装策略。
既有命令名若为对外兼容接口可以保留薄包装，但不能留第二份 EFI-only 实现。

## 构建接口和文档收尾

构建依赖显式包含 BIOS 模块、宿主/来宾 GRUB 工具、配置和 loader 选项。
跟踪输入缺失时尽早失败；增量构建不能使用旧的单固件镜像。将 ESP 共用资源
和两种 loader 的 staging 按职责组织，替换被取代的 EFI-only 适配器并更新所有
调用者。删除退役脚本、变量、重复布局代码和仅适用于两分区的活动 fixture。

QEMU 运行目标增加显式固件选择，默认仍是 UEFI；BIOS 模式不要求 OVMF，UEFI
缺固件时明确失败，不静默切换模式。`doctor`、README、BUILDSYSTEM、
BOOT_AND_INTEGRITY、ADVANCED_INSTALL、安装器帮助和发行说明使用新的真实契约。
历史验证记录保留日期与原结论，并标注其适用提交，不能再作为当前操作说明。

## 验证与验收

源码和测试先证明旧行为会失败，再实现。验证按层次记录，不把构建成功等同于
启动成功，也不保证发现所有缺陷。

1. 内核：检查两个入口、header 校验和/对齐/文件位置、ELF 链接地址、引导页表
   保留、跨模式寄存器和栈约定；测试有效和畸形 Multiboot2 标签。
2. 镜像：实际读取 ISO 两个启动目录条目；检查 raw/VMDK GPT、BIOS Boot GUID、
   MBR/core 范围、FAT 内核/EFI 载荷、root 文件系统和镜像发布失败行为。
3. 安装：真实磁盘文件与工具覆盖四种选项、手动/自动布局、空间不足、缺失载荷、
   子进程失败、旧双分区 UEFI 更新和 NVMe 路径；取消的 loader 不出现写入。
4. 构建：父/子仓干净源码、Release/Debug、增量修改 BIOS/EFI 输入、缺输入、
   clean 重建；本地目录和待交付 Git 文件清单都必须覆盖全部生产源码。
5. QEMU：同一最终 Installer ISO 在 SeaBIOS 与 OVMF 下到达安装器；同一最终
   Live ISO 和 VMDK 在两种固件下到达登录环境。由 Installer 真正完成默认安装，
   弹出安装介质，把同一目标盘分别在两种固件下启动；再验证仅 BIOS、仅 UEFI
   和两者均不选的预期结果。至少覆盖 TTY 登录和 XDM → IceWM；检查内存映射、
   ACPI/CPU、framebuffer、磁盘访问、重启和标准 POSIX 程序。
6. 平台：每次只运行一台本任务的 4 GiB VM，不影响其他聊天进程；记录固件、
   镜像/内核哈希、日志、截图和退出状态。**VMware 验证未完成**，物理硬件未
   验证时必须明确标注，不能用 QEMU 代替。

既有 ABI golden 漂移、音频策略和其他基线失败单列，不单方面重生成或放宽断言。
本次尚未获新提交/推送授权；即便设计获批也不自动提交或推送实现。

## 附带 Actions 修复的独立进度

用户指定的 ReliefNT run `39622766793` 已定位为生产 `mm/cache/` 被泛化
`cache/` 忽略规则排除，两个源码文件漏提交。已在工作树恢复原文件、改为
`/cache/`、增加真实 ignore-rule 审计和 CI 早期布局检查；原提交的源码归档
复现失败，完整修复候选干净导出后 `make all` 退出 0。完整宿主测试仅保留既有
`test-abi`（168 → 296）失败，golden 未改。用户追加授权后已推送：K `dbf3b29` / W `90bb78dc`，远端 Actions run `39690836331` 完整通过。
详细纠正与证据在 `docs/build/kernel-modularisation-2026-10-21.md`。

## 参考规范

- [GNU Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html)
- [GNU GRUB BIOS installation](https://www.gnu.org/software/grub/manual/grub/html_node/BIOS-installation.html)
- [GNU GRUB bootable CD-ROM](https://www.gnu.org/software/grub/manual/grub/html_node/Making-a-GRUB-bootable-CD_002dROM.html)
- [GNU GRUB installation utilities](https://www.gnu.org/software/grub/manual/grub/html_node/Invoking-grub_002dinstall.html)
