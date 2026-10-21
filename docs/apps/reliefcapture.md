# ReliefCapture

ReliefCapture 是独立的 C++17 / FLTK 截图与标注工具，使用系统中文字体和 FLTK
默认 gtk+ 控件，位于桌面应用分组，不属于 ReliefOffice。
窗口内直接显示截图与标注工具栏，程序名仅位于窗口标题栏。

## 截图

```sh
reliefcapture
reliefcapture --select
```

按 **Win+Shift+S** 可直接选择截图。桌面先冻结，顶部显示区域、窗口、全屏和取消按钮。
拖动鼠标选择矩形区域；按 W 后点击普通窗口，按 F 截取整个 X 屏幕，按 R 返回区域模式。
Esc 或鼠标右键取消。选区显示实际像素尺寸。

截图后打开编辑窗口，并自动将图像复制到 X11 剪贴板。可直接粘贴到支持图像的应用。
剪贴板由本进程提供，**粘贴前请保持 ReliefCapture 运行**，退出后不保证继续可用。

普通启动打开编辑窗口。“截图模式”可选择区域、窗口或全屏，“延时”支持无延时及
3、5、10 秒。点击“新建截图”或 Ctrl+N 开始。倒计时中可点击“取消”或按 Esc。
调用截图前隐藏自身窗口，完成或取消后恢复编辑器。重复调用快捷键会交给同一用户、
同一 DISPLAY 下的已有实例，不叠加多个选区。

窗口截图取冻结桌面上窗口边框所在的矩形，保留实际可见的遮挡；不恢复被其他窗口
盖住的内容。支持截图的范围是当前 X11 根屏幕，不提供各显示器单独选择。

## 标注与保存

标注工具提供画笔、箭头、矩形和文字，支持七种颜色、四种线宽与四种字号。
选择工具后在图像上拖动画线或矩形，文字工具点击后输入内容。Ctrl+Z 撤销，
Ctrl+Y 或 Ctrl+Shift+Z 重做；新标注会清除已经撤销部分的重做记录。

“适应窗口”方便查看整张图，“100%”以原始像素显示，可用滚动条浏览。
缩放只影响预览，复制和导出的尺寸保持原始截图像素。

- Ctrl+C：复制当前图像，包括标注。
- Ctrl+S：保存 PNG；未填写 `.png` 后缀时自动补上。
- Ctrl+N：按当前模式和延时重新截图。

保存前可以选择位置；替换已有文件需要确认。新文件权限为 0600，保存通过同目录
临时文件和原子发布完成，拒绝符号链接、非普通文件，发布前再次检查目标身份和修改时间。
PNG 编码与磁盘写入在后台执行；保存期间关闭窗口会等待本次保存结束。
关闭前会提示保存未保存的截图，重新截图前会提示放弃未保存的标注。

## IceWM 配置

系统默认键位文件为 `/etc/reliefos/icewm/keys`，经 `/etc/icewm` 使用：

```text
key "Super+Shift+s" /usr/bin/reliefcapture --select
```

新会话会读取这条默认绑定。已有会话需重新登录或重启 IceWM 以重新读取键位。
如果用户自己的 `~/.icewm/keys` 覆盖了系统文件，应将此行添加到该用户文件。
本程序不会覆盖用户自定义键位。

## 支持范围与构建

每边最大 8192 像素，每张最多 16,777,216 像素；最多 512 条标注，每条画笔最多
8192 点，文字最多 1024 UTF-8 字节。超过限制明确提示。初版不含录屏、滚动长截图、
OCR、自由形状选区、自动上传或可继续编辑的分层文档格式。

```sh
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefcapture.sh
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefcapture-x11.sh
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefcapture-ownership.sh
TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug -j4 app-reliefcapture nls image-vmdk
```

X11 宿主测试需要 Xvfb、xauth、X11 和 libpng 开发包。
`tools/test_reliefcapture_qemu.py` 在带 test/test 账户的 Xorg 基线镜像私有副本上测试
真实快捷键、截取、标注、复制、保存和退出；独立 X11 客户端读取剪贴板 BMP，
磁盘只读回收后检查 PNG 和 BMP 像素。QEMU 测试不属于生产构建链。

程序对象由本目录 `Kbuild` 登记。测试工具可用 `--build-dir <O>` 指定实际
构建输出，默认内核也随此目录选择；`--kernel` 可显式覆盖。`--image` 使用带
测试账户的 RAW 镜像，工具仅修改私有副本。

构建、镜像内容核对和私有 QEMU 运行结果分别记录在本次实施计划末尾。
VMware 验证未完成；默认 VMDK 与私有基线副本的运行验证分别报告。

开发者注意：已有文件采用 POSIX 检查后原子替换。身份检查与 rename 之间不提供
跨进程条件式提交，其他程序不应同时写入同一路径。新文件使用不覆盖现有路径的发布方式。
