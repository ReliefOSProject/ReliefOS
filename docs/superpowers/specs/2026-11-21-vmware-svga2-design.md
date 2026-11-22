# ReliefOS / ReliefNT：VMware SVGA II 驱动设计

**状态：** 路线 A 已批准，实施进行中。  
**日期：** 2026-11-21。

## 1. 目标与完成条件

在 x86-64 ReliefNT 上为 VMware SVGA II（PCI `15ad:0405`）新建 DRM/KMS 后端，以 ReliefOS 中未经修改的 libdrm 和 Mesa Gallium `svga` 为用户态验收对象。Linux 主线 `vmwgfx` 是设备操作、生命周期与用户 ABI 的行为参考，不引入 Linux DRM、TTM 或调度器作为内核依赖。只使用当前工作树保留的 ReliefOS / ReliefNT 接口和第 8 节的公开资料；不查阅或使用两个项目已删除的实现、专属接线及其历史、备份和构建产物。构建与虚机验收使用从当前树新生成的产物。

「完整」不等于每台虚机都有相同的硬件能力，也不等于能执行协议头中定义的每个编号。目标是覆盖 SVGA II 各代适用的 2D / 3D 功能、保留的 `DRM_VMW_*` ABI，以及 Linux `vmwgfx` 向用户开放的对应命令和 KMS 行为；内核专用、已废弃、保留、仅适用于其他 PCI 设备的命令不能因此开放给用户态。维护一份覆盖表，以命令或 ioctl 的编号（含别名、版本）、适用能力、用户态权限、实现入口、正/负例与环境为索引。每个条目都要有对应的实现与测试，或有规范 / 参考证据说明为何不适用。宿主未提供、驱动尚未实现、没有环境验证三种状态分别记录；后两种不能用 `-EOPNOTSUPP` 掩盖后算作完成。

| 能力族 | 实现目标与验收边界 |
| --- | --- |
| PCI 与传输 | BAR0 索引 / 数据端口、BAR1 VRAM、BAR2 FIFO；版本与寄存器、FIFO、3D devcap 协商；基本与扩展 FIFO、DMA command buffers、fence、IRQ、失效与可证明的恢复。 |
| 显存与客体内存 | VRAM 与系统内存 BO、GMR/GMR2、MOB 和页表、对象表；可回收驻留、读写同步、设备地址校验、共享和映射。GMR1 单独以设备协议为准，不能把 Linux 参考中的 GMR2-only 代码当作 GMR1 实现。 |
| 2D 与 KMS | Legacy display unit、Screen Object / Screen Object 2、Screen Target；相应模式、输出布局与变更通知、plane、legacy/atomic 提交、damage、翻页、vblank、读回、硬件光标及热点、overlay/stream；按设备能力选择直接扫描输出或回退。包括适用的 2D 更新和加速命令，不给用户态原始 FIFO 入口。 |
| Legacy SVGA3D | Host-backed context / surface / shader、格式和 mip/face 布局，上传下载、绘制、状态、查询和资源销毁，验证 Linux 兼容的 execbuf 流。 |
| Guest-backed SVGA3D | GB context / surface / shader、MOB 绑定和读回、扩展 surface ABI、对象换入换出与绑定恢复；旧用户态的 devcap 格式和兼容路径。 |
| DX/vGPU10 | DX context、COTable（上下文对象表）、shader、view、state、query、stream output、GPU copy；按能力覆盖 SM4、SM4.1、SM5、GL4.3 对应的图形和计算命令。 |
| DRM 与 ReliefOS | `vmwgfx` 设备识别、card/render 节点、主控与认证、`DRM_VMW_*`、GEM/dumb BO、mmap、PRIME、sync_file、事件；另处理 VMware 专有 `MSG`/`MKSSTAT_*` 的门控及生命周期，不能用这些 ioctl 的成功代替 3D 验证。 |

不包含 SVGA3（PCI `15ad:0406`）、物理 GPU 直通、Windows WDDM 客户端，也不承诺超出协议和所选 Mesa 驱动的 Vulkan 或 OpenGL 版本。SVGA II 设备上支持的 **SVGA3D** 是命令协议，与 **SVGA3** PCI 设备不是一回事。保持现有单主设备模型，设备可有多个显示输出，不扩展 `card1`/`renderD129`、跨 GPU 调度或新通用 DRM UAPI。Surface PRIME 与通用 GEM DMA 导入是不同能力，按第 5 节的共享契约分别实现和验证。

## 2. 后端边界与项目接入

路线 A 复用 Linux `vmwgfx` 的用户态接口，不另设一套 ioctl 或修改 Mesa winsys。这样需要实现上游程序实际依赖的版本、资源和同步行为，但用户态不必维护 ReliefOS 专用分支。内核侧不直接移植 Linux DRM/TTM：以它们的可观察行为为参考，在 ReliefNT 的设备、内存和文件所有权接口上实现，避免更换现有后端的基础设施。

