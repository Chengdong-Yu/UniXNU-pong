# services132：锁屏/唤醒 scanout 退役与固件 USB 所有权

## services131 实机

REC 的 console/pmsg 共1370640字节。USB在首次MMIO前明确跳过，系统已进入SpringBoard并有真实GPU scanout完成。电源键down/up都由HID发出，锁屏/唤醒后touch mode出现0x1fe、0x101、0x1；最后一次电源事件后swap3783/3786报告surface183未完成（500ms），presenter拒绝一个未提交的拷贝（kNoResource）。此后launchd、音频客户端和其他服务仍继续打印到04:09:37，没有panic。因此不能把画面停住直接说成所有CPU停死，按键传递也不是缺失环节。

本轮音频记录真实MI2S PRM申请/释放、PCM和DSP完成。TFA状态是在第一批PCM提交之前读取的，不能把当时尚无AMPS位直接当作稳态喇叭故障结论；尚无出声确认。

## 成对的 scanout 缓存退役

原实现只在presenter拒绝未提交帧时清理用户缓存，内核仍保留旧source mapping/prepare；临时command buffer和encoder的autorelease pool也尚未结束。熄屏空层没有通知presenter退役这两侧引用。这是实际生命周期缺口，日志中的唤醒资源拒绝与之相符，但尚不能排除其他渲染任务问题。

- 空画面且无在途offer时，内核向等待线程返回idle通知，暂停新的GPU offer。
- 用户侧先释放导入缓存，再退出autorelease pool，之后通过新私有ResetSources方法确认内核可以释放旧source映射和prepare引用。
- 刚完成或未提交offer仍在交接时返回Busy；调用方短暂让出CPU并有界重试，不持锁等待。只有核实无offer在途才能退役映射。
- 下一帧使用新的source generation重新导入。真正已提交而完成未知的DMA仍隔离，不清除poison、不伪造surface已完成、不并发CPU覆盖。

配套内核和Metal bundle一起更新，保留ABI1 bridge-services123、Turnip和128MiB池。GPU加速仍启用；CPU仅沿已有安全回退处理未提交的帧。

## 音量键追加修复

用户追加报告音量键也会触发卡住。原生IOAudio2 startOutput在持有PCM共享gate时等待实际DSP启动，最多500次10ms；volume/mute control和PCM fill共用此锁。现改为记录starting状态后释放锁，再等待真实硬件结果；并发start返回Busy，不增加伪造的running用户。音量/静音属性发布用独立controlGate串行化，更新采样增益时仅短暂取得PCM锁；分配字典、registry属性发布和messageClients不持PCM锁。未改为自定义按键调音量，也未伪造设备已启动。此项只需新的内核，正在写入的Metal bundle保持匹配，不重写Data。

## USB 所有权的真实实现与边界

DarwinBoot24在Apple ADT的chosen中动态增加独立USB DMA、handoff和status槽，正常解析和boot_args长度按扩展后的树计算；不修改用户维护的ADT/打包脚本。

- UEFI通过AllocateMaxAddress/EfiReservedMemoryType分配独立低32位32KiB，和原音频64KiB分配由固件分配器保证不重叠。XNU额外RAM捕获时仍排除保留页。
- 输入文件都读取完成后，仅当bootargs请求USB时通过真实EFI_USBFN_IO_PROTOCOL尝试StopController。记录LocateProtocol/StopController实际EFI_STATUS；loader失败返回时尝试恢复原固件控制器。停止操作后重新获取内存映射，再退出BootServices。
- 成功停止仅设置“旧固件DMA所有权结束”位2，不设置PHY/repeater保留位1。不能从StopController成功推断板级供电存在。XNU打印真实状态，缺少PHY保证仍在MMIO前跳过。

因此本轮不是AppleUSBDeviceMux/Finder接通，也不声称已经能够CDC枚举；完成的是此前缺失的独立分配和固件释放接口。下一步仍需要SM8475 eUSB2/repeater的实际供电/保留条件，不能靠填一个假flags=3重新引入启动卡死。

## 部署

主机内核、匹配Metal bundle和loader均已编译。当前REC的实际包版本/配置状态与此前保留代际没有新增真实包载荷，已保留最新dpkg状态，仅替换Metal bundle；音频/UI/rootful、Data/SystemOS不重置。OS使用完整逻辑代际流写入，避免将旧主机APFS元数据差量覆盖guest改写的卷。没有镜像哈希、读回比较或性能基准，自动重启关闭。完成安装和临时启动后补记实际结果。

ABI检查追加：AGX handoff用户客户端的分发上限已从0x7003扩到0x7004，新ResetSources方法可到达真实Pong处理函数；不是仅新增一个无法被调用的函数。最终内核在音量锁改动和分发改动后重新编译，配套插件无需第二次修改。USB协议在访问StopController字段前检查标准Revision，bootarg按独立token匹配；不请求USB时不调用固件停止接口。

最终部署：OS9663676416字节安装完整receipt=complete，Data/SystemOS保留。最终内核包含0x7004分发和音量锁修补；/Users/desktop/Downloads/Pong-build-services132-final/pong-xnu-initial.img 已临时启动，Sending2.323秒、Booting10.129秒均OKAY。未写boot分区，设备保持运行。启动后即时主机快照未发现Pong CDC或Qualcomm EDL；不能据此证明界面或按键效果，USB PHY仍未实现安全接管，不宣称USB工作。
