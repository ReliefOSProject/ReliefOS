# ReliefMail 证书与连接排查

添加普通邮箱时，账户“高级”页的“自定义 CA 文件（可选）”留空。
ReliefMail 使用 `/etc/ssl/certs/ca-certificates.crt`，并校验服务器证书链、域名和有效期。
企业或自建邮箱的私有 CA 文件由管理员提供，按账户单独选择。

## 修复或更新系统证书

账户“高级”页提供“修复或更新系统证书”。使用默认 CA 的连接测试遇到证书库加载
失败或信任链错误时，也可从错误对话框进入修复。

该入口启动 `reliefapk --repair-ca`：

1. 检查证书恢复条件；系统 CA 可用时，在线模拟固定证书包的变更。
   恢复计划会说明两个 CA 包的 world 请求将刷新为不带版本或仓库标签约束的包名；
   使用已配置的默认签名仓库，其他请求保留。
2. 在 ReliefAPK 中检查变更并应用，使用其现有 sudo/root 密码认证。
3. 系统 CA 不可用时，先原子恢复随系统分发的证书库，将原有常规文件保留为
   权限 `0600` 的独立备份。随后才进行在线包可用性检查和真实 apk 模拟。
4. apk 使用已配置的软件源和包签名验证获取固定的
   `ca-certificates` 与 `ca-certificates-bundle` 软件包。先用 `add --upgrade` 更新
   两个请求及其版本，再用 `fix --reinstall` 修复源文件和生成文件。每次真实变更前
   均重新模拟，依赖仍由 apk 求解。HTTPS 源必须通过 TLS
   证书验证；管理员配置的本地或 HTTP 源也必须通过 apk 签名验证。
5. 安装后重新解析证书库；文件仍不可用、新生成 `.apk-new` 或 apk 报告
   `APK unavailable, skipped` 时报告失败，不把退出码 0 当作实际执行的充分证据。
6. 返回 ReliefMail，重新输入测试所需的邮箱授权码并测试连接。

证书修复不会发送邮件。邮箱密码与系统提权密码分别由两个程序处理，不通过启动
参数传递。账户已指定私有 CA 时，应检查该文件，不会自动替换为公共 CA。

下载使用当前有效的系统证书库；文件缺失或损坏时，使用随 ReliefOS 分发、
由 root 管理的 `/usr/share/reliefos/certs/ca-certificates.crt` 恢复系统证书库。
恢复文件及其父目录必须由 root 拥有、不可由组或其他用户写入，且不能是符号链接。
预览阶段说明恢复计划，不修改系统证书。经认证应用后才恢复文件并进行在线模拟；
恢复后的网络或签名检查失败时保留已恢复的出厂 CA，并明确报告更新失败。
预览与实际修复禁用磁盘缓存；检查包可用性时仅请求元数据，不下载归档。
固定修复工作程序启动 apk 时使用独立的最小环境，忽略调用者的 `APK_CONFIG`、代理和 TLS
环境变量，也不加载全局 `/etc/apk/config`，防止配置关闭签名或证书验证。
APK 输出中的仓库 URL 会隐藏，避免将可能包含的仓库凭据写入操作日志。
软件源、签名密钥与架构仍使用系统配置；仅依靠环境变量配置代理的网络需要管理员
先提供可直接访问的软件源。
邮箱连接仍按账户配置验证。apk 当前下载库优先读取编译时指定的 CA 文件，
恢复过程直接修复该系统文件，不依赖环境变量覆盖该路径。
在线操作前和安装后均检查系统 CA 文件及其父目录的信任条件。即使 PEM 可以解析，
符号链接、非 root 属主或组/其他用户可写的路径也会被拒绝。管理员应先核对来源，
恢复 root 拥有的常规文件和安全权限；程序不会把这种文件直接用作特权下载的信任锚。

`reliefos-trust` 同时提供恢复证书库和
`/etc/apk/protected_paths.d/reliefos-ca-bundle.list`。apk 3 的文件替换保护按目录生效；
该规则把 `/etc/ssl/certs` 作为生成的证书输出目录，允许签名证书包替换损坏的已安装
文件，避免仅生成 `.apk-new` 而留下错误文件。私有 CA 源文件宜保存在
`/usr/local/share/ca-certificates`，账户专用文件可放在其他适当目录。

更新已有系统时，应同步更新 ReliefMail、ReliefAPK 和系统信任包/配置。仅复制新程序
到旧镜像，可能缺少恢复证书文件或 apk 保护规则；应检查安装日志及 `.apk-new`。
自定义保护策略仍需由管理员合并；修复程序会检查最终证书文件是否可解析。
不能只更新汇总包：[Alpine 的安装钩子](https://github.com/alpinelinux/aports/blob/3.24-stable/main/ca-certificates/ca-certificates.trigger)会执行 `update-ca-certificates`，按源文件
重新生成证书库。仅替换汇总文件可能被旧源集合覆盖；所以修复流程同步维护两个包，
并移除它们在旧镜像中遗留的精确 world 约束，允许后续安全更新。

## 按实际原因处理

| 提示 | 应检查的内容 |
| --- | --- |
| 系统 CA 文件不可用 | 文件是否存在、可读、完整，必要时使用修复入口。 |
| 自定义 CA 文件不可用 | 文件路径、权限和 PEM 内容，或留空使用系统证书。 |
| 服务器证书链不受信任 | 服务器地址、中间证书链、私有 CA 选择，以及系统根证书是否需要更新。 |
| 证书已过期或尚未生效 | 系统日期、时间和服务器证书有效期。 |
| 域名不匹配 | IMAP/SMTP 主机名与服务器证书是否相符。 |
| TLS 协商失败 | 端口、隐式 TLS/STARTTLS 选择、网络状态与双方协议支持。 |

CA 包中部分证书无法解析时，只要有可用根证书，程序会继续用这些根验证服务器；
服务器身份、有效期和信任链校验仍为必需。全部证书不可用时拒绝连接。
诊断保留失败阶段、TLS 返回码、校验标志和系统时间，便于区分原因。

## 定向验证

```sh
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmail.sh protocol
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefmail.sh worker
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefapk-model.sh
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefapk-certificates.sh
TMPDIR="$PWD/out/tmp" sh tests/build/test-reliefapk-process.sh
TMPDIR="$PWD/out/tmp" python3 tools/tests/test_reliefapk_apk.py \
  --apk out/xorg-vmware-debug/upstream/apk/apk.static
TMPDIR="$PWD/out/tmp" make O=out/xorg-vmware-debug PROFILE=debug -j4 \
  app-reliefmail app-reliefapk nls image-vmdk
```

QEMU 工具 `tools/test_reliefmail_ca_qemu.py` 使用独立磁盘副本及临时签名仓库，
测试 CA 损坏后的可见错误与修复流程。测试 CA、仓库密钥和账户均为夹具，
不会安装到宿主系统或生产输入镜像中。
