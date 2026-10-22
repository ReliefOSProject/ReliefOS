# ReliefOS Legacy BIOS / UEFI Dual Boot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. 用户已于 2026-10-22 选择 Native：当前会话顺序实现，最终独立审查。

**Goal:** 同一默认 Installer ISO、Live ISO、VMDK 和全新安装结果可分别由 BIOS/UEFI 启动，安装器允许分别跳过两种 loader。

**Architecture:** 保留 GRUB2、Multiboot2、GPT 和现有 EFI 载荷，新增内核 BIOS protected-mode 入口、BIOS Boot 分区及锁定的 GRUB i386-pc 工具链。图形、TTY 和命令行安装共用引导安装实现；旧盘更新不迁移分区。先验证上游 GRUB 在私有 raw 文件和来宾上的真实行为，再接入发行镜像。

**Tech Stack:** x86_64/IA32 entry ASM、C、C++/FLTK、POSIX shell、Linux block ABI、GNU GRUB 2.12、递归 Kbuild、GPT/FAT32/ext4、El Torito、QEMU/SeaBIOS/OVMF。

**Spec:** [已确认设计](../specs/2026-10-21-legacy-bios-dual-boot-design.md)

## Global Constraints

- “同一份默认 Installer ISO、Live ISO 和 VMDK 均可分别通过 x86 Legacy BIOS 与 x86_64 UEFI 启动。”
- “允许只安装一种或两种都不安装。” 新安装 `skip_bios=0`、`skip_uefi=0`；更新默认保留已安装模式。
- “统一采用 1 MiB 对齐的三分区 GPT”：1 MiB BIOS Boot、至少 128 MiB FAT32 ESP、ext4 root。
- BIOS Boot GUID：`21686148-6449-6E6F-744E-656564454649`；不格式化、不挂载。
- “保留已验证的现有 GRUB 2.12 EFI 模块”；BIOS 工具/模块同源锁定；“只有 `make fetch` 可以联网”。
- “用户态使用 POSIX 文件/进程接口、Linux block ioctl、标准 `/proc` 和 `/sys` 信息”；不增加私有安装 syscall，不伪造工具成功。
- “自研 C/C++/ASM 继续由目录本地 Kbuild 声明和编译，不修改上游 Kbuild 引擎。” 第三方继续原生构建。
- “旧系统更新不重分区、不改 ESP/root 编号。” 输出路径、账户、Xorg 选择和回滚兼容保留。
- 不重生成 ABI golden，不修改音频策略掩盖既有失败。历史证据不能替代本次验证。
- 工作树 `W=/home/xiaobai/Projects/Projects/.worktrees/kbuild-migration/ReliefOS_Xorg`，`K=$W/kernel/reliefnt`；不修改主工作区用户改动。
- `TMPDIR=$W/out/tmp`；日志写 `out/dualboot/logs/`；同时仅一台本任务 4 GiB VM，QMP 使用短 `out/dbq/` 路径。
- 新函数/声明写 Doxygen，说明边界、借用/所有权、锁/中断上下文；引导代码不分配堆、不依赖运行态锁。
- 未授权提交、推送或建分支；各任务结束只检查 diff、更新执行记录。用户以后授权时先推子仓，再推父仓。
- 审查代理如需使用，只用 GPT-6-Luna / Max、干净上下文、禁止嵌套。
- **VMware 验证未完成**；物理硬件未验证时单列，不用 QEMU 代替。

## Review Focus

1. 多盘及顺序变化：不能把 bootloader 写到源盘、分区节点或另一块同名文件的磁盘；任务 3/4/6 验证所选整盘、PARTUUID 和逆序启动。
2. 跳过 loader：完整 ESP 复制不能绕过选择；任务 4/5 检查真实 MBR/core 与 EFI 文件副作用，含四种组合。
3. 容量/扇区边界：过小目标、大载荷、非支持逻辑扇区大小须在擦写前失败；任务 3/5 检查 GPT 两份表、溢出、对齐及未写入的源盘。
4. 固件交接差异：EFI 高地址指针不能截断，BIOS 不得调用 EFI 或依赖原页表；任务 2 的畸形标签/高地址 fixture 与任务 6 的两固件实启动覆盖。
5. 安装中断/更新失败：不能发布缺载荷的新菜单或显示成功；任务 4 覆盖复制/子进程/同步失败，任务 6 实测旧 UEFI 布局更新与失败恢复。

