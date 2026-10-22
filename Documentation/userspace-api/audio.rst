========
音频接口
========

设备与行为
==========

音频支持取决于控制器、codec、路由和当前构建。ALSA/OSS 兼容接口只承诺
实际实现的格式与操作，不因使用上游 alsa-lib 就自动符合完整 Linux 音频 ABI。
缓冲写入需处理非阻塞短写、回压、排空、占用及错误，不能丢弃未完成数据。
应用应明确区分静音、无设备和播放失败。

验证状态
========

机器可读的音频支持清单位于 ``configs/audio-abi-support.json``。
这份数据在文档整理时保持内容不变；其 guest_runtime/upstream_alsa 语义及
既有断言冲突仍需按原问题处理，不能通过修改标志掩盖。
ReliefPlayer 的具体编解码和输出范围见 :doc:`../applications/reliefplayer`。

控制设备与 soundctl
===================

使用 ``soundctl cards`` 列出卡号，``soundctl --card 0 controls`` 列出当前设备
实际提供的名称、ID、范围和通道数。示例::

    soundctl --card 0 get 'Playback Volume'
    soundctl --card 0 set 'Playback Volume' 50%
    soundctl --card 0 set 'Playback Volume' 10,20
    soundctl --card 0 watch
    soundctl --pcm default test --channels 2 --seconds 2

名称依卡而变。Master Playback Switch 的 on 表示打开，而 Playback Mute 的 on
表示静音。百分比按真实范围和步长取整；单值广播所有通道，多值必须匹配通道数。
枚举接受查询到的标签或项目 ID。route headphones 与 capture-source microphone
依赖 Output Source/Input Source 控件，缺少时返回 ENOTSUP，不假定固定项目编号。

访问遵守设备节点 DAC。当前默认节点由 root 拥有、模式 0666，管理员可通过
chmod/chown 限制；播放与录音节点的 DAC 独立。控制元素的锁属于打开的文件描述
对象（OFD），dup/fork 共享该对象；其他 OFD 不能窃取锁，最后引用关闭时释放。
事件订阅与排队也依附 OFD。不要按同名进程、路径或裸 fd 数字推断所有权。

共享 PCM 与状态
===============

默认播放使用 dmix、录音使用 dsnoop，硬件端为 hw:0,0。当前配置周期 8192 帧、
硬件缓冲 65536 帧，48 kHz 下约 170.7 ms 周期及 1.365 s 缓冲，有明显延迟成本。
共享服务持有对应方向的硬件租约，直接打开 hw:0,0 可能返回 EBUSY。
没有录音端点时不会伪造 capture 节点。

OpenRC reliefos-audio 服务在存在时恢复 /var/lib/alsa/asound.state；停止时先写
临时文件，再发布新状态，保存失败保留原状态。修改 /etc/asound.conf 前先记录
设备、格式与运行日志，不用扩大缓冲掩盖有限流输出错误。

错误处理与验收边界
==================

* EAGAIN：非阻塞临时回压，保留正数短传输进度并按 poll 重试，保留 EINTR。
* EPIPE：真实 XRUN，恢复前需 PREPARE；不能吞掉 XRUN 作为正常结束。
* EBUSY：可能是共享服务器或其他打开者持有独占硬件方向。
* ENODEV/ENOENT：区分存在但不可用的兼容节点与缺失的真实端点。
* 非阻塞 DRAIN 还需观察 STATUS 到达 SETUP；完成通知的 POLLERR 不能单独变成 EIO，
  STATUS XRUN 仍是失败。

用 aplay -l、arecord -l、amixer -c 0 contents、设备权限及 HDA/PCM 日志定位。
完整有限流、重复关闭重开、长期 XRUN 清洁度与真实 VMware HDA 可听性仍有未完成
验收；历史 EPIPE 与尾帧损失反例不能由一次重跑成功抹去。物理 codec 未验证。
AC97/ES1371 的窄 OSS 路线不等于 ALSA 卡支持，具体状态仍以支持清单与真实运行为准。
