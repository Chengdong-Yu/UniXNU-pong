# services106：SpringBoard 同步等待 MobileWiFi

用户确认105仍无界面，要求恢复音频默认原生设备选择与PowerUI注入。两项已恢复。
105返回REC的日志有对应boot ID，但没有自动同步/reset提交标记，返回原因不确认为自动定时。

## 已定位的主线程等待

105 Data中的35秒SpringBoard快照，以该进程真实dyld cache slide校正后：

`mach_msg → wifi_manager_open+0xc0 → __WiFiManagerClientGetOrReconnectServerPort+0x19c → WiFiManagerClientCreate+0x17c`

它发生在SpringBoard的初始化调用链上，阻止首帧。backboardd主线程在正常CFRunLoop，
CP fence/Metal真实设备仍正常创建；不能把本轮问题认定为GPU硬件未启动，或音频/PowerUI改动所致。
无线helper同样等到manager deadline，真实en0发布与SC配置已经返回0。

## 本轮修补

原板级策略给legacy MobileWiFi Mach服务填True。这预留了接收权，但不证明原生wifid已check-in并调度接收。
改为原生`HideUntilCheckIn=True`（所选22H374 launchd包含该策略键）。
原生wifid保留check-in权；只有实际check-in后才向普通客户端暴露监听器。
未准备的端口不应让SpringBoard无限等待不存在的回复。
没有替代MIG服务器、假网络、关闭Wi-Fi控件或固定AllowEnable。

恢复实际PongSpeakerNative优先选择和PowerUIAgent注入；103已确认的ADSP、电池、64MiBDMA、固件和扫描路径保留。
保留104已执行成功的真实en0/SC顺序。沿用两次有界进程快照，补充wifid自身等待栈，
用以进一步定位原生接收路径；wifid注入入口不应用任何UI/设备能力hook。

另有Siri/Shortcuts在NSLocalizedStringResource的XPC序列化中SIGABRT；本轮报告已保留。
这是独立的编码失败，尚无异常错误详情，不通过吞异常、伪造归档或删除Siri来宣称修复。

编译/启动结果与界面、开关、出声分别记录，功能完成仍需实机证据。