当前 DRM 的设备是单例，BO、文件状态、KMS、ioctl 与退役路径含有 VirtIO 专用代码。保留现有 graphics service 和模块所有权边界，仅把接入所需的分派改为基于**持有的设备**而非全局 PCI ID。`card0` 和 `renderD128` 仍绑定同一个已发布设备；文件打开后固定绑定，不能在故障或复位时切换到另一个后端。同步、PRIME、内存映射等只提取两者确实共享的契约，不重写 VirtIO 提交与 panic 路径。

| 层与归属（相对于 ReliefNT 根目录） | 职责 |
| --- | --- |
| `drivers/video/drm/` | 管理设备身份和发布、文件与 master、GEM/PRIME 的共有入口、ioctl 外层 usercopy/权限、事件与 mmap 的通用生命周期；按生命周期、ioctl、KMS、BO 四组内部操作分派。设备驱动描述记录稳定身份、ioctl 表和功能查询；不向 UAPI 暴露操作表。 |
| `drivers/video/drm/virtio/` | 将现有行为接入操作表；保持 `virtio_gpu` 身份、既有 ioctl、VirGL、控制台交接与 panic 输出。冻结对外行为，避免改变 VirtIO 的传输与资源布局。 |
| `drivers/video/drm/svga/` 的 `device`、`fifo`、`cmdbuf`、`fence` | 拥有 BAR/端口、capability 快照、提交队列、IRQ 与完成序列；设备传输层只接收内核构造的字节及拥有引用的提交对象，不解析用户指针。 |
| 同目录的 `bo`、`gmr`、`mob`、`resource`、`binding` | 管理 BO 驻留、GMR/MOB、surface/context/shader/query、COTable、对象 ID、绑定依赖和事务；KMS 和渲染都经由资源层，不直接持有用户句柄。 |
| 同目录的 `ioctl`、`execbuf`、命令族验证器、`kms`、`cursor`、`overlay` | 将用户 ABI 转成上述对象和事务。命令描述表与验证逻辑按 legacy / GB / DX 拆分；显示后端按 legacy / Screen Object / Screen Target 分离。宿主消息和统计另设模块，不能取得提交通道的管理权限。 |

只在需要时给通用 BO 增加后端私有数据和最终释放回调；GEM 句柄、映射授权和 `/dev/dri` 节点语义保留。私有 ioctl 号在各 DRM 驱动间重叠，外层必须先绑定文件设备、再按该驱动的描述表匹配，不能按号试跑两个实现。设备热路径仅在 KMS、提交、回收等阶段入口调用后端操作；每个 FIFO 字和每条命令不经由全局后端分派。

新 Kconfig 项为 `DRM_VMWARE_SVGA`，随 `DRM=y/m` 链入同一 DRM provider，可单独关闭，不依赖 VirtIO。ReliefNT 的 Kconfig 和 x86-64 默认配置均启用该后端，ReliefOS 默认构建继承该配置；选项仍标记为实验性，剩余验收见第 7 节。两者都编入时，默认维持现有 VirtIO 优先；没有 VirtIO 时探测 SVGA II，缺卡走当前永久 framebuffer/控制台。只有启用该后端且匹配 PCI 功能才触碰其寄存器。探测返回「不存在 / 不适配」与「找到但失败」要区分，失败按回滚结果决定是否安全保留早期显示，不发布半初始化节点。不新增运行时切换 ABI。ReliefOS 只增补必要的 Kconfig、设备发现和用户态打包接入；现有 VirGL 构建、运行和测试配置不被替换。若安装的用户态缺少 `svga`/libdrm VMware 支持，纳入现有打包流程，以未修改的上游组件验收，不用私有 winsys 弥补内核 ABI 缺口。

## 3. 设备、提交与同步契约

**探测与发布。** 核对 PCI ID、BAR 类型和完整跨度（包括寄存器/IRQ 端口的访问宽度），保存更改前的 PCI 命令字和显示交接状态，对设备写入版本后读回确认。BAR0 不是寄存器 MMIO；索引和数据端口成对串行化，IRQ 状态访问遵从 SVGA II 的独立端口协议。BAR1 和 BAR2 的设备报告范围只能缩小 BAR 提供的范围，不能扩大；尺寸、偏移、对齐、FIFO 游标和扩展字段的存在性均须检查。先关闭设备 IRQ 源，再建立资源、FIFO、完成和显示路径；只在能接收请求并回滚时发布节点。留意固件 framebuffer 交接与缓存类型，不创建同一物理页的 UC/WC 冲突别名，也不擅自更改既有永久映射。

