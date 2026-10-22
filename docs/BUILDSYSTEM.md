# Linux Kbuild / Kconfig 构建系统

根 Makefile 是生产入口，要求 GNU Make 4.3+ 和 Linux/WSL。运行 `make help` 查看完整公共接口。
自研生产 C、C++、汇编与宿主工具由固定的 Linux 6.12 原版 Kbuild 规则编译。
项目 C 工具负责格式转换与校验，产品 Make 规则集成第三方、SDK、包、镜像与测试。
第三方项目保持现有原生构建与移植边界；生产链不执行 Python/Meson/Ninja，不需要 Rust 工具链。

上游来源、摘要与许可证见 `scripts/kbuild-source.json` 和 `LICENSES/`。项目扩展与原版文件
分开，C++ 扩展沿用 `if_changed_dep` 与 fixdep。迁移证据和残留审计见 [迁移记录](build/kbuild-migration.md)。

## 开始构建

```sh
git submodule update --init --recursive
make doctor
make fetch
make defconfig
make -j8 all
```

依赖安装示例见 README。doctor 会实际编译/链接目标探针，检查 compiler-rt、
ext2fs 头文件/库和镜像工具。fetch 校验锁定摘要，是唯一联网阶段；缺缓存的生产目标直接失败。

## 输出与配置

默认 `O=out/x86_64/release`；`ARCH=x86_64 PROFILE=debug|release` 选择配置隔离。
`make O=out/custom menuconfig` 编辑该树 `.config`；olddefconfig 保留设置并补充新项，
defconfig 重置为 configs/default.conf。可以复制 `.config` 保存配置，再 olddefconfig。
Kconfig 菜单（Build、Image defaults 等）决定默认配置，configs/components.toml 用 stage/entry/sdk/api 字段区分组件归属；required 组件强制开启。

从旧构建目录沿用配置时，先将旧目录的 `config/.config` 复制到新 O 的 `.config`，再运行
`make O=新目录 olddefconfig`。推荐使用新 O 验证迁移；旧镜像、下载缓存及其他用户文件不会被迁移脚本删除。
旧对象、`.o.d` 和签名文件不作为新 Kbuild 的输入。

生成文件只写 O。`SOURCE_DATE_EPOCH` 默认为提交时间，仅用于时间元数据与可复现打包；
版本为 `major.minor.patch`，没有构建号覆盖或计数器，提交身份另存 RELIEFOS_SOURCE_ID。

内核基础版本为 `5.1.0`，由 `kernel/reliefnt/configs/build-version` 定义。
`uname -r`、`/proc/sys/kernel/osrelease`、`/proc/version` 和系统信息使用同一份
生成的内核版本，包含后缀；不再单独报告 `6.12.0`。

后缀采用 Linux Makefile 的 `EXTRAVERSION` 和 `LOCALVERSION` 变量，按顺序直接拼接：

```sh
make -j8 EXTRAVERSION=-perf kernel                # 5.1.0-perf
make -j8 EXTRAVERSION=-perf LOCALVERSION=-test kernel # 5.1.0-perf-test
make -j8 kernel                                  # 5.1.0（默认无后缀）
```

也可以修改根 `Makefile` 或 `kernel/reliefnt/Makefile` 的 `EXTRAVERSION` 默认值。
根 Makefile 的非空设置或显式命令行设置会传给内核；否则采用内核 Makefile 的默认值。
切换后缀会重新生成版本头并重建内核，无需 clean。后缀仅接受字母、数字、`._+-`，
完整版本最多 31 字节，以适配现有系统信息 ABI。包和发布元数据继续使用数字基础版本。

正式发布的版本提交、跨仓更新范围、Release 说明和推送顺序见
[发布规范](RELEASE_PROCESS.md)。`system/rootfs/etc/os-release` 的 `VERSION_ID`
必须与主仓 `configs/build-version` 的发布版本一致。

