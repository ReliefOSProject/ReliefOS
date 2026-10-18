# ReliefOS 应用图标

图标沿用 ReliefOS 的经典彩色像素风格，仅用于 ReliefOS 自写的图形应用。现有 36 套资源各自可区分，包含 14 个 X11 程序和保留的旧版图形程序资源。控制台命令、后台服务、第三方应用不提供自定义图标。

`configs/app-icons.list` 是生成和安装共同使用的明确清单。sudo、fastfetch、nano、PL Editor、DOOM 游戏本体、PortableGL gears 及 StardustUI 上游示例均不在其中；ReliefOS 自写的 DOOM 启动器保留图标。旧版自写图形终端属于图形应用，和 shell 等命令行程序分别处理。

## 标准路径

公开桌面图标使用透明 RGBA PNG，尺寸为 16、32、48、64、128、256 像素；共 216 个 PNG。安装路径为：

```text
/usr/share/icons/hicolor/16x16/apps/reliefos-fileman.png
/usr/share/icons/hicolor/32x32/apps/reliefos-fileman.png
/usr/share/icons/hicolor/48x48/apps/reliefos-fileman.png
...
```

`hicolor` 的目录定义继续由上游 `hicolor-icon-theme` 包提供。清单内应用的 `.desktop` 使用无扩展名的图标名，例如 `Icon=reliefos-fileman`；不引用私有 BMP 路径。程序的 `StartupWMClass` 与窗口身份一致，方便桌面环境关联入口和窗口。IceWM 的固定菜单及工具栏也引用对应应用图标名。

仅为清单内的已启用组件安装图标，并按组件归属打包。C 和 Python 的归属实现均使用六种尺寸的精确文件规则，不会把整个共享图标目录归给某个程序。禁用组件不安装其图标；控制台安装清理被排除的图形程序及其图标；安装器运行环境单独补齐安装器的资源。未列出的应用生成空的 manifest 图标字段，RPR 的 DOOM 包也不再携带 BMP。

## 窗口图标

计算器、系统信息、Fileman、Taskmgr、扫雷、Leonmmcoset、Xiaobai、画图、图像查看器、DOOM 启动器、安装器、ReliefExcel、ReliefWord 和 ReliefPPT 使用共用 FLTK 图标接入。DOOM 游戏本体不设置自定义图标。

每个程序初始化自己的 `reliefos-程序名` 窗口身份，并通过 FLTK 的 `default_icons` 设置 16、32、48、64 像素的窗口图标。FLTK 将它们发布为标准 `_NET_WM_ICON` 属性，供标题栏、任务栏和窗口切换器使用。

旧 BMP 保留为程序包内的兼容资源及窗口像素来源：读取器仅接受生成器使用的 32×32、未压缩、32 位 BGRA 格式，校验头部和长度，支持上下两种行顺序，保留透明度。读入后转换成 RGBA 交给 FLTK；不改变图片查看器或其他 PNG 解码行为，也不增加图像解码库。图标缺失或损坏时程序仍可启动。

## 维护与验证

`tools/make_app_icons.py` 保存图案绘制代码，使用 Python 标准库生成 BMP 和 PNG。默认只生成清单内的图标；`--apps` 也拒绝清单外应用，避免重生成时再次加入控制台或第三方程序。确定性重生成命令：

```sh
TMPDIR="$PWD/out/tmp" python3 tools/make_app_icons.py \
  --out-dir resources/build-art/app-icons \
  --theme-dir resources/build-art/app-icons/hicolor
TMPDIR="$PWD/out/tmp" sh tests/build/test-app-icons.sh
```

测试覆盖图标范围、生成器拒绝清单外程序、图案唯一性、PNG 尺寸/CRC/透明度、桌面入口解析、窗口像素读取的通道/透明度/行顺序及坏文件拒绝。根文件系统、安装器及包归属的集成测试检查真实安装路径、隐藏入口、禁用组件和不应带图标的命令；RPR 测试核对 DOOM 的包内容及空图标字段。

`tools/test_app_icons_qemu.py` 克隆指定镜像，注入程序及图标并校验读回哈希，逐个启动程序。测试探针独立读取 X11 的 `WM_CLASS`、`_NET_WM_ICON`，逐像素核对四种尺寸的 ARGB 数据，并保留实际桌面截图；不会修改源镜像。测试运行环境为 QEMU，发布 ISO 未重建。

## 本次验证记录（2026-10-17）

- 编译：调整范围前，13 个迁移应用的目标 ELF 均构建成功，日志为 `build/app-icons/build.log`。本次撤销 DOOM 的图标初始化调用后，DOOM 重新编译、链接通过，日志为 `build/app-icons/scope-doom-build.log`；其余图形应用代码不变。
- 测试：范围调整后的图标、PNG/BMP 像素一致性、桌面入口校验和 ASan/UBSan 读取器测试通过；根文件系统、安装器、RPR 包内容检查通过，日志为 `build/app-icons/scope-*.log`。根文件系统测试确认 sudo、fastfetch、soundctl 无 BMP/PNG，并确认 fastfetch 的 manifest 图标字段为空。
- 运行：调整前的 `build/app-icons/qemu-verified/result.json` 记录 13 个程序全部通过，四种窗口尺寸的 ARGB 像素均精确匹配；源镜像未变。本次移除其中 DOOM 的自定义图标，保留的 12 个 X11 应用接入代码及图案不变，未重跑 QEMU。后续图标验证脚本按 12 个自写图形应用运行。

本次运行以 `build/fltk-migration/desktop-ui-compact-fileman-10/guest-private.raw` 为源镜像，使用已有的 `build/kernel-throughput-5g/final/kernel.sys`，未修改内核。复现时选择一个不存在的输出目录：

```sh
TMPDIR="$PWD/out/tmp" python3 tools/test_app_icons_qemu.py \
  --image build/fltk-migration/desktop-ui-compact-fileman-10/guest-private.raw \
  --kernel build/kernel-throughput-5g/final/kernel.sys \
  --output build/app-icons/qemu-new
```

两次较早的复测在 XDM 启动等待阶段超时，尚未执行应用检查；日志保留在 `qemu-final-doom`、`qemu-final`。测试脚本增加显式登录界面就绪等待后，最终全量复测退出码为 0。

**VMware 验证未完成。**

规范参考：[Icon Theme Specification](https://xdg.pages.freedesktop.org/xdg-specs/icon-theme/latest/)、[Desktop Entry Specification](https://specifications.freedesktop.org/desktop-entry/latest/)、[EWMH 窗口图标属性](https://specifications.freedesktop.org/wm/latest/ar01s05.html)、[FLTK 窗口图标 API](https://www.fltk.org/doc-1.3/classFl__Window.html)。