**能力。** 保存硬件探测值和可公开的有效能力两份快照。公开的能力必须同时满足寄存器 / FIFO / `DEV_CAP` 条件和驱动资源、传输及验证器条件。在兼容格式中同时屏蔽能触发尚不可用路径的位，避免某个 `GET_PARAM` 回答关闭但另一查询仍使 Mesa 启用该路径。`SVGA_ID_2` 不蕴含 3D；legacy 路径按 3D 位、FIFO 版本与可用资源选择，GB 需要可用的 MOB/DMA 管理。DX 还需要 `SVGA_CAP_DX` 和 `DXCONTEXT` devcap；SM4.1 需要 DX2 和 SM41，SM5 再需要 DX3 和 SM5，GL4.3 对应能力在此基础上还需 GL43。仅用 ID、宿主版本字符串或者软件 GL 渲染器都不能作为能力证据。

**FIFO 与 command buffers。** 以 Linux `vmwgfx_cmd.c` 的 reservation/commit 顺序为参考。先确认 `MIN/MAX/NEXT_CMD/STOP` 对齐和区间不变量，保留一字用于区分空和满；遇到非法宿主游标不继续读写。支持 `RESERVE` 时连续区间内原地编码可信内核命令，回绕最多两段复制，按屏障、`NEXT_CMD`、清 reservation、通知宿主的顺序发布；不支持 `RESERVE` 时遵守旧 FIFO 的逐字发布规则。不能把普通 RAM 的 `memcpy` 或 WB 假设用于 UC 设备窗口。一个提交事务的全部用户数据必须先验证，校验成功不表示宿主原子执行整批。DMA command buffer 用容量有界的常驻池，遵守设备声明的 512 KiB / 4 MiB 容量及命令和队列上限；headers、命令、完成状态遵循协议的 DMA 可见性与访问顺序。只在资源和命令族允许时回退 FIFO；跨通道依赖必须显式排序，不能把两条物理队列的 fence 当作天然同序。

**完成与等待。** `fence` 是以设备完成为依据、可持有的对象；硬件序号按允许的未完成窗口做回绕比较，软件区分设备 generation。优先使用可用的 MSI/MSI-X；有可用的 SVGA IRQ 源但只有 INTx 时，沿用安全的 PCI 路由能力，或在实现阶段增加足够小、可独立验证的共享 INTx 注册/清理支持。现有 ReliefNT `pci_irq.h` 没有通用 INTx 注册，不能假定 PCI 配置中的 interrupt-line 字节就是可用路由。合法 IRQ 都不可用时使用有截止时间的轮询/定时退役路径；在无 fence 的传统设备上，靠协议确认的 `SYNC/BUSY` 及 FIFO 进度，不能靠提交即完成。IRQ 只确认来源、应答与记录完成/错误，唤醒或调度有预算的退役；不运行 usercopy、分配、销毁和等待。失去 IRQ 仍有定时进度检查；等待采用采样、登记、复查，避免丢唤醒。必须区分用户等待到期（如 `VMW_FENCE_WAIT` 的 `-EBUSY`）、信号中断、可重试队列满和独立的设备进度 watchdog 故障。

**生命周期。** 探测、运行、静默、复位、终止是显式状态。失败时按获取的相反顺序撤销未公开资源；已公开的对象走退役。关闭或复位先关新提交，再屏蔽 IRQ 源和排空 ISR/工作队列，在限时内确认设备不再访问旧 DMA／MOB。IRQ drain、FIFO `STOP` 前进、清 PCI bus master 都不能单独当作 MOB 安全释放证明。只使用已确认适用的设备停止/复位过程；不提供凭空写寄存器的「硬复位」。出错的旧 generation fence 终止并唤醒等待者，旧 context/绑定不静默重放；经过完整重新探测才允许新对象，新旧 ID/回调/完成状态不得混淆。若无法证明静默，隔离有配额的设备可访问 backing、停止新硬件请求和节点开放，且保留已持有的 provider pin 以阻止不安全的卸载；不能通过重置计数器、强制卸载或重复恢复来释放这批资源。无法确认设备不再访问时，不把同一 aperture 交给其他后端；永久 framebuffer 若可安全保留则继续使用。

**致命故障后的旧会话。** 即使设备成功重新探测，新一代只服务重新打开的 fd。旧 DRM fd 不可新建对象、重新取得 master 或提交硬件工作，但仍可释放对象、读取已排队的事件和查询旧 fence；已完成结果不改写，未完成工作按各 ABI 的错误路径终止并唤醒等待者。旧代 PRIME 对象不得导入新代执行。仍可安全访问的 CPU 映射随 VMA 引用保留，不因 fd 关闭或代际切换而指向新对象。如无法证明新一代避开仍可访问的旧 VRAM 映射，也不能按项目 VM 契约安全撤销这些映射，则延迟重新发布。有协议保证的正常挂起/恢复与致命故障分开，不因正常挂起给旧会话换代。