| 目标 | 输出 |
| --- | --- |
| kernel / kerneldebug | ReliefNT 独立 Kbuild；发布到 generated/system |
| runtime / userland / reliefos-pam / reliefos-upstream | 运行库、应用、独立上游安装树 |
| sdk / musl-sdk | packages/reliefos-musl-sdk.tar.gz |
| rootfs-raw / rootfs / apk-repo | rootfs/raw、managed、manifest.json；packages/apk/repository |
| image-vmdk | images/reliefos.raw、reliefos.vmdk |
| iso / image-iso | images/reliefos-live.iso |
| installer | images/reliefos-installer.iso |
| rpr-pages | rpr-pages/，只生成本地 RPR 机器接口与人类页面（apk/、kernel/、packages/、css/） |
| pages | pages/，完整 GitHub Pages 部署树（首页、download/、rpr/、docs/）；依赖 installer 与 rpr-pages，docs/ 由构建系统扫描 `docs/` 目录并用 awk 渲染 Markdown 为无 JS 静态 HTML |
| all / release | all 含 SDK 和三类镜像；release 再含完整 pages/，仍不自动上传 |

## 增量、并发和诊断

`-jN` 由 Make jobserver 控制，嵌套上游 Make 继承它。项目组件不嵌套另一个调度器。
原生 `.cmd` 保存真实命令，fixdep 跟踪头文件与配置依赖。源码成员显式登记在 Kbuild；
增加或删除源码时同步对象清单。产品参数清单由原版 `if_changed` 生成，不另建编译签名引擎。
同一 O 的两个真实构建互斥；不同 O 可以并行。不要手工设置内部锁 token。

`V=1` 显示命令，`--trace` 解释规则，`-n` 预览，`-p` 查看数据库。
检查模式不会重建被包含的配置文件。首次预览应用和产品先 defconfig、config-sync。
`-n/-q` 在未初始化输出树上要求先执行 defconfig；产品选择清单缺失时要求 config-sync。
检查不会创建输出目录或补写选择清单。
目录 stage 从空树组装并记录所有成员；缺失非主文件或权限/链接变化可触发重建。
上游 package 保持独立 root；rootfs 冲突需要显式 override。

clean 保留 `.config`；distclean 清该树配置。两者都检查所有权、拒绝危险路径并保留下载缓存。
历史 build/、buildsystem/cache/ 和用户镜像不在清理范围。

## SDK、签名和运行

SDK 是可重定位 musl sysroot（`packages/reliefos-musl-sdk.tar.gz`）：随包 C 驱动
`reliefos-musl-cc` 按自身位置定位 sysroot，支持重定位、动态与 STATIC=1。
SDK 的可选头/库由组件选择控制。

APK 使用上游 apk 的真实 mkpkg/mkndx/add，包含数据库和签名。
默认密钥为 `~/.local/share/reliefos/apk-signing/key.pem`（0600），可用 APK_SIGNING_KEY 指定。
默认版本号 `2.<SOURCE_DATE_EPOCH>-r0` 排在旧 gen-1/gen-0 媒体之后。
正式发行需要递增 epoch 或显式递增 APK_BUILD_VERSION；同一提交的脏工作区不保证内容哈希排序。
本地 world 请求保持未锁定，升级可替换系统包；外部包与受保护配置保留。

`make run`、run-debug、run-iso、run-installer 使用 Kconfig QEMU 设置。
可覆盖 CPUS、MEMORY、QEMU_BOOT、QEMU_FIRMWARE、QEMU_KVM、QEMU_DISPLAY、QEMU_SERIAL、QEMU_AUDIO。
`QEMU_BOOT=uefi` 为默认值，要求 OVMF 固件；`QEMU_BOOT=bios` 使用 SeaBIOS，
不需要 OVMF。未知值报错。无 KVM 的主机用 `QEMU_KVM=0`。
同一 Installer ISO、Live ISO、VMDK 默认均可通过两种固件启动；输出路径不变。

## 验收边界

- test：C 单元测试及 ASan/UBSan、Shell 构建契约。
- test-long：生产 execve、并行/中断恢复、缺失 stage 恢复；需要较多磁盘临时空间。
- test-legacy：明确选择的既有 Python OS 主机回归，不参与生产构建。
- test-smoke：顺序覆盖三种介质的 BIOS/UEFI 启动；检查对应固件、内核完成、PID 1 和用户环境。
  每次运行保存全新日志、截图和输入哈希，超时及 GRUB 横幅均不能算通过。

可用 `TMPDIR=$PWD/out/test-tmp` 避免 tmpfs 太小。结果以 verification.md 最新记录为准；
生成镜像或通过主机测试均不能替代来宾安装/升级验收。历史参考 Python 的保留边界见 legacy-removal.md。
