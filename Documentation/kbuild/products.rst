==========
目标与输出
==========

产品
====

输出均相对于 O：

* ``kernel`` / ``kerneldebug``：generated/system 内核与调试载荷。
* ``runtime`` / ``userland``：公共运行库与所选应用。
* ``sdk``：packages/reliefos-musl-sdk.tar.gz。
* ``apk-repo``：packages/apk/repository。
* ``rootfs``：rootfs/managed 与 manifest.json。
* ``image-vmdk``：images/reliefos.raw 与 images/reliefos.vmdk。
* ``iso``：images/reliefos-live.iso。
* ``installer``：images/reliefos-installer.iso。
* ``rpr-pages``：独立 RPR 机器接口和人类页面。
* ``pages``：pages/ 网站，包括下载、RPR 与 Sphinx 文档。
* ``htmldocs``：Documentation/html/，只构建手册，不构建镜像。

all 包含 SDK 和三类镜像；release 再包含网站，均不自动上传。
文档与 Pages 的 Sphinx 依赖不影响单独构建内核和用户态。

运行与发布
==========

``make run``、run-iso、run-installer 启动对应介质。
默认 ``QEMU_BOOT=uefi`` 使用 OVMF；``QEMU_BOOT=bios`` 使用 SeaBIOS。
可覆盖 CPUS、MEMORY 和 QEMU 变量；无 KVM 的宿主用 ``QEMU_KVM=0``。

RPR、下载页与文档组装为一个 Pages 输出；RPR 签名、APK 索引和内核摘要保持
机器契约，不因文档主题更换而改变。发布目标要求内核 clean 且等于主仓 gitlink。
真实版本发布见 :doc:`../process/releasing`。

文档与 RPR
==========

完整 Pages 网站分别从主仓 Documentation/ 和所选 ReliefNT 仓库的 Documentation/
读取源文档，生成 docs/reliefos/ 与 docs/reliefnt/，docs/index.html 是产品入口。
两份手册使用各自的 Sphinx 配置和版本，不把内核源文档复制到主仓维护。
RPR 首页及内核下载页链接到相应手册，APK 索引、公钥、health 与内核发布元数据
路径保持原协议。独立 rpr-pages 仍只装配包和内核子树，可供 RPR 客户端使用；
页面中的站点导航和文档链接依赖完整 Pages 父目录，预览完整网站应使用 pages。
完整手册由 pages 发布。
任一手册构建失败时，保留上次完整网站，不发布部分文档。
