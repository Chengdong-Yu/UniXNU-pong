# services97：网络发现、原生音频路由与画面所有权

## 依据

services96 的 REC console/pmsg、Data 诊断，以及选定22H374的 MobileWiFi、wifid、SystemConfiguration、MediaExperience、VirtualAudio、IOMobileFramebuffer 调用。Apple ABI 拆解与原始组件留在忽略目录。公开网络实现参考 [configd 的 SCNetworkInterface](https://github.com/apple-oss-distributions/configd/blob/main/SystemConfiguration.fproj/SCNetworkInterface.c) 和 [InterfaceNamer](https://github.com/apple-oss-distributions/configd/blob/main/Plugins/InterfaceNamer/ifnamer.c)。

## 网络

真实 QCA6490 station 和 en0 已存在，MobileWiFi 却未发现设备。Data 没有 NetworkInterfaces.plist，网络 preferences 只有版本号。22H374 wifid 在设备到达回调中先要求 State:/Network/Interface 的 Interfaces 列表含该 BSD 名称，然后由真实 IONetworkInterface 建立设备。

驱动标明 SelfNamed，因为它已通过 DLIL 把物理接口命名为en0。板级初始化只在 if_nametoindex 和实际 IO80211Interface 同时存在、原生 SC 创建的类型为 IEEE80211 时，建立缺失的原生网络服务和默认配置；已有配置保留。将真实 BSD 存在发布到原生接口通知键，再通过 stock wifid 请求电源。不是伪造 Wi-Fi 设备、开关状态或热点。

按选定系统实际 entitlement 类型，SCPreferences 写权限仅列 preferences.plist，SCDynamicStore 写权限为布尔值。一次电源请求的标记不再阻止后续启动重建瞬态接口状态。真实关联、DHCP和收发仍须分别确认。

## 音频

物理 PongAudioDevice 默认输出选择成功不代表 iPadOS 会建立系统音量路由。恢复到选定系统实际 VirtualAudioDevice_Default / SystemLocal 的原生输出路径，先要求物理Pong输出和真实VAD存活，通过UID解析实际对象号；没有VAD就保留失败，不伪造端口或固定设备ID。保留标准音量/静音控件、真实PCM增益和原生HID按键，不恢复直接调音量的自制按键实现。

## 显示

CPU最终拷贝降低了驱动耗时，却暴露原来MiniIOSurface不跟踪真实GPU工作、SwapWait立即返回成功的缺口。

增加Pong自有IOSurface扩展，由调用者任务中的真实IOSurface Mach send right授权。Metal提交前取得所有相关surface的GPU使用权；真实完成并完成缓存维护之后释放。显示等待这些使用权，并在拷贝期间排除新GPU提交。多surface竞争时撤销未提交的部分获取再有界重试，避免交叉等待。进程退出但工作未完成时将对应帧标为失败；不伪造fence或提前重用未知DMA。

SwapEnd返回排队错误；SwapWait等待本swap的真实退休结果，遵循22H374输入布局和毫秒超时，最长1秒。只有显示拷贝结束后才让出队列槽。保留Metal硬件合成、四线程最终CPU拷贝、实际120Hz模式、现有GPU故障隔离和缓存维护；不修改动画计时。

## 构建与实机状态

Darwin24内核、Metal前端与插件、原生网络初始化和音频运行时编译通过。Rust/Vulkan归档复用上一版，未改变其接口。用户维护的基础构建脚本未修改。未运行测试、基准、镜像哈希或回读比较。配套OS更新和临时启动结果另记；主机构建成功不等于Wi-Fi、音量或呈现已在设备工作。

136 MiB OS差量已安装，Data和SystemOS保留，未写永久boot分区。设备功能结果以本轮实际捕获及用户操作为准。

## 真机回归结果（services97）

用户确认本版未显示 iOS 界面。REC console 中 SpringBoard 和 backboardd 已运行，但没有首个 SwapEnd；audiomxd 每 5 秒出现 posix_spawn EACCES。检查本版改动发现两处回归：新增 SwapWait 拒绝 ID 0，而 22H374 IOMFBDisplay::is_ready 和 update_power_state_locked 在首帧前明确使用 0；新增 stderr 重定向要求 mobile 在 root 所有的 /var/logs 创建文件。services98 分别修复零 ID 的真实队列查询与服务日志权限问题。

pmsg 没有 timed-return-sync / logs-committed-reset，不据此判定回 REC 的原因。用户现已要求移除自动重启；services98 删除 ArmCpu 的定时重启调用及其板级实现，保留原有 console/pmsg 路径。