## 文件职责

| 边界 | 创建或修改 | 职责 |
| --- | --- | --- |
| GRUB 输入 | `configs/dependencies.lock.json`、`tools/build/grub.sh`（新）、`scripts/reliefos/Makefile.grub`（新）、`Kbuild` | 锁定输入，隔离宿主/来宾工具与 BIOS 模块产物 |
| 内核入口 | `kernel/reliefnt/arch/x86_64/boot/{bios.S,boot.S,multiboot2.c,Kbuild}`、`arch/x86_64/linker.ld`、`include/reliefnt/{kernel.h,multiboot2.h}`、`kernel/init/kernel.c` | BIOS 转长模式、EFI 宽指针、标签验证 |
| 分区 | `userland/runtime/{include/reliefos/blockdev.h,src/blockdev.c}`、`userland/apps/installer/installer_partitions.{c,h}`（新） | BIOS Boot 类型与唯一自动布局实现 |
| 引导安装 | `userland/runtime/include/reliefos/bootloader.h`（新）、`src/bootloader{,_publish,_process}.c`（新）、`src/bootloader_internal.h`（新）、`src/Kbuild` | 标准 ABI 预检、发布、GRUB 进程和安装模式记录 |
| 命令入口 | `userland/apps/bootinstall/{main.c,Kbuild}`（新）、`configs/components.toml`、`userland/storage/leonos-grub-installer` | 共用安装 API 的 console CLI、旧命令薄兼容入口；不加图标 |
| 安装 UI/更新 | `userland/apps/installer/{main.cpp,install_ops.c,install_ops.h,installer_setup.h,installer_tty.c,Kbuild}`、`configs/nls/po/{leonos.pot,zh_CN.po}` | 选项、预检、确认、执行和错误展示 |
| 镜像 | `tools/build/{boot-stage.sh,disk.sh,iso.sh,installer-stage.sh,rootfs-stage.sh}`、`scripts/reliefos/Makefile.{images,rootfs}`、`configs/apk-ownership.json`、`boot/grub/*.cfg` | 替换 EFI-only staging；双固件载荷与包归属 |
| 运行/验收 | `scripts/reliefos/Makefile.run`、`scripts/{test-smoke.sh,doctor.sh,help.sh}`、`tests/build/`、`tools/test_dual_boot_qemu.py`（新） | 明确固件选择、真实介质/安装矩阵与证据 |

下文父仓相对路径均以 W 为根；内核路径标为 K。宿主测试可以用 Python，生产自研编译仍使用 Kbuild。新增文件在各自目录显式登记，不列入根部集中源码清单。

---

### Task 0: 保留并核实 Actions 修复（已完成）

**Files:** K 的 `.gitignore`、`.github/workflows/build.yml`、`tools/test_module_layout.py`、`mm/cache/{Kbuild,page_cache.c}`；父仓 `docs/build/kernel-modularisation-2026-10-21.md`。

**Interfaces:** CI 在 fetch 前执行 `python3 tools/test_module_layout.py`；根下载缓存 `/cache/` 继续忽略，生产 `mm/cache/` 可交付。

- [x] 原提交 Git archive：布局检查报 `missing Kbuild: mm/cache`，`make all` 退出 2。
- [x] 新 ignore-rule 审计先在旧规则下报两个 ignored input；修正规则后通过。
- [x] 临时 Git index 导出候选，无本地未交付源码，`make -j4 all` 退出 0，三个内核产品与 `page_cache.o` 存在。
- [x] `make -k -j4 test` 只有既有 `test-abi` 168 → 296 失败；独立只读审查无待修项。用户追加授权后已推送：K `dbf3b29` / W `90bb78dc`，远端 run `39690836331` 完整通过。