## 4. 资源所有权与有界验证

- **BO 与地址。** 文件句柄、VMA、PRIME 文件描述、扫描输出和在途提交各持有独立引用。VRAM 有范围分配器，系统 BO 可由页向量支撑，连续 DMA allocation 仅用于协议实际要求连续的对象。物理页、设备可达地址、GMR ID、MOB ID、用户句柄是五类不同的数据。根据地址编码、DMA mask、页数和可用 backing 选择 GMR1、GMR2 PPN32/PPN64、MOB 页表深度；设备未支持的地址空间不截断使用。VMA 仍在、尚未解绑或 fence 尚未退役的物理页不迁移、不复用；对象的设备绑定可以在有序退役后单独重建，不必搬移其 CPU 映射。淘汰前先进行必要的写回与解除绑定。内存、ID、在途队列及延迟释放分别计入设备/文件限额，固定预算与硬件上限取较小值；预算要覆盖合法的最大单次提交，防止一个文件占满设备而无法退役。
- **验证与可见性。** 用户字节和嵌套数组先导入 kernel-owned 缓冲，所有长度、对齐、乘加、数量和地址都做溢出/上限检查。解码表记录命令 ID、最小/精确大小、尾部数组规则、适用能力、可调用来源及对象访问类型；不透传未知、保留、内核管理命令。检查对象类型、文件归属、显式共享权限、子资源范围、shader/query/copy/indirect 参数；原始 GMR/MOB/物理地址和对象 ID 不可信，只从已引用且范围有效的对象生成。用事务内去重索引收集引用与 relocations，不边解析边修改活动绑定，也不做全局线性逐对象查询。
- **提交点。** 先解析，后按稳定的 BO 顺序预留和检查依赖，准备搬迁、绑定、重定位、fence 与 fd/事件资源；排定传输空间并再次核对设备 generation。提交前失败取消软件预留而不发布用户命令；已经发出的管理命令交给有界退役/撤销流程，已发布的用户命令不能靠回滚软件结构撤消。第一笔对宿主可见的发布之后，引用归完成路径；迟发的 copyout 故障回收未交付的句柄/fd，按 ioctl 契约返回错误，不释放仍在使用的页，也不重复发出命令。
- **并发约束。** 保持 ReliefNT execution 所有权规则，不假定全局 execution 锁能保护 IRQ 共享字段。需要持有多个提交/资源锁时，以 device submission → 按稳定对象顺序的 BO reservation → context binding → 短期 transport/索引寄存器自旋锁为单向次序；完成路径只更新自己的短临界区，复杂退役和最后引用释放推迟到任务上下文。GPU 等待不持有全局 service registry 锁或自旋锁，不在 usercopy 争用重试或对象最终释放回调中进行；设备停止允许在明确的任务上下文有界等待。执行需要睡眠或掉锁时保留明确的对象/provider pin，并重新确认 generation/条件，不保存借用的用户指针、任务指针或对象表指针。`CONFIG_SWAP` 专用重试标记仅在不可回滚发布之前返回给 syscall 外层。

## 5. 用户 ABI 与显示行为

