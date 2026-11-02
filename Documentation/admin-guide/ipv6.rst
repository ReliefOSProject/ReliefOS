==========
IPv6 网络
==========

系统支持 IPv4/IPv6 双栈。启用的以太网接口自动生成链路本地地址，并通过
重复地址检测和路由器通告配置 SLAAC 地址、前缀路由及默认路由。所在网络必须
实际提供 IPv6；没有路由器通告时不会伪造可连接公网的 IPv6 地址。

查看与配置
==========

可以使用未修改的标准用户态工具查看和临时配置地址：

.. code-block:: sh

   ip -6 addr show
   ip -6 route show
   ping6 -c 3 ::1
   ping6 -c 3 fe80::1%eth0
   ip -6 addr add 2001:db8:1::20/64 dev eth0
   ip -6 route add default via fe80::1 dev eth0

``2001:db8::/32`` 是文档示例地址，实际使用时换成网络分配的地址。
链路本地目标必须带接口区域；也可以使用工具提供的接口参数。
地址和路由变更需要 root 或相应的 ``CAP_NET_ADMIN``。raw ICMPv6 socket
需要 ``CAP_NET_RAW``。临时配置不修改持久化的 IPv4 profile。

DNS 自动配置
============

``reliefos-rdnss`` OpenRC 服务运行 ``rdnssd``，读取路由器通告中的 RDNSS
选项。它检查链路本地路由器来源、hop limit 255、接收接口和完整选项长度，
按通告有效期撤销 DNS，接口禁用时清除该接口的通告缓存。链路本地 DNS
带区域标识，避免将同名地址发到错误的接口。

服务只更新 ``/run/reliefos/ra-dns``，现有 ``publish-resolver`` 统一发布
``/etc/resolv.conf``。自动模式合并 IPv6 RDNSS 与 IPv4 DHCP DNS，去重后
最多发布三个服务器。显式的自定义 DNS 或 Cloudflare 策略优先，RA 不覆盖
用户的选择。DNS 搜索后缀的 DNSSL 自动配置尚未提供。

.. code-block:: sh

   rc-service reliefos-rdnss status
   cat /etc/resolv.conf
   cat /var/log/rdnssd.log

当前默认自动地址来自 RA/SLAAC；不自动运行有状态 DHCPv6 地址租约客户端。
网络控制器的持久化配置页仍管理 IPv4，IPv6 通过标准命令管理。关闭可选的
网络控制器 GUI 不影响 IPv6 和 RDNSS 服务。

排查时分别检查链路、地址、路由、数值地址连通性与 DNS；能 Ping 通不等于
DNS 或 TLS 已成功。实现和 Linux ABI 的边界见 ReliefNT 仓库的
``Documentation/userspace-api/ipv6.rst``。
