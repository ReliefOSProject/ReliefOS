# ReliefMath++ 实施计划

**目标：** 交付原生 FLTK 二维/三维数学绘图和计算应用。
**架构：** 表达式字节码、场景/文档模型、几何采样、投影与导出、FLTK 窗口和工具对话框各自独立；复用现有事务文件 IO。
**技术栈：** C++17 无异常/RTTI/标准容器、musl、FLTK、libpng（现有运行库）。直接在当前分支实施，不提交。

- [x] 数学核心：expression.h/.cpp 的 compile/eval 与 analysis.h/.cpp 的 derivative/integrate/root/extrema；先在 tests/host/test_reliefmathpp.cpp 覆盖 -2^2=-4、2^3^2=512、sin(pi/2)、未闭合输入、1/x 跨零拒绝积分和伪根、x^2 积分及 sin 零点，再执行 tests/build/test-reliefmathpp.sh。
- [x] 场景：model.h/.cpp、mesh.h/.cpp；验证圆/球度量、向量积、对象输入范围、采样间断、投影有限、所有几何类型和可取消增量采样。
- [x] 存储：document.h/.cpp 使用有版本及校验的格式；验证 roundtrip、截断、CRC、非法浮点和失败不改变原文档。复用独立进程文件 IO，并验证保存冲突。
- [x] GUI：window.h/.cpp、dialogs.cpp、canvas.h/.cpp、main.cpp。使用标准菜单/工具页/对象属性/二维三维视图；公式编辑、拾点、参数滑块、撤销与导出相连；在最小/默认尺寸验收。
- [x] 系统集成：configs/components.toml、Kconfig.components、configs/fltk-apps.list、configs/app-icons.list、APK 归属、desktop/MIME、rootfs staging/installer、fileman、图标生成；运行组件/归属/图标与 NLS 检查。
- [x] 验证：定向宿主测试、目标构建 app-reliefmathpp nls image-vmdk，检查最终镜像；tools/test_reliefmathpp_qemu.py 使用私有镜像验证二维/三维、公式工具、计算、保存打开/导出并独立读盘，产出真实截图。记录 VMware 验证未完成。

三维函数计算补充：z(x,y) 值、梯度/法向量、拉普拉斯算子、切平面、矩形区域二重积分和曲面面积；同样在可取消工作进程中计算。

独立审查已复核有限差分与求根修复；导数大坐标失真、微小正函数伪根、ln 小正数域、abs 零点不可微均已有回归测试。

最终验收证据：`out/reliefmathpp-qemu-final/acceptance.json`、`persisted-verification.json` 及真实 PNG 截图；最终镜像成员核对见 `out/reliefmathpp-image-verification/verification.json`。全量 make test 的既有 soundctl 归属失败已记录；VMware 验证未完成。