- **身份、查询和命令布局。** 使用 `DRM_IOCTL_VERSION` 的 `vmwgfx` 名称并提供 libdrm 可发现的 PCI、节点与 sysfs ABI；版本由已验证的 ABI/行为决定而非硬编码 Linux 模块版本。基准 Mesa 用版本判断 GB、execbuf v2、共享、SM 及 GL4.3 路径。布局冻结以保留的 `vmwgfx_drm.h` 为准；按 ioctl 的方向、用户声明大小、Linux 的短参数补零规则和各版本有效字段处理，限制缓冲上限。支持 execbuf v1 的短布局和 v2 的 context/显式 fence 字段；分别验证非法版本及 flags。每个 ioctl 核对权限和错误语义；特别核对 `PRESENT/READBACK` 的 master + auth、`UPDATE_LAYOUT` 的 master / 管理权限，及 render-node 许可表，不把版本、大小和 errno 政策套到其他后端。
- **兼容能力与同步。** `GET_PARAM` 中的 `MAX_MOB_MEMORY` 具有 per-file `gb_aware` 效果；`GET_3D_CAP` 分别输出旧 FIFO records、GB 设备给旧用户态的兼容记录和 GB-aware devcap 数组，不把它们统一成一个结构。实现 fence reply、等待、查询、解绑、事件和 execbuf 的 sync_file 导入/导出及 `SYNCCPU`；按现有 DRM `sync_file` 文件契约提取共用完成对象，不依赖编入 VirtIO；可导入其它合规生产者或合并后的 fence。仅在能证明同一队列顺序等价时消解依赖，否则按 ABI 等待，不能持提交锁跨睡眠。上游不提供的额外通用 DRM syncobj 能力不因支持 sync_file 而自动开放。
- **PRIME。** 区分 VMware surface 共享和通用 GEM BO。同一设备与代际内，跨文件/进程导入引用同一对象，保持 `DRM_CLOEXEC`/`FD_CLOEXEC`、`DRM_RDWR`、重复导入与共享授权语义。参考 Linux `gem_prime_import_sg_table`，外部 GEM backing 只有能提供稳定页面、正确 DMA 映射和共享同步/释放契约时才经由窄的内核接口导入；不得用一次拷贝假装共享，也不把专用 surface fd 当通用 DMA backing。外部生产者的契约用测试对象验证，不为验收引入第二个活动 DRM 设备；缺少真实生产者时如实记录外部互操作未实测。
- **POSIX 和 Linux DRM 的分界。** `open`/`dup`/`close` 共享并正确释放 open file description；仍有效的映射不因关闭原 fd 或删除 BO handle 失效。`read` 仅交付完整 DRM 事件，按 Linux DRM 保留放不下或复制失败的首个待交付事件；已经成功复制的事件返回其累计字节数，首次复制失败返回 `EFAULT`。设备就绪时 `poll` 报告相应 `revents`，不受 `O_NONBLOCK` 影响，空队列的非阻塞读取返回 `EAGAIN`，阻塞等待可被信号中断。VMA 保留访问权限、缓存类型及页边界约束，拒绝把 BAR 控制区、未授权 BO 或 kernel-only backing 映射给用户。POSIX 不定义 DRM ioctl 编码、fence flags 或设备断开语义；这些由当前项目 ABI、Linux DRM 和 `vmwgfx` 行为约定确定。显式的设备失败唤醒使用者，不伪造成功的 flip 或 fence。
- **KMS。** 参照 Linux 顺序选择 Screen Target → Screen Object → legacy；限制多输出总像素和驻留预算、格式/stride、damage 与光标尺寸。校验整份 KMS 请求后才提交（`TEST_ONLY` 无副作用），保持已显示 FB 的独立引用，按完成语义产生翻页/vblank 事件，区分设备完成与基于 ReliefNT 时钟的合成 vblank。Overlay/stream 按 video/escape 能力和 Linux 限额注册，逻辑关闭必须停用相应设备对象并等退役。调节 VT、旧 framebuffer 和新扫描输出的所有权；panic 路径只允许预分配且已验证为无锁、无等待依赖的输出，不挂上普通 SVGA 提交入口。
- **宿主消息与统计。** `MSG`/`MKSSTAT_*` 纳入编号与行为覆盖，但独立于 3D。只在确认 VMware 宿主及适用通道时启用设备操作，分配/注销与文件或进程所有权对应；限制输入、宿主回复、页数和 lifetime。`MSG` 的 `receive_len` 是输出，不擅自重释为输入容量；对宿主回复应用协议上限和可写范围检查，不作截断成功，不假装能从地址推知调用方缓冲对象大小。统计页只在宿主确认注销或有等价静默证明后解 pin。

## 6. 性能策略与判定方法

先保证真实 GPU 进度和正确同步，再用测量选择优化。Probe 时确定传输与显示路径；常见 draw/dispatch 批次复用命令缓冲及临时引用表，直接从经过验证、归内核所有的 DMA 缓冲提交，FIFO 只进行必要复制。按批提交、通知与合并退役，缓存已确认的资源绑定；IRQ 通常按需要订阅完成目标而不是逐条命令中断。无需 CPU 等待的同队列工作靠顺序和 fence 保证；慢路径才分配、映射、回收、轮询或执行数据搬移。可扫描的已驻留 surface/BO 优先直接绑定，只有格式、位置或能力不匹配时才拷贝；desktop damage 以验证过的矩形或范围限制字节数，不能无条件整帧上传。

用三个可重复负载：小 draw/dispatch 批次（含并发 context）、持续大纹理 upload/copy/readback、双缓冲小区更新加全帧 page flip。对每组保存热身后相同数量的命令、输出像素/缓冲校验值、分辨率、宿主版本/后端、VM vCPU/RAM、PCI ID、设备能力、guest/Mesa 构建标识。报告 CPU 时间、提交到 fence 的 p50/p95/p99、吞吐、队列等待、每批 doorbell/IRQ 数、复制字节和退役失败数；优化前后跑同宿主、同配置至少 3 次，记录离散度，不用 `glxgears` FPS 代替。Linux `vmwgfx` 客户机用作同宿主的对照，不把跨内核的吞吐差直接归因于某项优化。没有同配置测量就只报告实现的快路径，不声称性能提升；关闭 SVGA 时仍核对现有 VirtIO 性能与行为未出现可重复回退。

