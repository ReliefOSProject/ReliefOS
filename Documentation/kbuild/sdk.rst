================
SDK 与用户态移植
================

消费 SDK
========

SDK 为可重定位 musl sysroot，C 驱动 ``reliefos-musl-cc`` 按自身位置找头与库。
验证时必须真正编译、链接和运行适当探针，覆盖带空格的重定位路径、动态与
``-static``；成功列出归档成员不能替代消费验证。
公开头通过白名单导出，不复制内核私有头。

库与 ABI
========

目标环境使用 musl、mimalloc 和 ReliefOS 扩展库。保持实际 SONAME 与动态
装载依赖，不用宿主 glibc 库冒充目标产物。旧库与旧转发头的兼容范围见
:doc:`../userspace-api/abi`。新接口需同步公开头、运行库、内核实现、消费者、
头导出及包归属，不能只改声明。

SDK 同时携带锁定版本的 zlib、libpng、SQLite、mbedTLS 3 头文件、动态库和静态库。
驱动为扩展库补全 PNG、zlib、mbedTLS 的传递链接依赖；直接使用 SQLite 的程序
仍需传入 ``-lsqlite3``。开发包的来源元数据位于 ``share/reliefos/sdk/apk``，
许可位于 ``share/licenses``。所有 mbedTLS 消费者必须使用随包配置头，不能使用
旧私有 ``MBEDTLS_CONFIG_FILE`` 配置改变结构布局。
