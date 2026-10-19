# Xorg / VMware SVGA II 硬件加速设计

日期：2026-10-18。

用户已批准先前讨论的 DRM/KMS、vmwgfx 兼容接口、Mesa 和上游 Xorg 驱动路线，并要求开始实现。验收目标是 Xorg 实际使用 VMware SVGA II，显示和硬件渲染分别有运行证据。不能把 fbdev 软件显示、驱动名称或扩展初始化日志当作硬件加速成功。

## 接口与兼容性

ReliefNT 保留自身内核实现，对用户态提供 Linux DRM UAPI、对应 vmwgfx 设备 UAPI，以及原有 POSIX/Linux 进程、文件、映射、事件和权限语义。采用上游定义的 ioctl 编码、结构布局、句柄生命周期和 errno。能力查询只报告实际支持的功能；不伪造 DRM 版本、DRI 能力、fence 完成或 GPU renderer。

显示节点 `/dev/dri/card0` 承载 DRM/KMS 显示控制。渲染节点只有在实际支持其权限和隔离语义时启用。每个独立 open 拥有资源与事件状态，dup/fork 共享同一个打开文件描述；关闭最后一个描述和进程退出正确清理，映射或设备仍引用的存储不得提前释放。

KMS 负责显示资源枚举、缓冲区、帧缓冲、模式和显示提交事件。GPU 驱动负责上下文、surface、guest memory、命令验证和 fence；内核不得把任意用户命令直接提交给设备。命令只可引用调用者有权访问的资源。不同文件的私有句柄不得被未授权地使用。Linux legacy surface 的全局名称按照上游授权域共享：同一创建者 DRM master 下已认证的 primary 文件可通过 REF_SURFACE 取得引用，其他 master 或 render 文件需要已有引用或显式 PRIME 文件描述符授权。

用户态复用锁定版本的 libdrm、Mesa Gallium SVGA 和上游 Xorg modesetting/glamor。实际源码核对确认 Mesa 26.1.6 已移除 XA，而 VMware DDX 的加速路径依赖 XA，因此不选择该组合。首个渲染验收使用未修改 Mesa 的 host-backed / VGPU9 路径与 GL 2.1 级别绘制；设备底层在需要时使用真实 GB 兼容转换。DRI、硬件版本、着色器能力和 OpenGL 版本按真实支持范围协商，不以最高 OpenGL 版本为首轮目标。

现有专用 `/dev/gpu` ABI 可继续服务原有程序，但不被当作 Linux DRM。已有 SVGA FIFO、GMR/MOB、资源探测和 fence 代码只在语义一致时复用。优先减少不必要的 GPU 回读；共享缓冲区和显示提交必须遵守同步与资源生存期规则。

## 边界与错误处理

- 未实现的 ioctl/能力按上游契约拒绝；不得返回虚假的成功。
- 所有长度、偏移、整数运算和用户地址检查溢出与可访问性；提交前快照可变输入。
- 普通渲染用户不能取得显示控制权限；DRM master 与 VT 控制权保持一致。
- 显示提交、等待和设备错误有界处理；资源仍被设备引用时保留其 backing memory。
- 原有 fbdev/文本 VT 作为无兼容硬件或无法建立加速时的恢复路径，不冒充加速路径。
- 仅修改本任务相关模块；既有 audio、ABI golden 等事项不纳入本任务。

## 验收

1. 主机运行真实内核模块的 ABI、权限、资源生命周期、映射、命令验证和失败回归；比较上游 UAPI 布局与编码。
2. 构建目标内核、用户态库与镜像，检查缺失符号和依赖。
3. QEMU VMware SVGA 设备验证 KMS 显示、无 3D 能力时的明确结果、VT/Xorg 生命周期和软件恢复；QEMU 的二维 SVGA 模型不作为 VMware 3D 加速证据。
4. 原生 VMware 开启 3D，记录设备能力、Xorg 选用驱动与加速后端、标准 Mesa `glxinfo` renderer、实际绘制结果及截图。执行普通用户、多应用、缩放、窗口覆盖、退出和 VT 切换。
5. 关闭 VMware 3D 后验证能力回退，结果与开启时分别记录。

交付报告将编译、测试和系统运行分开。未经真实运行，持续标注“VMware 验证未完成”。本任务不自动提交、推送或建分支；测试临时文件使用项目 `out/tmp`，所有构建产物留在 `out/` 或 `build/`。

## 上游依据

- Linux DRM UAPI：https://docs.kernel.org/gpu/drm-uapi.html
- Linux KMS：https://docs.kernel.org/gpu/drm-kms.html
- Mesa VMware SVGA：https://docs.mesa3d.org/drivers/svga3d.html
- Xorg VMware DDX：https://www.x.org/releases/current/doc/man/man4/vmware.4.xhtml