## 7. 分阶段验收与复现入口

每道验收门都必须有当前源代码、新产物、命令行、关键输出和通过/失败/未覆盖状态。阶段可以独立交付证据，不能把阶段完成标作整套驱动完成。覆盖表按命令 / ioctl 编号跟踪实现和有界验证测试；使用少量参数化测试覆盖边界及拒绝路径，用组合负载验证每个能力族的真机成功路径。真机记录列出实际经过的命令 / ioctl 和输出，不要求每个编号一个独立虚机用例，但不能把同族的通过结果记成未经过条目的实测通过。

| 阶段 | 最少可重复验收 |
| --- | --- |
| 1. DRM 边界与 2D | DRM `y/m/n` 与 VirtIO-only、SVGA-only、双编入的构建矩阵；现有 DRM 文件/模块、firmware framebuffer/VT/panic、QEMU VirtIO 2D 与 VirGL 回归；QEMU `-vga vmware` 测 PCI、基础 FIFO/2D 更新、mmap/damage、无 3D 不开放 render、无 SVGA 仍可启动。 |
| 2. 内存、同步和故障 | 后端寄存器/FIFO/IRQ 的小型确定性测试模型驱动实际验证代码：FIFO 回绕、无 `RESERVE`、非法游标、满环/丢 IRQ、序号回绕、部分初始化每步回滚、OOM、晚到完成和 reset 代际；同一 BO 经 close、munmap、PRIME、fence 与异步销毁的交错测试；用外部 backing / fence 测试对象覆盖导入和故障拒绝。模拟成功重新探测后，旧 fd 提交须失败、仍受保护的 VMA 不得看到新对象。模型结果不能替代宿主验证。 |
| 3. Legacy 与 GB 3D | 在实际 `15ad:0405` VMware 客户机上，验证 legacy host-backed 和 GB 两条路径；使用具备相应能力的宿主/虚拟硬件配置，分别断言选中的协议、带 fence 的渲染结果、纹理和 query/readback 值。用同一组简短 ioctl 测试覆盖非法包长、越界或未授权的对象、版本、权限及输出故障后不重提。 |
| 4. DX 与整机 | VMware 上验证 SM4/SM4.1/SM5/GL4.3 分别可达和正确门控。未修改的 Mesa 通过 `vmwgfx` render node，断言无 `llvmpipe`/软件回退并检查纹理、深度/MSAA、draw/indirect、query、compute/UAV 输出；测试 PRIME/显式同步、KMS 多输出/翻页/光标/stream 和关闭/复位后不再访问旧对象。使用正确 PCI ID 和 host caps 的实测才可登记对应完成。客户机选择 SVGA3D 不能单独证明宿主使用物理 GPU；同时记录 VMware 宿主渲染后端和所用 GPU，不能排除宿主软件回退时将物理 GPU 加速标为未验证。 |
| 5. 性能与发布 | 按第 6 节运行同配置对照；核对 Linux/Mesa ABI 能力门控和对应的测试证据，记录硬件不具备或尚未实测的项目。没有可验证的 legacy 或 GB/DX 宿主时保持对应条目未完成/未验证，不用另一个设备或 QEMU 结果替代。 |

当前树可立即运行的布局基线（从 ReliefOS 根目录执行，均只读当前源文件）：

```sh
python3 -B kernel/reliefnt/tools/test_abi_layout.py
cc -std=c11 -Wall -Wextra -Werror -fsyntax-only \
    -I kernel/reliefnt/include/uapi -I kernel/reliefnt/include \
    kernel/reliefnt/tools/tests/drm_abi_test.c
cmp kernel/reliefnt/include/uapi/drm/vmwgfx_drm.h linux/include/uapi/drm/vmwgfx_drm.h
```

**截至 2026-11-21 的验收记录。** 下面的日志路径相对于 `out/svga-verify/`，均来自新实现。不同条目可能对应不同内核构建，不能合并为同一产物的全量通过。复现入口见[实施计划](../plans/2026-11-21-vmware-svga2.md)。

