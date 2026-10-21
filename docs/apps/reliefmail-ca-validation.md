# ReliefMail CA 修复验证记录（2026-10-21）

基线为主仓 `8966d129344aa6c7b107c545a9497518a6af7c8d`，分支
`rewrite/desktop-apps`；内核 gitlink 为
`c91b7d25fac869df86f63400988e7f7b8b09a61d`，没有修改内核跟踪文件。
使用方法与更新注意事项见 [证书与连接排查](reliefmail.md)。

## 修复依据

- 原代码把 `mbedtls_x509_crt_parse` 的任意非零结果当作整库失败；正数实际表示
  部分成员失败。原生系统实测有 69 个可用根、54 个未解析成员，使用可用根后，
  `imap.163.com:993` 和 `smtp.163.com:465` 均通过完整 TLS 验证。
- TLS 校验结果 `UINT32_MAX` 表示尚无校验结果，不能按“不受信任的 CA”处理。
- apk 的配置保护可能把新文件留为 `.apk-new`，并保留损坏的旧文件；apk 3 的
  此项保护按目录生效，已用真实 apk 验证生成目录排除规则。
- 只更新汇总包会被 `update-ca-certificates` 按旧源重新生成；两个 CA 包必须
  一起维护。旧镜像的 CA 源包 world 精确版本约束也需要显式刷新。
- 真实 apk 的 `fix` 可能输出 `APK unavailable, skipped` 并返回 0。修复工具
  现在检查输出，遇到这种情况报告失败；仓库 URL 被隐藏，防止日志包含凭据。
- 独立只读审查发现的环境配置继承和信任路径问题已修复：apk 使用最小环境，
  忽略调用者配置；可解析的文件仍需满足 root 属主、不可组/其他用户写入、
  常规文件及父目录安全条件。符号链接不会被特权修复程序直接信任。

## 构建与镜像

以下命令成功，最终日志为 `out/reliefmail-ca-final-build.log`：

```sh
TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug -j4 \
  app-reliefmail app-reliefapk nls image-vmdk
```

从最终 `images/disk-root.ext4` 只读读回并逐字节核对：Mail ELF、APK ELF、
运行库、出厂 CA、apk 保护规则及中文 MO。证书汇总文件仍归
`ca-certificates-bundle`，恢复 CA 和保护规则归 `reliefos-trust`。
证据为 `out/reliefmail-ca-image-readback.json`。

| 程序 | 最终 SHA256 |
| --- | --- |
| ReliefMail | `b62c33a01f1ccac2c3c3cdcb29e32616a41ca5e06ac5be1080988e2dd763d475` |
| ReliefAPK | `5ff63fb386e553d5cba03cd4bfaa91ee8fd70b0f9126a14f8703e4a6bb5d5c6e` |

## 定向测试

全部通过，命令见使用文档；本地产物不提交：

| 范围 | 结果与日志 |
| --- | --- |
| 邮件协议及 TLS | `out/reliefmail-ca-protocol.log`：协议测试通过，94 次 AUTH 均加密，记录已脱敏。包含部分 CA 解析、缺失/空/损坏/FIFO CA、错误域名、过期、不受信任及非 TLS 端点。 |
| 邮件工作进程 | `out/reliefmail-ca-worker.log`：通过。 |
| UI 与适配器 | `out/reliefmail-ca-ui-layout.log`、`out/reliefmail-ca-adapters.log`、`out/reliefmail-ca-detail-widget.log`：通过。 |
| APK 模型 | `out/reliefmail-ca-review-model.log`：固定目标、参数、权限与模拟规则通过。 |
| CA 文件和输出处理 | `out/reliefmail-ca-restore-final.log`：ASan/UBSan 通过；覆盖备份、原子恢复、符号链接、属主、权限、损坏数据及跳过操作/URL 脱敏。 |
| 子进程 | `out/reliefmail-ca-process.log`：99 项通过。 |
| 真实签名 APK | `out/reliefmail-ca-apk-integration.log`：42 条命令通过，包括 CA 源文件缺失修复、汇总文件损坏替换、两个 CA 包升级和解除旧 world 精确版本约束；模拟不改文件或 world。 |
| rootfs 与翻译装配 | `out/reliefmail-ca-rootfs-stage.log`、`out/reliefmail-ca-nls-stage.log`：通过。 |