### Task 1: 锁定 GRUB BIOS 工具链与共用 boot staging

**Files:** 文件职责表的 GRUB 输入和镜像 staging；`scripts/reliefos/Makefile.upstream`、`scripts/doctor.sh`、`tests/build/test-grub-kbuild-inputs.sh`；新 `tests/build/test-grub-bios-tools.sh`。

**Interfaces:** `make grub-host` 产生 `O/grub-host/bin/{grub-mkimage,grub-mkstandalone,grub-bios-setup,grub-probe}` 与 `O/grub-host/lib/grub/i386-pc/`；`make upstream-grub` 产生 `O/upstream/grub/root/usr/{sbin,bin,lib/grub/i386-pc}` 的来宾工具/模块。`GRUB_BIOS_DIR` 显式指向受验证目录；EFI 继续用 `GRUB_EFI_DIR`。

- [x] **先写并运行失败测试。** `TMPDIR=$W/out/tmp sh tests/build/test-grub-bios-tools.sh`：要求同源 2.12 工具、完整 BIOS 模块；缺缓存不得联网，缺 `biosdisk.mod` 不得生成 Kbuild `.mod` 替代物。无 root/loop 设备，在私有 GPT raw 的 BIOS Boot 分区调用真实上游 setup；断言 GPT、ESP、root 字节未被越界修改。当前应因缺少新目标/BIOS 产物失败。
- [x] **实现第三方适配。** 从 GNU GRUB 2.12 发布输入核验来源/签名并记录真实摘要，不填占位 hash。宿主/来宾分开 configure/build/stage，避免版本、路径和工具参数串用；可选依赖按实际启用范围关闭。来宾工具用 musl，不能拷宿主 glibc ELF。Make 声明完整产物与参数身份，`scripts/clean.sh` 覆盖新产物目录且保留缓存和未知文件。
- [x] **实现共用 staging。** `boot-stage.sh SRC EFI_MODULES BIOS_MODULES KERNEL FONT DISPLAY STAGE EPOCH` 生成公共 kernel/config/font/theme、EFI loader 与 BIOS 安装载荷；保留原 kernel 回滚文件语义。更新所有调用并移除 `efi-stage.sh`。测试读取实际 ELF/模块而非只 grep 配置文本。
- [x] **验证。** `make O=out/dualboot-build grub-host upstream-grub`、上述工具测试及 `test-grub-kbuild-inputs.sh` 退出 0；再次无改动构建不重编译，删除必需模块明确失败。保存真实 raw setup 越界比较及来宾 ELF 依赖检查。此门禁未通过不能开始镜像集成。

### Task 2: 内核标准 BIOS / EFI 双入口及交接解析

**Files:** 文件职责表的内核文件；K 新 `tools/{test_boot_entry.py,tests/multiboot2_runtime_test.c,tests/multiboot2_parser.c}`，修改 `tools/tests/Kbuild`、`Makefile`。

**Interfaces:** ELF entry 为 `bios_start`；EFI header tag 9 仍指向 `_start`。`void kernel_main(uint32_t magic, uintptr_t multiboot_info)`；`static void kernel_start(uint32_t magic, uintptr_t multiboot_info)`；`int multiboot2_parse(uint32_t magic, uintptr_t info_addr, struct boot_info *out)` 返回 0 或 `-EINVAL`，失败不暴露部分解析结果。新增 `make test-boot` 纳入 K 的 `test`。

