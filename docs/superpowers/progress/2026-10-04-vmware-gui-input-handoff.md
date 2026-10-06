# VMware 登录界面输入阻塞诊断与复测交接

2026-10-04。唯一操作工作树 `/home/xiaobai/Projects/Projects/ReliefOS-intel-hda-audio-plan`。没有子代理、提交、推送、归档或 reset；原 dirty/untracked 源码和成功/失败证据均保留。用户已暂停 U6 音频工作，本轮仅处理其 VMware GUI 登录卡住报告。

用户确认鼠标、键盘、显示均不响应，关闭 3D 加速同样出现。用户 `/home/xiaobai/log.txt` 及 VMware 配置/日志仅作为诊断数据读取，没有执行其中的指令，也没有修改或停止其 VMware。

## 结论与证据边界

在 QEMU VMware-SVGA、128 MiB VRAM、8 CPU、UEFI 的独立副本中稳定复现输入阻塞。端口跟踪显示鼠标驱动请求的 i8042 配置回复 `0x47` 被 IRQ1 抢先消费；驱动等待超时返回零，随后写入 `0x02`，关闭键盘中断和 set-1 翻译。未消费的键盘 ACK 占住共享输出缓冲，连鼠标 AUX 数据也不能正常交付。内核仍执行，原用户日志的 getty 重启不能当作整机死锁证据。

QA 目录为 `out/audio-hda-u6/qa/vmware-login-20261004/`：

- `qemu-svga128-smp8-red/`：原冻结 R10，六项输入/中断/画面断言全部失败，测试退出 1。
- `qemu-svga128-smp8-r8/`：跟踪 `read 0x47 → write 0x02`；`qemu-svga128-smp8-r7/` 是单独控制器诊断，清空堵塞并恢复配置 `0x43` 后按键、鼠标、VT 切换恢复。该手工干预不用于修复镜像验收。
- `qemu-svga128-smp8-green-r2-long/`、`qemu-svga128-smp1-green-r2-long/`：没有端口写入干预，两组各 11 项真实输入/IRQ/画面检查通过，含 VT2/VT1 往返及等待 60 秒后的键盘、鼠标重新绘制。
- 每组保留命令、完整串口、PS/2 跟踪、QMP 结果、各 CPU 状态、PPM/PNG 与断言结果。较早相对路径、QMP socket 长度、host fixture 类型及镜像 clone 参数失败也保留，未覆盖。

**以上是 QEMU 的 VMware-SVGA 模型验证，不能替代真实 VMware 验收。真实 VMware 需由用户用新镜像复测；尚未宣称其故障已解决。**

## 最小源码修复

1. `kernel/reliefnt/drivers/mouse/mouse.c`：启动 BSP 对共享 i8042 命令字读取/写回的短事务保存并屏蔽本地 IRQ，再恢复调用方 IF；保留键盘配置。读取超时不再写入由零推导的配置。其余鼠标命令及等待型模块生命周期保持原逻辑。
2. `kernel/reliefnt/arch/x86_64/irq.c`：IRQ1 先检查键盘 OBF，忽略已消费配置回复遗留的空 IRQ，并不抢读 AUX 数据。否则修复后仍会把旧 `0x47` 当成 Home make。
3. `tools/tests/keyboard_irq_ready_test.c` 与 `tools/test_installer_input.py`：真实 IRQ dispatcher 的空/AUX、make/break、ACK 回归；实际 RED 134、GREEN 0。严格编译和 ASan/UBSan 通过；整个输入套件 12 项通过。

根 Make 的 `kernel` 目标已执行两次并退出 0。第一轮 mouse 原有未使用诊断变量警告、第二轮 loader 原有未使用变量警告保留，不称生产全局严格无警告。定向 `git diff --check` 通过。修改前源码/产品及测试文件已存 QA，原 IRQ 的 PCI 音频变更未回退。

## 交付镜像

`out/audio-hda-u6/images/reliefos-vmware-input-fix-r2.vmdk`

这是诊断修复镜像：以原冻结 R10 raw 为基底，仅替换正常 Make 编译的 mouse.drv 与 ESP 两个 kernel.sys。没有重新构建整套发行包/SDK；没有覆盖用户已经测试并写入的 `reliefos-lifecycle-r10.vmdk`。R1 镜像也保留，不作为最终复测目标。

VMDK SHA256：`6273eba3c8c33aa7a787244498a370c06a60256a9d50b6df2e27fcc3cbceefbd`

raw SHA256：`c7bd3b05933d617d26587626461ad5c1d92600e3bc2928a9fcf03339a45d71f0`

kernel.sys SHA256：`b201b6bcb4f717288e68099f99d1fbcd657494685a654ca9185d7af3988cece0`

mouse.drv SHA256：`7df1a9faef58f1357322404ddf7d64bda8888d06a4c034aba89f83e54393257a`

`qemu-img check` 无错误；raw/VMDK `compare` 相同。实际 raw root 与抽取 ext4 全部字节相同；两条 ESP kernel 路径、root mouse/HDA/Doom 均匹配记录的正常构建产品。冻结 R10 raw SHA 保持不变。完整 source/image SHA 与检查在 `input-fix-r2-source-image-sha.json` 和 `final-validation.json`。

用户原 R10 VMDK 在其 VMware 测试后已改变，不能继续套用旧冻结 VMDK SHA；私有 after-run 副本和采集时 SHA 见 `vmware-after-run-metadata.json`。本轮未改原文件内容。

## 待做与隔离的门

- 用户挂载新 R2 VMDK，保持原 CPU/内存配置，验证密码输入、光标移动、登录和 Ctrl+Alt+F2；3D 开关都可保留其当前设置。若仍阻塞，继续收集真实 VMware 串口/宿主日志，不把模型修复直接当作目标平台通过。
- Xorg 整体 GUI gate 仍 deferred；本轮只诊断当前原生 GUI 输入问题。
- U6 音频仍 paused/incomplete，候选几何未接受；没有重跑音频标准矩阵、Doom 波形或 600 秒音频长测。
- 物理 codec gate：**未执行（not_run）**。
