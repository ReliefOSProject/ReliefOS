# 计算器多模式增强验证记录

日期：2026-10-22。实现是在同步后的主仓提交
`a4b10bf607d96468c4536191be2ad7540b68e4bb` 上完成的计算器多模式增强。
内核 gitlink 为 `7b502f59f08143d99002652c9c20de99014fc49d`，本任务未修改内核源码。

## 编译与宿主检查

| 检查 | 结果 | 日志或证据 |
| --- | --- | --- |
| `make O=out/xorg-vmware-debug PROFILE=debug -j4 app-calc nls` | 通过，生成目标 ELF 与中文翻译 | `out/calc-app-final-build.log` |
| `sh tests/build/test-calc-modes.sh` | ASan/UBSan 下 1175 项检查，0 失败；18 类、145 个单位 | `out/calc-model-final.log` |
| 用目标 musl 加载器运行链接最终生产计算模块的测试 | 1175 项检查，0 失败 | `out/calc-musl-final.log` |
| `sh tests/build/test-motif-apps.sh` | 通过旧计算引擎与 FLTK 构建/选择相关检查 | `out/calc-fltk-tests.log` |
| `sh tests/build/test-nls-stage.sh` | 通过 | 终端检查 |
| `msgfmt -c --check`、桌面文件校验、Python 语法、`git diff --check` | 通过 | 终端检查 |

上述构建及测试使用 `TMPDIR="$PWD/out/tmp"`。
最终程序为 `out/xorg-vmware-debug/userland/calc.elf`，SHA256：

```text
022c8834e3ea8ae7470411c5085501258c67afddcdd8e7d15ad8150a61a6c4ee
```

测试覆盖表达式优先级、定义域、上下溢、角度/弧度、精确 64 位整数、不同字长的
补码及截断、移位、所有换算单位、温度边界、浮点中间值、统计和公历边界。
目标 musl 与宿主 glibc 对数学函数下溢的 `errno` 行为不同，因此也验证了显式
下溢检测，不能仅用宿主测试替代目标端检查。

## QEMU 真实运行

平台为 QEMU KVM、ReliefOS Xorg、IceWM，使用普通用户启动中文界面。
验收使用 `out/reliefmathpp-baseline.raw` 的私有副本，注入最终程序、翻译、
桌面文件、公共用户态库与独立验证工具。注入后只读回提，核对程序哈希一致。
源镜像在前后检查中保持不变：

```text
9a6be9824d1534eed85e73c7e718e967b45c2752786151029c9d1a0e3930e266
```

显式指定既有已运行内核 `out/mail-ca-q5/readback-reliefos-kernel.sys`，SHA256：

```text
82a334207a55411cd57b8f0f5b8aa9d0497a34c79341abb6bf96950ce83f2a2f
```

这份旧内核上的应用验收不代表当前内核源码可构建，也不代表新默认 VMDK 已启动。

主轮次证据位于 `out/calc-modes-q6/`，`prepare.json` 记录注入与回读哈希，
`serial.log` 记录目标端 1175 项检查全过、实际交互和正常退出；`result.json`
记录最终验收状态。24 次 GUI 数值检查通过原生选择/复制操作，由另一个进程
通过 X11 剪贴板独立读取，逐项保存为 `*.clipboard.txt`，没有把 OCR 结果作为
数值正确性的依据。

| 实际操作 | 验证结果 |
| --- | --- |
| 标准表达式 `(12.5+7.5)*3-5`、内存存取 | 55，切换模式后输入保留 |
| 输入计算后继续运算、输入新数字、等号键 | `7+2` 得 9，`(2+3*4)` 后乘 2 得 28，新输入 9 得 9 |
| 科学计算与角度切换 | `sqrt(81)+sin(30)*10+5!` 得 134，弧度 `sin(pi/2)` 得 1 |
| 十六进制 C 按钮、NOT | C 得十进制 12，64 位取反得 18446744073709551603 |
| 64 位最大无符号数与剪贴板往返 | 18446744073709551615 保持精确 |
| 位编辑和字长/符号切换 | 最低位翻转得 18446744073709551614，8 位有符号显示 -2 |
| 长度、温度、存储、速度换算 | 1500 m → 1.5 km；100 °C → 212 °F；1 MiB → 1.048576 MB；10 m/s → 36 km/h |
| 多行统计 `Ctrl+Enter` 与示例 | 1 至 5 平均值 3；示例平均值 25.5，并显示总体/样本指标 |
| 日期差和加天数 | 2024-02-28 至 2024-03-01 相差 2 日；加 30 日为 2024-03-29 |
| 错误恢复 | 除零显示可见错误，随后 `3+4` 正常得 7 |
| 帮助与关窗 | 帮助打开/关闭正常，计算器退出状态为 0 |

主轮次曾因测试脚本过早识别帮助窗口的绘制帧而暂停，修正脚本后在同一实例
继续完成，`harness_resumed` 为 `true`；不是一次全自动无干预通过。
初次缩小窗口截图也早于重绘完成，因此另用同一最终 ELF 补充验收。

缩放补充证据位于 `out/calc-layout-q7/`：实际客户区为 940×661（USB tablet
拖动有像素量化），缩小后原生复制再次读取结果 7，检查历史区、清空按钮及底部
状态栏均完整，随后正常关窗。该脚本在 IceWM 缩放抓取结束后立即发送快捷键时
暂停，重新点击原生模式控件恢复焦点后完成；补充轮次也明确记录了恢复状态。
最终脚本已改为缩放后等待、点击恢复焦点再取结果和截图。

原始截图（均为 QMP 直接取得的真实像素）：

- [标准](../../out/calc-modes-q6/calc-standard.png)
- [科学](../../out/calc-modes-q6/calc-scientific.png)
- [程序员](../../out/calc-modes-q6/calc-programmer.png)
- [长度换算](../../out/calc-modes-q6/calc-units-length.png)
- [温度换算](../../out/calc-modes-q6/calc-units-temperature.png)
- [数据大小换算](../../out/calc-modes-q6/calc-units-data.png)
- [统计](../../out/calc-modes-q6/calc-statistics.png)
- [日期](../../out/calc-modes-q6/calc-date.png)
- [错误提示](../../out/calc-modes-q6/calc-error.png)
- [帮助](../../out/calc-modes-q6/calc-help.png)
- [缩小窗口补充截图](../../out/calc-layout-q7/calc-minimum-size.png)

这些本地产物位于 `out/`，不应提交入库。

## 未通过或未执行的系统级检查

- 全镜像构建 `make O=out/xorg-vmware-debug PROFILE=debug -j4 app-calc nls image-vmdk`
  被同步后的内核阻塞：`kernel/reliefnt/mm/Kbuild` 引用 `cache/`，但该提交未跟踪
  `mm/cache/Makefile`，报 `No rule to make target .../mm/cache/Makefile`。
  日志为 `out/calc-final-build.log`。本次未生成并验收新的默认 VMDK。
- 全量 `make O=out/calc-host-tests PROFILE=debug -j4 test` 在组件清单检查处报
  `ValueError: component policy mismatch: ['soundctl']`，日志为
  `out/calc-root-tests.log`。没有修改音频 manifest 语义来绕过这一既有问题。
- **VMware 验证未完成**；真实硬件运行未验证。