- [x] **写红灯测试。** runtime fixture 链接真实解析代码：合法 BIOS mmap/ACPI/RGB framebuffer/空列表/未知 tag 成功；坏 magic、总长溢出、短/零 tag、缺 end、未终止字符串、mmap entry size 为 0 或过小、非法 module 范围失败；在可用高地址映射放置信息，断言指针不截断。ELF 检查分别验证 BIOS entry 与 EFI tag 9 地址。先运行并记录当前失败。
- [x] **实现 `bios.S`。** 独立 `.code32` 入口保存 EAX/EBX，`cli`/`cld`，先检查 CPUID/long mode，再建立 GDT/栈/六页引导页表，以 2 MiB 页 identity-map 低 4 GiB；设置所需 CR0/CR4/EFER 状态并远跳 64 位。临时表和栈进入 linker 保留范围；不支持 CPU 输出可识别早期错误并停机。EFI 入口保留 64 位 RBX，进入 C 前栈按 ABI 对齐、清方向标志。
- [x] **调整 header 和初始化。** EFI 专属 tag/信息请求为可选；ELF 默认 BIOS entry。解析器按剩余字节验证每种已知标签并允许规范规定的未知标签；只在 EFI system table 存在且确需捕获时调用 EFI。C 调用者检查解析错误，不使用估算内存掩盖无效交接。
- [x] **验证。** 在 K 执行 `make O=$W/out/dualboot-kernel -j4 all test-boot test-module-layout` 和独立 Debug 编译；解析 fixture 跑 ASan/UBSan。用任务 1 真实 GRUB 制作最小私有测试 ISO，SeaBIOS/OVMF 顺序启动同一 kernel 到稳定初始化标记；检查 BIOS mmap、ACPI、页表保留和 EFI 原路径。该最小介质不充当最终安装验收。

### Task 3: BIOS Boot GPT 类型与唯一安装分区布局

**Files:** `userland/runtime/{include/reliefos/blockdev.h,src/blockdev.c}`；新 `userland/apps/installer/installer_partitions.{c,h}`；`install_ops.{c,h}`、`Kbuild`；新 `tools/{test_installer_partitions.py,tests/installer_partitions_test.c}`，扩展 `tools/test_installer_input.py`。

**Interfaces:** 追加 `RELIEFOS_BLOCK_GPT_BIOS_BOOT = 4`，保留已有 enum 值/struct 布局。`struct installer_partition_plan` 保存 BIOS/ESP/root 的 index、first_lba、sector_count；`int installer_partition_plan_default(const struct reliefos_block_disk_info *disk, struct installer_partition_plan *out)` 只计算/验证；`int installer_partition_apply_default(const char *disk_path, const struct installer_partition_plan *plan)` 才写 GPT。库 index 从 0 开始，对应分区编号 1/2/3。

- [x] **写红灯测试。** 在真实稀疏文件上通过现有最底层 block-I/O 测试适配运行生产 GPT 代码，以 `sfdisk --json` 独立读回：BIOS index 0、起始 LBA 2048、长度 2048；128 MiB ESP index 1、起始 4096；root index 2、起始 266240、末尾不越过 backup GPT。测试过小盘、整数溢出、只读/挂载中目标和逻辑扇区 4096 拒绝且文件哈希不变；512e 按逻辑 512 处理。当前因 BIOS 类型/计划函数缺失失败。
- [x] **实现标准 GUID。** `UNKNOWN` 文件系统可创建 raw 分区，设置/枚举/显示 BIOS Boot 类型；禁止对它调用格式化。支持范围仍为逻辑 512 字节，其他大小返回 `-ENOTSUP`，不能错误乘 2048 后继续写。GPT 主备 CRC 和保护 MBR 均交由现有标准 raw-I/O 逻辑维护。
- [x] **收敛自动布局。** `installer_partition_auto_layout()` 与最终 fresh auto 安装调用同一计划/应用函数，删除原两份 ESP+root 算法。手动计划按类型和实际 index 选择，加入 BIOS 分区检查；旧盘更新只枚举，不调用 create/apply。新头/对象登记在安装器本地 Kbuild。
- [x] **验证。** `TMPDIR=$W/out/tmp python3 tools/test_installer_partitions.py`、`python3 tools/test_installer_input.py` 退出 0；再以标准 `fdisk/blkid` 读回 GPT 类型与 PARTUUID。测试 NVMe `pN` 路径和 ESP/root 不是编号 1/2 的手动布局，保留未选分区字节。

