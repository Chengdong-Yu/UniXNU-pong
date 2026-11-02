# services99：原生网络、音频路由、资源等待与 Data 重建

## 输入与实际日志

读取 services98 REC console/pmsg、Data 的 wifi-bootstrap 日志，以及 Preferences 的 22H374 stackshot。也阅读用户提供的 unixnu-speaker-docs 两篇文档；其中 SM8850 / CS35L43 / iOS16 的结论与 Pong SM8475 / TFA9873 / Darwin24 分开处理。

- 用户确认系统 Wi-Fi 开关仍不可用。QCA6490 原生驱动仍注册并扫描，不能以此声称系统开关、关联或 DHCP 已接通。
- stackshot 明确显示 wifi-bootstrap 阻塞于 SCPreferencesLock → wait_for_PreferencesMonitor → SCDynamicStoreNotifyWait。在旧设计中，这阻止后续所有原生开启请求。
- audiomxd 已启动，物理设备35可解析；设置 dOut/sOut 到 VAD56 后读回仍为42，路由未确认，也未观察到 StartIO。
- GPU 明确记录 texture OutOfMemory(Device)。stackshot 同时有756500个空闲4KiB页（约2.9GiB）、memoryPressure=false。不是系统物理内存整体耗尽的证据。最终屏幕复制约0.8–1ms，GPU等待约13–19ms，呈现间隔明显不稳定。

## 已修补

### Wi-Fi

真实 en0 和 IO80211Interface 存在后直接向 stock wifid 的 MobileWiFi manager 请求开电源；保留飞行模式。真实设备到达通知使用实际 BSD Name=en0，不再以 SCPreferences 或 CopyDevices 完成为请求前置条件。可阻塞的 SC service 配置移至单独15秒限时进程，不阻塞主初始化或系统启动。未修改设置控件返回值，未伪造网络、设备或连接状态。

### 音频

保留真实 APM、TFA9873、48kHz stereo、共享 PCM 环和系统 HID 音量键。依据所选22H374的 vaem 调用布局，在实际 VAD 上查询真实 prts 端口，使用 rtcf 32字节请求选择可用 ptbo 驱动输出端口，读取 vcsp 确认系统音量能力；不再反复设置默认设备号。没有真实端口则保留失败，既不发布假麦克风也不把 SM8850 的 Hawking/CS35L43 直接搬来。

用户扬声器文档指出 DSP 可先提供24个小 intent 再提供大 intent。Pong 原来远端表为24，现扩至48；本地表和实际 Pong GPR/SMEM 常量不变。为 GPU PAS/ZAP 和 ADSP PAS 引入同一个 SCM会话锁，整个中断恢复过程持锁，避免二者并发进入安全固件。仍保留真实失败与 WAITQ拒绝，没有伪造安全世界成功；完整 WAITQ IRQ owner 仍需独立适配。文档的原生 IOAudio2Device 13方法/状态页、Speaker/Hawking以及响铃开关契约值得后续移植，但必须先核对Darwin24 ABI；本轮未用20H392布局替换22H374，也未复制CS35L43参数或增加不存在的输入能力。

### CPU/GPU/内存

八核与 X2→A710→A510 空闲核心偏好保留，EPSS 请求不越过已允许的非boost上限或热控。GPU早期连续池由旧256MiB上限扩为512MiB，失败仍逐级缩小；4KiB映射范围记录由131072扩为262144。未把物理内存空洞交给 VM，也不改变页大小。

BoMap/BoFree 等真实 GPU fence 时不再整段持有跨进程全局 gate：短轮询在锁下核对，未完成时在锁外休眠，再在同一锁下确认所有任务退休后修改页表。每客户端操作锁、真实 fence、DMA/缓存维护和失败隔离保持。未通过伪造完成或无条件解锁换取帧率。

## Data 与构建

用户明确授权清空 iOS Data。先保存本轮日志和 stackshot，再从原始空 APFS模板与当前22H374 seed重建独立8GiB Data；加入现有请求的rootless Sileo/Procursus文件，不带回旧用户数据库。OS/SystemOS、Pong固件与永久boot分区保留。

已清理122个旧临时启动构建目录，保留services98回退镜像、全部源码、日志、私有固件和系统输入。Darwin24内核、原生Wi-Fi初始化和音频运行时已编译，启动包已生成。用户维护的基础构建脚本未修改；未运行测试/基准/镜像哈希/镜像回读。自动重启继续关闭，实际外设可用性由新启动观察确认。

## 安装结果

72 MiB 配套 OS 差量及重建的8GiB Data均写入完成，安装回执为 complete。SystemOS与固件未改；未运行镜像哈希或回读比较。随后执行Fastboot临时启动，自动回REC继续禁用。

Fastboot Sending 2.322s / Booting 10.178s 均 OKAY，保持设备运行，未写永久 boot 分区。该回执不作为 Wi-Fi 开关、媒体出声或性能已修复的证明。