| 路径 | 结果与证据 | 未证明 |
| --- | --- | --- |
| ReliefNT 宿主模型 | `tools/test_drm.py` 的 ASan/UBSan 套件通过，包括 FIFO、命令缓冲、DMA 驻留、KMS、MKSSTAT 和现有 DRM/VirtIO 回归。`EXECBUF` 发布后回写失败用例确认命令不重提，完成后释放 BO，超时仅保留设备仍可能引用的资源。STOP 返回 `QUEUE_FULL` 的用例确认可重试，未确认停机前保留命令页。 | 不等于 VMware 上重现超时、成功恢复或完整模块卸载矩阵。 |
| VMware Legacy | Workstation 26.0.1，虚拟硬件 9，`15ad:0405`，`screen-object / cb / poll`。Mesa 26.3.0 OpenGL ES 2.0 的三角形、纹理和查询回读、execbuf v1/v2、`sync_file` 及双屏原子交换通过。`stop.2m46tx/legacy-musl/serial.log`。 | 双屏绑定与两个翻页事件不等于逐屏像素正确。文本控制台就绪标记已到达串口，但 host 截图工具需要来宾登录，未取得像素证据。 |
| VMware GB | 虚拟硬件 20，Mesa `SVGA_VGPU10=0`，使用 GB 路径；渲染、回读和 execbuf 通过。`matrix-OMrGrk/vmware-gb/serial.log`。 | 设备仍公布 DX 能力；这不是无 DX 的物理配置，不代表在所有 GB 代际通过。 |
| VMware DX | 虚拟硬件 20，`15ad:0405`，`screen-target / cb / poll`。Mesa 26.3.0 OpenGL ES 3.1 的纹理、深度、MSAA resolve、间接绘制、查询、计算回读、显式同步及双屏 KMS 通过。`exec.491LAY/dx/serial.log`；同目录 `mksSandbox.log` 确认宿主 Vulkan 选用 Radeon RX 6700 XT。 | 未逐级验证 SM4/SM4.1/SM5 或桌面 OpenGL 4.3。这台 VM 未验证 SVGA IRQ。 |
| QEMU 对照 | `q2-1795258568/serial.log` 的 SVGA II legacy FIFO 2D；`qg-1795258568/serial.log` 的 VirtIO VirGL。两者完成标记均为 `status=0`。 | QEMU 的 SVGA II 仿真无真实 3D/fence 完成，不用于验收 VMware 3D 或 IRQ。 |
| 性能 | `matrix-OMrGrk/vmware-dx-dual-1795257040/serial.log` 和 `matrix-OMrGrk/linux-dx-sync-1795256868/serial.log`：100,000 次 64×64 离屏 draw，ReliefNT 29.242 ms，Linux 24.354 ms。 | 仅单样本，不支持性能优劣结论或第 6 节的分位数判定。 |

**故障边界。** `vmw_cmdbuf_ctx_submit()` 把 `SVGA_CB_STATUS_QUEUE_FULL` 视为提交未入队；`vmw_cmdbuf_startstop()` 单独走设备上下文。ReliefNT 只对已排空的数据上下文重试被明确拒绝的 STOP，不重新开放普通提交，不把 `BUSY`、IRQ drain 或关闭 PCI bus master 当作旧 DMA 静默的证明。状态未知时仍保留命令页及未确认退役的 backing；未实现故障后安全复位与旧文件代际隔离。

仍待验收：逐命令/ioctl 编号证据、实际 IRQ 与 GMR1、各 SM 档位、外部 PRIME 互操作、overlay、VMware `MSG`/`MKSSTAT_*` 正向路径、控制台与双屏像素、故障复位以及同配置性能分位数。已有命令入口或宿主测试不能代替相应的设备证据。

## 8. 资料与许可证