### Task 4: 共用引导安装 API、CLI 与四种选项

**Files:** 文件职责表的 runtime 引导安装、CLI、安装 UI/更新、rootfs/installer staging/归属；`tools/test_installer_setup.py`、`tests/build/test-image-adapters.sh`、`tests/build/test-installer-stage.sh`；新 `tools/test_bootloader_install.py` 与 `tests/host/test_bootloader_install.c`。

**Interfaces:** 声明位于 `reliefos/bootloader.h`，选项字符串仅借用至调用结束，回调可为空；runtime 三个源文件分别拥有预检/流程、文件发布、子进程处理。

```c
struct reliefos_boot_install_options {
    const char *source, *boot_dir, *disk_path;
    uint8_t install_bios, install_uefi, preserve_rollback;
    void (*progress)(void *context);
    void *progress_context;
};
int reliefos_boot_validate_payload(const struct reliefos_boot_install_options *options);
int reliefos_boot_validate_target(const struct reliefos_boot_install_options *options);
int reliefos_boot_install(const struct reliefos_boot_install_options *options);
int reliefos_boot_probe(const char *disk_path, const char *boot_dir, uint32_t *modes);
```

函数返回 0/负 errno；probe 输出 BIOS=1、UEFI=2 位。新安装器总是提供所选整盘路径；只有明确禁用 BIOS 的兼容文件复制调用允许 `disk_path=NULL`，此时不执行任何 raw 写入，仍验证目标文件路径安全性。

CLI：`reliefos-boot-install --source DIR --boot-directory DIR [--disk DEVICE] --bios=yes|no --uefi=yes|no [--check] [--update]`，省略模式选项时默认安装两种，启用 BIOS 必须提供 `--disk`。旧 `leonos-grub-installer [--source DIR] ESP` 转发为 `--bios=no --uefi=yes --update --boot-directory ESP`，保留原 UEFI 复制和回滚语义，不保留第二套发布逻辑。CLI 为 console 程序，不注册图标。

- [x] **写并观察红灯。** 同一私有目标针对 `(bios,uefi)=(1,1),(1,0),(0,1),(0,0)` 验证公共 kernel/config、EFI 文件和 MBR/core 的真实结果。边界测试包含分区路径冒充整盘、源盘=目标盘、两个目标磁盘、缺 BIOS 分区/源文件/工具、symlink 目标、复制/GRUB exit 非零/同步失败；错误时不发布新菜单、不报告成功、不修改未选 loader。进程 test double 只用来触发失败，成功至少执行一次真实上游 GRUB。
- [x] **实现共用 API/CLI。** `posix_spawn` 参数数组调用真实 `grub-install --target=i386-pc`；固定工具路径及子进程环境，不全局修改父进程环境。验证整盘、所属 boot 分区、所选载荷和 BIOS 嵌入空间。原子临时文件发布与比较，先资源后配置，保留回滚；等待子进程时通过回调驱动现有 UI 事件泵。写入不能断电原子化，错误保留阶段诊断，不虚报回滚完整。
- [x] **实现模式记录/更新。** 成功发布 `/boot/reliefos/config/bootloaders.conf`（`version=1`、`bios=0|1`、`uefi=0|1`）。有记录按记录并核对文件；旧无记录盘以既有 EFI 文件识别 UEFI，不因存在空 BIOS 分区就声称 BIOS 已安装。更新保留未选 loader；显式增加 BIOS 缺分区时预检失败。公共 kernel 更新继续兼容既有 rollback 发布顺序。
- [x] **接入 GUI/TTY。** `installer_setup` 增加 `uint8_t skip_bios, skip_uefi`，零值全选；确认页显示实际选择和两者均不选提示。在新装任何擦写前做 payload/工具与布局预检，手动/更新再做真实 target 预检；替换 whole-ESP 无条件复制和更新中的无条件 EFI 目录发布。GUI/TTY/手动 CLI 共用执行策略；账户、Xorg、密码清零保留。中文/键盘操作和错误提示同步。APK 归属、console 安装 profile 和来宾所需工具/库全部明确入包。
- [x] **验证。** `TMPDIR=$W/out/tmp python3 tools/test_bootloader_install.py`、`python3 tools/test_installer_setup.py`、`sh tests/build/test-image-adapters.sh`、`sh tests/build/test-installer-stage.sh` 退出 0；完整 Xorg 与 console payload 都包含实际 musl GRUB 工具。新装 none 保留共享文件，但不写 EFI loader/BIOS boot code；更新 skip 与“删除”语义明确区分。