全量 `make test` 未通过：现有 `test-apk-ownership.sh` 报
`component policy mismatch: ['soundctl']`，日志为
`out/reliefmail-ca-root-tests.log`。没有擅自更改 audio/soundctl 归属语义。

## QEMU 运行证明

使用 `tools/test_reliefmail_ca_qemu.py` 和私有磁盘副本。源镜像为
`out/reliefmathpp-baseline.raw`，前后 SHA256 均为
`9a6be9824d1534eed85e73c7e718e967b45c2752786151029c9d1a0e3930e266`。
最终结果在 `out/mail-ca-q5/result.json`，测试进程退出码为 0。

实际完成：普通用户中文界面中复现损坏 CA → 查看恢复计划 → sudo 认证 →
恢复并备份原文件 → 经 HTTPS 获取私有签名的两个 CA 包 → 运行真实证书生成钩子 →
返回 Mail 重试 IMAP 认证成功。出厂恢复库不包含 IMAP 测试服务的签发者；只有
签名更新和生成钩子保留新增源后才能认证，不能用“恢复旧库”冒充更新成功。

测试 CA 包版本 `99999999.0-r0` 仅用于该私有副本。关机后独立读取文件与数据库：

- 两个 CA 包版本一致，均为夹具版本；两份新增 CA 源文件与夹具逐字节一致。
- 生成的 CA 库包含 123 个证书，包含两个测试签发者。
- 原损坏文件的独立备份仍存在，内容与最初夹具一致。
- 认证记录全部经过 TLS；没有 SMTP DATA 或 IMAP APPEND，邮箱密码未进入串口日志。
- 163 的 IMAP 993、SMTP 465 仅做 TLS 验证，均成功；没有使用真实邮箱凭据或发信。

图形事务使用的 APK ELF 为
`9915c585962805a0aa63d9d5497d849413b39b22d00d59a447f322d002b9254f`。
最后增加信任路径保护后，将上表最终 ELF 通过私有签名测试包送入同一虚拟机，
校验 SHA256 并替换测试副本中的程序。最终 ELF 实际验证了：可写 CA、错误属主、
符号链接、可写目录四种场景均在启动任何 apk 进程前拒绝；root 拥有的损坏常规
文件仍可只读预览恢复计划；正常用户在健康 CA 下在线模拟成功。关机后再次读取
最终 ELF，SHA256 与最终默认镜像一致。没有把最后权限补丁前的整段 GUI 事务
宣称为最终 ELF 重新完整执行过的事务。

本轮高级页文字识别和窗口焦点检查曾中断，人工检查实际画面后在同一虚拟机继续，
这些脚本问题已修正；结果明确记录了恢复执行，未宣称全程无人干预自动通过。
此前汇总包被生成钩子覆盖的运行作为失败证据；另一次夹具版本低于日期式源包版本，
没有升级源包，也未计为通过。

可见截图：

- `out/mail-ca-q5/reliefmail-ca-error.png`
- `out/mail-ca-q5/reliefmail-ca-recovery-plan.png`
- `out/mail-ca-q5/reliefmail-ca-repaired.png`
- `out/mail-ca-q5/reliefmail-ca-retry-success.png`
- `out/mail-ca-q5/reliefmail-certificates.png`

默认 VMDK 已构建并核对内容，未直接启动验收。QEMU 使用独立的已知基线副本及
本次程序/内核，不能将二者混同。**VMware 验证未完成**，真实硬件未验证。
