# 完整 GNU coreutils 与 BusyBox 命令归属实施计划

**目标：** 发布锁定的 GNU coreutils 9.12 全部 109 个命令，保留 BusyBox 的启动、救援与显式 applet 调用，在 ReliefOS 内验证行为并对常用工作负载测速。

**架构：** 使用上游 `--enable-single-binary=symlinks`，启用默认不安装的程序与 SELinux 工具入口；所有 GNU 命令及 stdbuf 辅助库由 coreutils 包持有。BusyBox 链接生成器按已选构建计划抑制同名入口，并将历史路径转发到 GNU。BusyBox shell 不优先解析内部 applet，显式 `/bin/busybox NAME` 保持可用。

**约束：** 不修改用户已有安装器工作，不提交、推送或建立分支；所有编译临时文件使用本仓 `out/tmp`。不声称内核具备 SELinux 等未实现能力；区分正常功能、预期不支持和异常失败。

- [x] 在 `tests/build/test-coreutils-links.sh` 验证跨目录 GNU 链接、已有路径不重复、BusyBox 独有命令保留；先证明当前生成器缺少历史路径转发。
- [x] 在 `tools/test_apk_ownership.py` 验证 GNU 多调用文件、命令链接、旧路径链接、stdbuf 库和手册归属同一包；先观察现行策略失败。
- [x] 增加 `userland/coreutils/programs.list` 和 `tools/build/coreutils-check.sh`，核对上游程序清单、全部命令、目标链接和 stdbuf 库；通过缺失/错误目标/非可执行文件夹具验证拒绝发布不完整套件。
- [x] 修改 `tools/build/upstream.sh` 与 `scripts/reliefos/Makefile.upstream`，启用完整 GNU 单文件发行方式，取消 dd/yes 裁剪，登记并检查全部产品和构建依赖。
- [x] 扩展 `tools/build/busybox-links.sh` 生成旧路径 GNU 转发，更新 APK 归属、升级冲突声明与依赖。检查非 usr-merge 根中的 `/bin` 和 `/usr/sbin` 调用，以及 GNU hostname 与 OpenRC 调用的兼容性。
- [x] 编译 BusyBox、coreutils、当前内核和镜像，运行相关合同测试与项目要求的构建检查；检查最终 APK 文件归属、所有命令目标和 ELF 依赖。
- [x] 使用私有镜像运行 `tools/tests/coreutils_guest.sh` 与性能探针，覆盖全部命令入口、实际操作、标准失败状态及存储/文本/哈希工作负载；同一内核、相同数据、同一虚拟机内比较 GNU 与显式 BusyBox。
- [x] 在 `Documentation/admin-guide/coreutils.rst` 记录命令归属、内建命令区别、使用方法、测试范围及可重复测速方法，并报告真实结果与平台限制。

验证输出保存在 `out/coreutils-full-20261031/`。命令清单以锁定归档的 `build-aux/gen-lists-of-programs.sh --list-progs` 为外部依据，`ginstall` 映射为 `install`，辅助库另行检查。测速报告至少包含数据规模、重复次数、中位耗时、吞吐与校验结果；不以 `--version` 成功替代实际功能测试。

已执行证据：定向链接、完整 stage、APK 归属、根目录 staging、上游 Make 边界、BusyBox profile 与测试归属检查通过；完整 VMDK 构建通过。真实签名 APK 升级将 88 条旧 BusyBox 路径转交 GNU，升级后的 143 条 GNU 路径归属检查通过。`make test` 在既有 `out/audio-hda` 音频产物缺失处中止，不能标为全绿。

目标运行调查：可选 CRC 加速后端引入了宿主 compiler-rt 的 AVX2 CPU 检测构造函数，开机触发用户态 invalid opcode；已使用上游 `gl_cv_crc_pclmul=no` 通用 CRC 路径消除该依赖并重新构建镜像。已在 4 vCPU 下观察全部 109 项版本/入口与功能检查通过，SELinux 两项按预期拒绝；多核 PIT 回退不适合发布长 I/O 的墙上时间，最终速度必须由单 CPU 高精度时钟门禁通过后的运行提供。

最终验收：`out/coreutils-full-20261031/qemu-accepted/results.json` success=true，versions=109、functions=109、unsupported=2、failures=0；可信 TSC 高精度时钟门禁、全部 12 组性能输出校验与源镜像未修改检查通过。数据规模为四组 32 MiB、排序 2.25 MiB、真实 ext4 复制 1 MiB；保留预热后的三次样本与中位数。完整结果见同目录 `report.md`；VMware 验证未完成。