### Task 5: 双固件 ISO / VMDK 与运行目标

**Files:** `tools/build/{disk.sh,iso.sh}`、`scripts/reliefos/Makefile.images`、`boot/grub/*.cfg`、`scripts/reliefos/Makefile.run`、`Kbuild`、`scripts/{help.sh,test-smoke.sh}`；新 `tests/build/test-dual-boot-images.sh`、`tests/build/test-qemu-firmware.sh`。

**Interfaces:** 保留 `make image-vmdk iso installer` 和原产物路径。`disk.sh ESP ROOT_EXT4 OUTPUT_RAW OUTPUT_VMDK EPOCH BIOS_MODULES GRUB_MKIMAGE GRUB_BIOS_SETUP`；`iso.sh SRC EFI_MODULES ESP ROOT CONFIG OUTPUT EPOCH VOLUME BIOS_MODULES GRUB_MKIMAGE`。新增 `QEMU_BOOT=uefi|bios`，默认 uefi；现有 `QEMU_FIRMWARE` 仅用于 uefi。`make test-smoke` 顺序覆盖两固件、三介质。

- [x] **写红灯测试。** 读取真实生成的 GPT、保护 MBR 和两个 El Torito catalog 条目：ISO BIOS platform 0、EFI platform 0xef；同一 kernel/root 模块字节一致。raw ESP/root UUID 保持原角色，BIOS Boot 未格式化；目标原子发布失败保留旧完整输出。测试 ESP 增大、root 不适配、缺任一平台输入、不同模块目录/工具身份的增量重建。QEMU argv capture 验证 bios 无 OVMF 参数、uefi 缺固件失败，未知值失败。
- [x] **实现磁盘与 ISO。** 新三分区布局，标准上游工具嵌入 BIOS loader 后再转换 VMDK并校验。构建私有 raw 上的 device.map，绝不接触宿主块设备。ISO 合并 BIOS El Torito no-emulation 和现有 EFI FAT 入口，选择正确平台模块。Make 参数身份和依赖覆盖工具/模块/配置；菜单沿用当前 boot 分区，移除所有活动 `(hd0,gpt1)` 强制跳转。
- [x] **实现运行策略。** Make/帮助/测试入口共用明确的 `QEMU_BOOT` 分支；BIOS 不探测/要求 OVMF，UEFI 不回退到 BIOS。smoke 的固件标记必须与预期一致，GRUB 横幅不能算内核启动通过。
- [x] **验证。** `make O=out/dualboot-build -j4 image-vmdk iso installer`、两个新 shell 测试退出 0；实际提取三介质 kernel 哈希等于当前内核。分别删除 BIOS/EFI 单个输入、替换工具参数后验证失败/重建，不允许使用旧镜像假通过。

### Task 6: 实际安装/双启动验收、文档和残留审计

**Files:** 新 `tools/test_dual_boot_qemu.py`、`docs/build/dual-boot-2026-10-22.md`；现有 `tools/test_installer_{accounts,update}_qemu.py`、`tools/test_lsblk_filesystems_qemu.py`；更新 `README.md`、`docs/{BUILDSYSTEM.md,BOOT_AND_INTEGRITY.md,ADVANCED_INSTALL.txt,ADVANCED_INSTALL.md}`、`scripts/doctor.sh` 及相关发行说明。

