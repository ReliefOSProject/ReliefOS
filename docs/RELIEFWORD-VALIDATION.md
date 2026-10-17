# ReliefWord 首版验证记录

日期：2026-10-17。工作分支：`rewrite/desktop-apps`；基线提交：`73476568873f60e6a7d4386811f0e21e040ef0a1`。

## 编译

```sh
TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug -j4 app-reliefword app-fileman nls
```

目标构建通过；最终普通构建日志为 `build/reliefword-dev/build-focus-final.log`。GUI 翻译单元另以 `-Wall -Wextra -Werror` 做语法检查通过（`ui-syntax-escape.log`）。全局额外启用 `-Werror` 会遇到既有 musl SDK 的 endian 宏警告，因此不声称整个 SDK 在该选项下无警告。临时键盘事件诊断代码已移除，验收使用普通 ELF。

最终 `out/xorg-vmware-debug/userland/reliefword.elf` SHA-256：

```text
9db01f15631d628ab7c58ecca93075b237064b9c89c7a3d076eae5518312ee65
```

## 主机及集成测试

`TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefword.sh` 通过，日志为 `build/reliefword-dev/host-tests-final.log`。模型、文件读写和分页三组测试使用 ASan/UBSan、禁止异常和 RTTI，并启用 `-Werror`。覆盖 Unicode 与编辑原子性、撤销后分支的版本唯一性、打开文件清空历史、原生格式往返、TXT/HTML、畸形和过限输入、外部文件冲突、恢复锁、分页和命中定位。

以下六组相关测试均通过，对应日志位于 `build/reliefword-dev/test-*.log`：

- `tests/build/test-app-icons.sh`
- `tests/build/test-components.sh`
- `tests/build/test-desktop-backend.sh`
- `tests/build/test-installer-stage.sh`
- `tests/build/test-apk-ownership.sh`
- `tests/build/test-fileman-boundary.sh`

独立的核心和 GUI 审查发现的问题均已修复并复核；审查记录在 `build/reliefword-dev/core-review/report.md` 和 `build/reliefword-dev/ui-review/report.md`。

## QEMU 系统内运行

最终全流程退出码为 0，`build/reliefword-dev/qemu-file-dialog-accepted/result.json` 中 `passed` 为 `true`。完整日志为同目录外的 `qemu-file-dialog-accepted.log`；最终目标 ELF 与注入、读回哈希一致。

运行命令：

```sh
TMPDIR="$PWD/out/tmp" python3 tools/test_reliefword_qemu.py \
  --image build/fltk-migration/desktop-ui-compact-fileman-10/guest-private.raw \
  --kernel build/kernel-throughput-5g/final/kernel.sys \
  --output build/reliefword-dev/qemu-file-dialog-accepted --recovery
```

环境为 KVM/QEMU、2 vCPU、4 GiB 内存、VMware VGA、USB tablet、1280×800；使用既有 Xorg 镜像的私有克隆注入最终 ELF、翻译、入口、图标和测试夹具。该环境不能代替 VMware 实机验证。

实际完成中文显示和 X11 UTF-8 粘贴、快速数字输入、18 磅粗体居中、富文本复制、撤销重做、全部替换、Escape 关闭查找并返回正文、文末分页、横向页面设置、保存重开、TXT/HTML 导入导出、导入来源覆盖保护、未保存提示和崩溃恢复。最小 860×540 窗口的状态栏与功能区滚动已截图检查；X11 图标探针确认窗口图标尺寸、透明度和 WM_CLASS。

虚拟机退出后，从磁盘提取以下 7 个文件，由独立进程解析并全部通过：

| 文件 | 独立检查 |
| --- | --- |
| `word-fixture.rword` | 完整文字尾部、两份 18 磅粗体居中文字、分页符、横向页面 |
| `acceptance.html` | 上述文字及 HTML 格式、分页和页面设置往返 |
| `acceptance.txt` | 完整中文/英文文字尾部和分页符 |
| `imported.rword` | HTML 中文标题、预格式空格/换行、等宽字体、链接可见文字 |
| `text-imported.rword` | UTF-8 中文与 CRLF 换行规范化 |
| `branch.rword` | 保存 A、撤销、编辑 B 后仍出现保存提示；取消关闭再保存得到 B |
| `recovered.rword` | 等待真实 60 秒自动恢复，SIGKILL 后恢复未保存的 `Recovery marker 2468`，并保留原文和排版 |

源镜像 SHA-256 保持 `c8a4240459c7981f5b5154158e865717f390d6d57be3e7755c5f960bec1be9ec`；导入的 HTML 原文件哈希也保持不变。内核读回哈希和逐文件注入哈希记录在 `qemu-file-dialog-accepted/prepare.json`。

实际截图：

- [中文文档主界面](../build/reliefword-dev/qemu-file-dialog-accepted/reliefword-document.png)
- [最小窗口](../build/reliefword-dev/qemu-file-dialog-accepted/reliefword-minimum.png)
- [页面设置](../build/reliefword-dev/qemu-file-dialog-accepted/reliefword-page-dialog.png)
- [恢复后的未保存内容](../build/reliefword-dev/qemu-file-dialog-accepted/reliefword-recovered.png)

较早诊断运行中发生过测试焦点时序和文件选择框固定坐标错误；其失败记录不作为全流程通过证据。最终脚本等待外部 X11 探针报告的焦点状态，并直接使用 FLTK 文件选择框默认聚焦的文件名输入框。保存结果必须由 VM 外独立进程解析，验证精确文字尾部、分页符、样式和页面设置；只出现截图不计作通过。

## 验证范围

未重建发布 ISO；未修改内核、XDM/IceWM 主题、背景、音频声明或 ABI golden。首版的格式和 Unicode 排版边界见 [使用说明](RELIEFWORD.md)，不提供 DOC/DOCX、表格、图片、打印或 PDF。中文显示及 UTF-8 粘贴在 QEMU 验证；未逐个验证第三方中文输入法。**VMware 验证未完成。**
