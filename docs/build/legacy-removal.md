# 旧构建实现删除台账

2026-09-19 Python → Make 迁移的历史记录。当前构建已改用 Linux Kbuild / Kconfig，
见 [Kbuild 迁移记录](kbuild-migration.md)；下表中的旧 Make 结构描述该次迁移完成时的状态。
用户明确允许删除旧 Python 构建系统。
`retired-build-files.txt` 是本轮逐文件删除清单；只删除源码，不删除本机 `build/`、
`buildsystem/cache/`、`buildsystem/deps/` 或旧镜像。

## 已删除与替代者

| 删除范围 | 替代实现 |
| --- | --- |
| `build.py`、受跟踪的 `buildsystem/core/`、组件调度与构建计数状态 | 根 Makefile、`mk/*.mk`；Make owns graph/jobserver；版本由 epoch/显式 BUILD_ID 派生 |
| musl、认证、存储、BusyBox、终端及应用 Python 构建驱动 | `mk/{third-party,pam,upstream,runtime,userland}.mk`、`tools/build/*.sh`；Linux-PAM 使用本项目 Make 移植 |
| Python SDK 打包器及编译器包装器 | `scripts/reliefos/Makefile.sdk`、`tools/build/{musl-sdk,developer-sdk}.sh`、`tools/host/sdk/leonos-musl-cc.c` |
| Python RPR 构建器 | `scripts/reliefos/Makefile.rpr`、`tools/build/rpr-{apps,pages}.sh` |
| Python Kconfig 同步和应用清单生成 | C 配置/组件工具、`scripts/reliefos/Makefile.config`、`tools/build/rootfs-stage.sh`；Kconfig.components 是受跟踪输入 |
| 自研引擎内部图和缓存的特定测试 | Shell/C graph、lock、incremental、bootstrap、components、SDK、APK 和 stage fixture；不再维护已删除类的 mock 测试 |
| `scripts/not-migrated.sh` | 生产目标均有实际规则；test-smoke/test-legacy 也有真实执行入口 |

## 保留的 Python 的边界

- `tools/test_*.py`、QMP、ABI/安全/存储等脚本：现存 OS 回归测试，显式 `make test-legacy`
  或单独调用。部分历史来宾 harness 仍针对旧 `build/` 夹具，并不属于 Make 生产链。
- `tools/{make_image,make_ext2_root,make_live_root,make_installer_root,apk_distribution,apk_ownership,leonos_layout,image_test_accounts,storage_tools}.py`
  及其依赖：历史镜像/升级/来宾回归测试仍导入的参考实现。Make、SDK、CI 生产目标不调用它们；
  保留它们不能作为新链验收成功的证据，也不是自动 fallback。后续迁移各来宾 harness 时再删除。
- `tools/make_*icons.py`、字体/资源 Python 转换器：历史格式回归与 canonical 美术源的再生参考。
  生产使用 C 字体/索引/资源工具及 `resources/build-art/` 的固定原始美术资源。
- `tools/build_api.py`、`tools/build_musl_ltp.py`、离线 GCC/Python 诊断夹具打包器：独立旧格式或 OS 测试工具；
  不是默认发布包生产路径。应用发行统一为真实签名 APK。
- `los2w/`、服务端、维护工具及上游仓库的 Python：不是被替换的构建系统。

`configs/dependencies.lock.json` 的生产 adapter 元数据指向新实现。
CI 的构建/发布步骤使用 Make。README、AGENT 与 BUILDSYSTEM 文档同步迁移。

## 验证边界

以 `verification.md` 最新章节及完整生产 execve 跟踪为准；不能以 grep 命中数代替实际执行验证。
新链目前的 UEFI 运行验证受本机 GRUB/OVMF 在进入 loader 前的页错误阻挡，不能宣称 A15 启动和
来宾内升级通过。旧代码删除不改变或掩盖这一未通过项。