**Interfaces:** `python3 tools/test_dual_boot_qemu.py --out out/dbq --images out/dualboot-build/images --matrix all` 顺序调度本任务唯一 VM，生成场景化 JSON/串口日志/截图/哈希；命令非零表示某验收失败。只清理自己创建的进程和私有磁盘副本，不改变源镜像。

- [x] **先证明 runner 会失败。** 用旧 UEFI-only 介质运行 BIOS 启动用例，必须报缺内核交接；故意缺少来宾完成标记的 fixture 必须超时失败。不能只匹配串口历史输出、GRUB 页面或截图存在。
- [x] **运行最终三介质矩阵。** 同一最终 Installer ISO、Live ISO 和 VMDK 各在 SeaBIOS/OVMF 到达相应安装器/登录环境。默认安装分别从 BIOS/UEFI 启动安装器完成，移除 ISO 后每块目标盘分别用两固件启动；至少一次完整 GUI 交互、一次 TTY 交互。仅 BIOS/仅 UEFI/none 均真实安装，检查对应启动成功或缺 loader 的预期失败，不能把 none 的不可直接启动视为产品故障。
- [x] **运行回归/负例。** 验证 XDM→IceWM、TTY 登录、内存/ACPI/SMP/framebuffer/磁盘、POSIX I/O 与重启。双盘逆序和同名文件不能换错 root；旧两分区 UEFI 更新前后 GPT、用户文件和 rollback 一致，添加 BIOS 缺分区请求在写入前失败。NVMe 路径在宿主与 UEFI 场景覆盖，不把 SeaBIOS 缺 NVMe 固件驱动归为内核已支持。
- [x] **干净源码和完整测试。** 像 Task 0 一样从候选 Git 文件清单导出父/子树，核对新源、Kbuild 和依赖清单；执行独立 Release/Debug、增量/clean 重建、父仓 `make test`/`make test-long` 和 K 的 `make -k test`。逐名报告失败并与本任务前基线比较，不能重生成 golden 或扩大白名单。只在新增失败/修复后重跑相关检查。
- [x] **更新并审计当前文档/残留。** 手动安装步骤使用 BIOS Boot+ESP+root、实际工具和选项；删除退役 `efi-stage.sh`、重复自动布局、无条件 EFI 发布和两分区 fixture。扫描活动消费者的旧脚本名、硬编码 partition index、UEFI-only 帮助及缺模块 fallback；逐项处理。历史报告保留日期并标明已由本次契约替代，兼容旧命令只留单实现薄包装。`git diff --check`、模块布局和 Kbuild 引擎摘要检查通过。
- [x] **最终独立审查与交付。** 审查最终 diff/边界/证据，修完有效问题后核验受影响场景。报告源码、编译、打包、宿主测试、QEMU、VMware/物理硬件的不同结果，提供 BIOS/UEFI 安装器与已安装桌面的实际截图及哈希。不提交、不推送、不计未运行平台为通过。

## 执行次序和审阅

Task 0 的修复和证据已存在；其余均未实现。Task 1 → 2 → 3 → 4 → 5 → 6 依次推进。
每项的失败/成功命令、退出码、变更范围和剩余问题填入执行记录，生产实现不因计划写完而算完成。

计划自审覆盖：内核交接、三分区、四种安装选择、旧盘更新、三介质两固件、标准 ABI、
第三方边界、Kbuild、文档/残留与源文件交付均有对应任务；五项 Review Focus 均已落到测试。

推荐 Native：由当前主代理顺序实现，最终使用用户指定的 GPT-6-Luna / Max 做一次
独立审查。任务共享 GRUB 输入、分区结构和安装事务接口，顺序实现可减少交接成本。
另一可选方式为逐任务子代理实现/复核；只有用户选择后才按该方式执行。
