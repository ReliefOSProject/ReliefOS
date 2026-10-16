# 安装器可选 Xorg 桌面

**目标：** 全新安装可选择是否安装 Xorg 桌面，默认安装。

**设计：** Motif 确认页的复选框与 TTY 的 Y/n 提问共同写入
`installer_setup.skip_xorg`。零初始化保持默认安装。安装介质包含分别经
upstream APK 事务生成的 `/install/root` 和 `/install/root-console`；复制所选
root，不在复制后伪造或修改 APK 数据库。Live 安装界面保持 Xorg。

更新模式不提供卸载选项，按目标 `desktop-backend` 选择相同 profile 的更新仓库。
命令行 profile 通过 getty 登录。格式化之前检查所选安装源及 APK 数据库、索引、
backend marker；缺失命令行安装源时停止。

**约束：** 使用当前分支，仅在用户明确要求后提交、推送；不修改内核 ABI、audio 或 ABI golden。
临时文件放 `out/tmp`；不改动用户已有未跟踪文件。子代理使用 GPT-6-Luna Max。

## 实施与验证

- [x] 前端：默认勾选、返回后保留选择、更新模式不出现该选项；TTY 空输入为是，
  n/no 为否，其他输入重试，EOF 取消。真实 TTY 输入单测先红后绿。
- [x] 安装源：console marker 筛除 feature:xorg 的上游包及其 raw 残留，再移除
  X11 产品配置和应用；真实 ELF 依赖检查、签名和 APK add 创建每个 profile。
  shell fixture 验证 console 包不回流到本地包，live 和默认目标仍为 Xorg。
- [x] 复制：所选 root 同时用于扫描和复制；账户配置不为 console 生成桌面快捷方式。
  宿主 C/Python 回归覆盖默认、缺失 console 安装源、错误 marker、缺失 APK DB。
- [x] 更新：console 目标选择 console 仓库，默认桌面和旧目标保持原更新源。
- [x] 启动：真实 console-session 在 chroot sandbox 验证 console tty1 直接 getty，
  无图形进程或 sentinel；xorg、非法 marker 和其余 VT 回归。
- [x] 编译新 installer 和 ISO，检查两份实际签名 APK DB 与仓库内容。
- [x] QEMU：检查默认勾选、切换及回退保留；在独占空白磁盘执行 console 安装，
  重启验证 root/普通用户登录及不含 Xorg/IceWM，保留日志与截图。

验证报告将区分构建、宿主测试、QEMU 运行；VMware 验证未完成。

## 验证结果

- `make -j8 O=out/xorg-vmware-debug PROFILE=debug installer`：exit 0。
- 宿主回归 50 项通过；APK 筛选、installer stage、原 Xorg stage 回归通过。
- QEMU SVGA：默认选择、取消、返回保留、命令行实装、root/alice 登录及权限和已安装包清单检查：exit 0。
- 证据：`build/installer-xorg/install-run.log`、该目录下 install/installer.log 和 root/user 的 session-evidence.txt。
- 通用 VGA 24bpp 测试遇到显示条纹；未改动显卡驱动。VMware 验证未完成。
- 上述 ISO 构建和 QEMU 验收使用内核 `0454b47`。提交前同步远端 `3cb23b4`
  （内核日志更新至 `50f9f77`），并重新执行宿主回归；未对该新内核重跑完整安装。
