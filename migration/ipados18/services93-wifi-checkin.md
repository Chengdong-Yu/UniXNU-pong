# services93：原生 Wi-Fi 管理端口与背光量程

## services92 的结果

用户确认亮度调节已可用，Wi-Fi 仍不可用。自动 REC 抓取成功，HID 原生报告设备和 IOHIDEventDriver 首次建立；没有本轮音量键按下报告，不能推定系统音量已工作。GPU 执行及 scanout 完成，没有本轮 BV opcode 故障或纹理 OutOfMemory，但仍有 20–40 ms 的驱动帧处理时长，不能宣称性能问题已经解决。

## Wi-Fi 注册和系统服务

真实 QCA6490 发布 en0、IO80211Interface、Infrastructure 角色及 IO80211Controller，并由原生 IO80211 库打开，PCIe/MHI/QMI/扫描完成。原生 wifid 作业未禁用，保留 wifi-xpc 和五个 CoreWiFi XPC 端点。日志中原生应用反复查找 com.apple.wifi.manager 失败，只有电源 GET，无扫描/关联 SET。

检查选定 22H374 的 wifid：WiFiManagerCreate 将 `com.apple.wifi.manager` 传给真实服务器创建路径，WiFiManagerScheduleWithQueue 调用接收端的 bootstrap_check_in。恢复镜像的作业只列出新 XPC 服务；Pong 的导入缓存没有为传统 MobileWiFi 端口声明接收权。

在 board_services18.py 中把该 Mach 服务声明加到原有 com.apple.wifid 作业，并同时更新规范 plist 和 launchd 缓存。实际服务端仍是 Apple wifid；没有添加替代服务器、假联网结果或禁用系统开关。此处是端口接线修补，设备是否列出热点/关联需本轮日志确认。

## 亮度和音频

长回复 GET(52) 的 NT37705 DBV 是写入 SET(51) 数值的四倍：1604→6416、787→3148、3894→15576、4045→16180。将 GET 的四分之一量程归一，保持 SET 数值、真实超时和回读处理；修复成功写入被当作拒绝、重复切换 LP/HS 并回跳的问题。8 位短回复保持独立处理。

services92 的外部音频客户端没有发现 PongAudioDevice，因此没有开始 StartIO。增加 CoreAudio 原生 UID 翻译和按 ASPL bundle ID 查询插件的设备列表，再选择真实可用输出。没有伪造设备 ID、输入端口、开始播放结果；服务端 ASPL 回调仍不重入客户端 HAL。

## 构建

Darwin24 内核、Pong 驱动和 runtime-services93 编译通过；沿用 native HID prelink、八核及 X2 空闲选择、GPU 窗口和 fence 锁修复、Pong 活动槽固件、现有 Data。用户维护的已有构建脚本未改。未运行测试、性能基准、镜像哈希或镜像回读比较。

128 MiB OS 差量已安装，保留 Data/SystemOS；Fastboot Sending/Booting 均为 OKAY，自动 REC 抓取完成。

## 实机结果

本轮 Wi-Fi 在供电阶段提前退出：smpe3 的第一次型号/子类型读取为 00/00，尚未注册无线设备。此前 services92 的固件初始化及扫描成功不能作为 services93 的注册证据。传统 wifi.manager 查找失败没有再次出现，但必须在实际无线设备就绪时继续确认设置连接。

原生 IOHIDUserDevice 已被 IOHIDEventDriver 打开，物理音量减键的按下和释放报告返回成功。用户仍报告系统音量无效，因此不能把 HID 报告成功算作音量控制可用。音频 ASPL 在 audiomxd 内初始化且连接实际立体声服务；外部 CoreAudio 客户端记录普通/系统输出均选中设备 35，但没有 StartIO 记录，仍须确认系统播放链路。

本轮没有 DBV 回读拒绝、GPU BV 故障或纹理 OutOfMemory；这不等于已解决全部性能问题。
