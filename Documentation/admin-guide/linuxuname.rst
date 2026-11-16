=========================
Linux 系统名兼容启动命令
=========================

``linuxuname 程序 [参数…]`` 启动指定程序，让该程序调用 ``uname(2)`` 时
得到 ``Linux`` 系统名。例如::

    linuxuname uname -s
    linuxuname java -jar HMCL.jar
    linuxuname sh -c 'uname -s; exec /path/to/program --option'

命令按 ``PATH`` 查找程序，也接受绝对路径、相对路径及脚本。
程序参数、环境变量和工作目录原样保留；命令用 ``execvp`` 替换自身，
程序的退出码和信号直接传递给调用者。``--`` 可用于结束命令本身的解析::

    linuxuname -- /path/to/program 'argument with spaces'

作用范围
========

该选择由内核为调用任务保存，经过 ``fork``、``vfork``、``clone`` 和
``exec`` 继承，因此目标程序创建的线程及子进程也会看到 ``Linux``。
启动它的终端和其他已存在的进程仍看到 ``ReliefNT``。
动态链接、静态链接和直接发起 ``uname`` 系统调用的程序均适用。

仅 ``utsname.sysname`` 改变；内核版本、构建信息、架构、主机名和域名
保持原值。``/proc`` 中的全局内核信息不变。此命令可通过仅检查系统名的
兼容性判断，缺失的系统调用、驱动或其他 Linux 功能仍需实际实现。
普通用户可使用，不要求 root 权限。

错误处理
========

``linuxuname --help`` 显示用法。没有指定程序或内核不支持该选择时返回
125；程序无法执行时返回 126；程序不存在时返回 127。

组件默认构建并随系统镜像安装，在 Menuconfig 的 System applications
中可关闭 Linux uname launcher；安装后的命令入口为 ``/usr/bin/linuxuname``。
