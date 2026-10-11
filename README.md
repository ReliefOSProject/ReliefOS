# ReliefOS
<p align="center">
    <img src="logo.png" alt="ReliefOS logo" width="240"/>
</p>

此代码库是 **ReliefOS** 项目的源代码仓库，此仓库的开源协议位于[LICENSE](LICENSE)。

## 感谢
感谢 [@VasilyZa](https://github.com/VasilyZa/) 对本项目 Linux ABI 和 musl libc 等方面作出的贡献。

## 编译源代码

构建环境为 Linux/WSL，入口为 GNU Make 4.3+。项目自己的构建工具使用 C，生产构建不运行
Python、Meson 或 Ninja；生产链只编译 C 与汇编，不要求 Rust 工具链。

Debian/Ubuntu 的典型依赖：

```sh
sudo apt install build-essential clang lld llvm libclang-rt-dev \
  autoconf automake libtool libtool-bin pkg-config bison flex gperf gettext libncurses-dev \
  grub-efi-amd64-bin grub-common xorriso mtools dosfstools e2fsprogs libext2fs-dev \
  fakeroot fdisk qemu-utils qemu-system-x86 ovmf zip curl xz-utils patch git
```

```sh
git submodule update --init --recursive
make help
make doctor
make fetch
make defconfig
make -j8 all
make run
```

`make fetch` 是唯一联网阶段，校验 `configs/dependencies.lock.json` 中的摘要。
`kernel/reliefnt` 子仓另有独立缓存：`git submodule update --init --recursive` 会初始化它
及其嵌套子仓，首次构建前还需在子仓内执行一次 `make fetch`。
构建缺缓存时会报错，不会暗中下载。`make doctor` 实际检查目标编译、compiler-rt
及镜像工具。Clang 必须包含 x86_64 compiler-rt builtins；仅有头文件不够。

常用目标：`kernel`、`userland`、`runtime`、`sdk`、`rootfs`、`apk-repo`、`image-vmdk`、
`iso`、`installer`、`rpr-pages`、`pages`。`all` 构建 SDK 与三类镜像；`release` 再包含完整
GitHub Pages 网站，不自动上传。`pages` 依赖 `installer` 与 `rpr-pages`，把安装镜像、RPR
机器接口与人类可读页面（首页、下载页、包列表、内核页、文档）组装成单一部署产物
`out/x86_64/release/pages/`，遵循一次构建、一次发布（详见 `docs/RPR.md`）。RPR、下载页
与文档页共用同一份蓝色简洁静态 HTML/CSS，全站禁止 JavaScript；文档页由构建系统扫描
`docs/` 目录、用纯 awk 把 Markdown 渲染为静态 HTML 生成。`rpr-pages` 仍可单独用于本地仅生成
RPR 子树。`run-iso`、`run-installer` 启动对应镜像，`QEMU_KVM=0` 可使用 TCG。

默认产物在 `out/x86_64/release/`：

- `images/reliefos.vmdk`、`images/reliefos-live.iso`、`images/reliefos-installer.iso`
- `packages/reliefos-musl-sdk.tar.gz`
- `packages/apk/repository/`、`rootfs/manifest.json`、`rpr-pages/`、`pages/`

`make menuconfig` 编辑所选 `O/config/.config`；`make olddefconfig` 保留选择并补全新项。
用 `O=out/my-build PROFILE=debug` 隔离不同配置。`V=1` 显示命令，`make --trace` 和
`make -n` 检查依赖；首次准确预览前先 `make defconfig`。同一 O 的两个真实构建互斥。

`SOURCE_DATE_EPOCH` 默认取提交时间，仅用于时间元数据与可复现打包。内核版本取
`configs/build-version`，不附加构建号；Git 身份单独记录。签名密钥默认保存在用户目录，可用 `APK_SIGNING_KEY` 指定，绝不会放入发行目录。
新发行 APK 使用 `1.<epoch>.<content-id>-r0`，排序高于旧 `0.<time_ns>-r0`。
正式发行应使用递增提交时间或显式递增 `APK_BUILD_VERSION`；同一提交的脏工作区内容哈希不保证排序。

验证入口为 `make test`、`make test-long`、`make test-legacy` 和 `make test-smoke`。
后两者分别运行显式 Python OS 回归和真实 QEMU 启动；主机产物生成成功不等于来宾验收通过。
本机 GRUB/OVMF 在 loader 之前的页错误及其验证边界见 [验收记录](docs/build/verification.md)。

`make clean` 清产物并保留配置，`distclean` 同时清配置；均保留下载缓存和旧 `build/` 镜像。
详细规则见 [构建系统文档](docs/BUILDSYSTEM.md)。

## 界面样式

系统默认使用蓝色、直角、平面化的 Metro 样式。管理员可在“设置 → 显示”中切换为完整保留的 Win95 样式；选择会立即应用到 Desktop 和已打开程序，并保存到 `/etc/reliefos/display.conf` 供下次启动的登录、安装器与内核早期画面使用。账户在安装器中创建，OOBE 已移除；普通用户和固定的 `root` 账户均要求 1 至 32 个字符且不含空白字符的密码。Python 与 GCC/binutils 可在安装时独立选择。

## 代码与目录结构

根目录中的主要源码、构建输入和工具按职责组织如下：

- `arch/`：各架构相关说明和预留代码（当前主要支持 x86_64）。
- `boot/`：GRUB 配置与 EFI 模块。
- `Makefile` 与 `mk/`：GNU Make 构建入口和依赖规则。
- `tools/host/`、`tools/build/`：C 数据工具和短上游构建适配器。
- `configs/`：组件清单、默认配置和可提交的构建 profile。
- `docs/`：架构、ABI、构建、文件系统、安全和工具文档。
- `include/`：公共 C 头文件（`reliefos/`，旧 `leonos/` 头转发兼容）与生成头（`generated/`）；UAPI 头在内核子仓 `include/uapi/`。
- `kernel/reliefnt/`：ReliefNT 内核子仓（gitlink，https://github.com/ReliefOSProject/ReliefNT），含内核核心、`drivers/` 与 `include/uapi`；首次使用执行 `git submodule update --init --recursive` 并在其中 `make fetch`。
- `los2w/`：宿主机上的操作系统/Windows 兼容工具和模拟器代码。
- `system/`：镜像中 staging 的系统配置、字体、证书、壁纸、图标和其他资源。
- `test/`：测试输入和测试资源。
- `third_party/`：通过 Git submodule 引入的上游或分叉项目源码，具体归属见 `.gitmodules`。
- `tools/`：构建辅助、资源生成、组件同步、镜像/安装器打包、验证、Doxygen 文档、启动日志分析、许可证归属检查和代码统计工具（包括 `count_code.py`、`analyze_boot_log.py` 与 `check_licenses.py`）。
- `userland/`：用户态运行库、窗口/UI 支持、BusyBox、Fastfetch 及桌面应用源码。

`Kconfig` 和 `Kconfig.components` 定义配置菜单；后者由工具根据组件清单生成，
不应手工维护。`build/`、`dist/`、`buildsystem/logs/`、`buildsystem/tmp/` 等目录
由构建或发布流程生成，不是手写源码的权威来源。

## 代码注释规范

内核子仓 `kernel/reliefnt/kernel/reliefnt/` 的每个函数定义和公共函数
声明都必须使用 Doxygen 风格注释。C、C++ 和汇编预处理源均采用以下块注释形式，
以便 Doxygen 读取：

```c
/**
 * @brief 简要说明函数负责的行为、边界和可观察效果。
 * @param request 输入请求；说明所有权、可空性和缓冲区容量（如适用）。
 * @param out_result 输出结构；调用方提供有效可写空间。
 * @return 0 表示成功，负 errno 表示失败。
 */
int subsystem_handle(const struct request *request, struct result *out_result);
```

- `@brief` 必须描述职责，不能只把函数名改写为一句话。涉及权限、用户指针、硬件、
  锁、引用计数、映射或中断上下文时，简要说明关键前置条件或副作用。
- 每个参数使用 `@param`。明确输入/输出、可空性、所有权转移、字节长度与数组容量；
  无参数函数不写空的 `@param`。
- 非 `void` 函数使用 `@return`，说明成功结果及错误值/特殊值。不会返回的函数标明
  不返回的原因；异步接口还应说明完成或回调语义。
- 注释紧贴其声明或定义。静态私有函数至少在定义处有注释；公共函数在头文件声明处
  和实现处保持一致，不要让两处描述相互矛盾。
- 结构、宏、全局状态与复杂算法仍应保留必要的独立注释；函数 Doxygen 注释不能替代
  ABI、并发、内存安全和错误处理说明。

## 作者注

源代码里还包含神秘的许可证服务端和客户端完整代码，但是这个项目是 Apache 2.0 开源协议且我也打算放弃许可证机制所以就作为纪念保留吧。
