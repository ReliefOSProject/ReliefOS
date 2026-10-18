# ReliefAPK 实现计划

> 实施方式：在当前会话连续执行，按模块分工并审查；子代理仅使用 GPT-6-Luna / Max。

**目标：** 交付可在 ReliefOS Xorg 内使用、支持中文和按需提权的 C++17 / FLTK apk 前端，并提供真实截图。

**架构：** `model` 解析 apk JSON、校验输入及构造 argv；`process` 非阻塞执行子进程并桥接 sudo/su 认证；`repositories` 保存配置；`window` 负责 FLTK 列表、详情、事务、工具及设置。所有包查询和事务通过真正的 `/sbin/apk` 完成。

**约束：** 当前工作树实现，不建分支；只在用户明确要求时提交、推送。构建及测试临时文件使用 `out/tmp`。既有未跟踪文件、内核及既有失败项不在本次修改范围。

## 任务 1：结构化软件包模型与命令构造

文件：`userland/apps/reliefapk/{storage.h,model.h,model.cpp}`、`tools/tests/reliefapk_model_test.cpp`、`tests/build/test-reliefapk-model.sh`。

- [x] 写出 JSON Unicode、已安装标志、嵌套依赖、截断输出、超大输入及危险参数的失败测试。
- [x] 实现不依赖 STL、异常或 RTTI 的存储和增量 JSON 包解析；字段未知时可安全跳过。
- [x] 构造查询、安装、删除、升级、重装、修复、缓存、下载、校验、审计等参数数组；包操作使用 `--`，拒绝选项和控制字节。
- [x] 以固定 apk 3.0.8 和实际包数据库验证结构化查询，运行主机测试及 sanitizer。

验证命令：`TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefapk-model.sh`。

## 任务 2：异步进程和认证

文件：`userland/apps/reliefapk/{process.h,process.cpp}`、`tools/tests/reliefapk_process_test.cpp`、`tests/build/test-reliefapk-process.sh`。

- [x] 先测试延迟输出时立即返回、stdout/stderr 分离、退出状态、exec 失败、大输出、取消和子进程回收。
- [x] 使用 fork/exec、非阻塞管道、有界轮询及独立进程组，禁止 shell 拼接。
- [x] 实现 sudo 标准输入密码提示、su 的伪终端认证、密码清理、认证取消；不修改系统策略。
- [x] root 模式直接执行；普通用户只提权固定工作进程。

验证命令：`TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefapk-process.sh`；密码错误和正确路径另在私有 QEMU 镜像中验收。

## 任务 3：仓库配置

文件：`userland/apps/reliefapk/{repositories.h,repositories.cpp}`、`tools/tests/reliefapk_repositories_test.cpp`、`tests/build/test-reliefapk-repositories.sh`。

- [x] 先测试注释和标签保留、条目启停、URL 校验、外部修改冲突及失败时原文件完整。
- [x] 仓库列表编辑保留未知行；配置保存使用同目录临时文件、备份、同步和原子替换。
- [x] 工作进程只允许修改标准 repositories 和标准 repositories.d 中的普通配置文件。

验证命令：`TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefapk-repositories.sh`。

## 任务 4：FLTK 界面和系统集成

文件：`userland/apps/reliefapk/{main.cpp,window.h,window.cpp,dialogs.cpp,reliefapk.desktop}`、组件/构建/默认配置/APK 归属、图标清单和中文翻译。

- [x] 实现侧栏筛选、虚拟表格、搜索防抖、可拖动分隔栏、按需详情和状态反馈。
- [x] 实现批量操作、可清除的待执行列表、模拟预览、逐项再模拟和错误后停止。
- [x] 实现认证、运行输出、取消、保存日志和运行中关闭保护。
- [x] 实现仓库编辑、缓存、离线、索引生命周期、网络超时、可信密钥浏览与诊断工具。
- [x] 接入菜单、desktop 文件、标准图标、中文国际化与本地 `.apk` 打开入口。

验证命令：`TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug -j4 app-reliefapk nls`，以及组件、metadata、APK 归属和图标测试。

## 任务 5：真实 apk 和系统内验收

文件：`tools/test_reliefapk_qemu.py`、`tools/tests/test_reliefapk_apk.py`、`tools/tests/reliefapk_command_probe.cpp`、`tools/tests/reliefapk_window_probe.cpp`、`tools/tests/reliefapk_catalog_benchmark.cpp`，证据目录 `out/reliefapk-acceptance-*`。

- [x] 在隔离目录制作受信任的测试仓库和两个版本的微型包；实际安装、升级、重装、删除和验证结果。
- [x] 复用既有私有镜像注入及 QMP 工具；普通用户登录、中文界面、sudo 错误/正确密码、root 密码均实际验收。
- [x] 验证仓库保存、离线查询、操作失败及主窗口响应；保存主界面、模拟/进度和配置截图。
- [x] 独立读取 guest 文件及数据库，确认实际副作用；确认源镜像未被改写。
- [x] 进行覆盖全部改动的代码审查并修复有证据的问题；再次运行相关检查。

交付时分别报告编译、测试、系统运行和截图；明确“VMware 验证未完成”。

主机真实 apk 集成：`TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug test-reliefapk-apk`，已验证 32 条生成命令。

最终验收：`out/reliefapk-acceptance-9/result.json`；编译、测试、运行和限制记录于 `out/reliefapk-dev/verification.md`。实现已完成；用户于 2026-10-18 明确要求提交并推送。