| 权威来源 | 本设计用于确认的行为 |
| --- | --- |
| 用户指定的 `linux/` 当前源码（`Makefile` 标识 `7.4.0-rc3`）；[主线 vmwgfx](https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/vmwgfx) | `vmwgfx_drv.c`：PCI/BAR、能力探测；`vmwgfx_cmd.c` / `vmwgfx_cmdbuf.c` / `vmwgfx_irq.c` / `vmwgfx_fence.c`：传输、等待和完成；`vmwgfx_gmr.c` / `vmwgfx_mob.c` / `vmwgfx_resource.c`：驻留；`vmwgfx_ioctl.c` / `vmwgfx_devcaps.c` / `vmwgfx_execbuf.c`：ABI 和验证；`vmwgfx_prime.c` / `vmwgfx_gem.c`：surface 与 GEM backing 的不同导入路径；`vmwgfx_kms.c`、`vmwgfx_ldu.c`、`vmwgfx_scrn.c`、`vmwgfx_stdu.c`、`vmwgfx_overlay.c`：显示路径。外部主线链接便于定位；以当前本地参考文件为复核基线。 |
| VMware 公开的 [设备协议头](https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/vmwgfx/device_include)，交叉查阅固定版本的 [SVGA 寄存器与 FIFO 定义](https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/gpu/drm/vmwgfx/device_include/svga_reg.h) | `svga_reg.h` 的 BAR 端口寄存器、位、FIFO 和 GMR；`svga3d_cmd.h`、`svga3d_dx.h`、`svga3d_devcaps.h` 与同目录定义的命令、布局、limits 和能力条件；设备协议与操作系统私有 UAPI 分开。 |
| [Linux DRM Userland interfaces](https://docs.kernel.org/gpu/drm-uapi.html)、[DMA-BUF](https://docs.kernel.org/driver-api/dma-buf.html)、[Sync File](https://docs.kernel.org/driver-api/sync_file.html)，本地 `vmwgfx_drm.h`、`vmwgfx_drv.c` 和 DRM core | 设备发现、主控与 render-node 规则、用户内存映射、ioctl 形状和错误、PRIME 的 fd 共享和 fence fd 生命周期；Linux 行为作为 ReliefNT 兼容目标，不直接复用 Linux kernel 实体。 |
| [Mesa SVGA3D 文档](https://docs.mesa3d.org/drivers/svga3d.html)、[Mesa 25.3.0 winsys `vmw_screen_ioctl.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-25.3.0/src/gallium/winsys/svga/drm/vmw_screen_ioctl.c) | 固定版本作为兼容基线而不是「最新」版本，核对 `drmGetVersion`、`GET_PARAM`、`GET_3D_CAP` 顺序，execbuf v1/v2、GB surface、PRIME、sync 和 `SYNCCPU` 的调用条件。 |
| [QEMU 11.1.3 `vmware_vga.c`](https://raw.githubusercontent.com/qemu/qemu/v11.1.3/hw/display/vmware_vga.c) | 只作模型能力和降级行为的证据，不是完整 VMware 协议的规范来源。 |
| [PCI-SIG PCI Local Bus 3.0](https://pcisig.com/PCIConventional/Specs/Base/LocalBus_3.0)；[POSIX.1-2024 `mmap`](https://pubs.opengroup.org/onlinepubs/9799919799/functions/mmap.html)、[`close`](https://pubs.opengroup.org/onlinepubs/9799919799/functions/close.html)、[`read`](https://pubs.opengroup.org/onlinepubs/9799919799/functions/read.html)、[`poll`](https://pubs.opengroup.org/onlinepubs/9799919799/functions/poll.html) | PCI 配置空间、资源类型和中断规则；用户进程可观察的文件、等待和映射契约。 |

线格式与寄存器以公开设备协议为准，Linux 源码用于确定相同 UAPI 的行为，Mesa 用于核对实际调用，QEMU 只说明一个仿真模型；若资料不一致，记入覆盖表并用对应设备与 ABI 的可复现观察解决，不从其他设备推定行为。

关键文件的 SHA-256，以复现这份设计核对的源码内容：

```text
1303b1df3983ea310862d1187a0d01797f3511fa6fc6bf406c9bb779beb4c4a4  Linux device_include/svga_reg.h
be97542a60591fccfaf40aaf72738d8ad8a5e7218404410850ab234eb513152c  Linux device_include/svga3d_cmd.h
3947fda4450d23ffe3a3ad8caf8d22f1512964039256b186463f0ef3a75e1608  Linux device_include/svga3d_dx.h
b1601123d2701092a0add1bd76ce64dbbc44072cdf56be99b346423838ccbb76  Linux / ReliefNT vmwgfx_drm.h
d010f39fb01e50c507ef79feb1f8944c8086199f9220a4d7dc3dd4c574d4e268  Linux vmwgfx_drv.c
e30d6330bdfbec2476497a661de187c2903361936d98c52bd3b7aa34f2a2e3bc  Linux vmwgfx_execbuf.c
5a211bb7e1c0be4890166793292da59a2631a33ec09fbd0801352f292d5cf4f9  Mesa 25.3.0 vmw_screen_ioctl.c
57f631ab9c0b1a7bad0c4451d768b4816517d6385941cc21d8c7d52cb47fd441  QEMU 11.1.3 vmware_vga.c
```

ReliefOS / ReliefNT 的新实现遵循各自 Apache-2.0 许可和代码规范；保留的 `vmwgfx_drm.h` 维持原 SPDX 与版权。若导入公开协议定义，只取需要的定义，保持独立文件、原有 `GPL-2.0 OR MIT` 授权和 VMware 版权及许可全文，按 MIT 选项使用，不把导入文件重新标记为 Apache-2.0。Linux `vmwgfx` 的源码按每个文件实际 SPDX 核对，不能用协议头的许可推定通用 DRM/TTM 文件的许可。本设计不规划移植 Linux core/TTM 或照搬 QEMU/Mesa 实现；实施时在第三方来源记录中列清导入文件、原路径、版本/哈希、许可、署名与本地修改。
