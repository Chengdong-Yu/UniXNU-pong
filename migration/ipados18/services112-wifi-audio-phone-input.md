# services112：关联空回调、音频路由和手机输入资源

## services111 实机日志

REC console记录点击连接后发生内核指令取指异常，PC=0，LR=0xfffffff0091634f0。
本轮实际内核符号定位为`start_connect+0xc2c`；x3=0x40080，对应BSSID/TXPOWER更新。
新关联布局已进入真实站点流程并建立HT/VHT peer配置，但随后调用了空的
`link_info_changed`。这不是单纯的设置界面等待或未扫描到网络。

ath11k实现旧的`bss_info_changed`；共享ath12k站点使用拆分的
`link_info_changed`和`vif_cfg_changed`。后一个在关联完成及断连时同样为空。
生成ath11k operations表时，链路更新转到真实的旧BSS实现，vif更新用default
link及原changed位调用同一实现。保留WMI实际操作与失败结果，不以空操作代替。
已逐项核对station调用的扫描、取消扫描、频道、peer状态、密钥及TX回调。

## 音频

111中真实物理设备、两个VirtualAudio端口和rtcf响应存在，vcsp仍为0；
dOut/sOut设置返回1852797029（`nope`）。iPadOS的会话输出不能靠桌面HAL默认值设置。

按22H374原生`vaemGetVADPortIDFromVADPortType`与
`vaeCopyDeviceIdentifierFromVADPort`核对：prts返回CFNumber音频端口对象ID，
对象的puid为设备标识。现在匹配实际Pong物理输出UID，向原生rtcf提交对应的
overridden/routable ports，不再仅允许某个transport，也不再写被拒绝的默认值。
仍校验实际响应、实际音量能力及输出存在；相同端口不会每秒重复拆建音频图。
没有虚构Hawking输入设备，没有恢复按键直接改采样增益的旁路。
PCM启动、真实音量提示和扬声器声音仍需本轮实机确认。

## 输入法和手机资源

111 Data确有拼音、英文和emoji三个输入模式，TextInput偏好刷新已执行，
所以此前仅保存列表不足以证明输入法可用。当前iPad固件的TextInputUI只有
KBLayouts_iPad.dat；Apple同版iPhone11,8的18.7.10/22H374有KBLayouts_iPhone.dat。

已从Apple恢复固件下载所需OS成员、解包，只把匹配版本的资源补入现有框架。
491条安装记录包含手机键盘布局、动态布局、TextInput/WiFiKitUI/电话设置资源
及iPhone运营商配置目录；没有更换当前ARM64 dyld缓存、内核或基带二进制。
保留iPad布局。kbd原生MachServices保持，增加RunAtLoad，避免跳过Setup后等不到
setupexited事件；不新增替代输入法服务，不持续KeepAlive。

可复用安装入口为`userland/phone_components18.py`，要求本机已挂载的donor/root、
对应BuildManifest和匹配runtime，校验目标与来源均为22H374，跳过框架Mach-O。
Apple原文件留在本机输入目录，不加入仓库。获取/解包未运行镜像哈希、CRC审计
或镜像回读；标准AEA解包使用Apple公开FCS接口。

## 电话与移动数据的实际边界

完整MobilePhone.app已从同版iPhone固件提取并保存在本机donor。
它只有arm64e可执行文件，当前iPad7 profile使用ARM64共享缓存，因此本轮没有
将它注册成可运行电话App，也没有篡改Mach-O架构字段。源码包含基于真实LS proxy
的注册工具，只有可匹配的应用安装后才生成对应job。

Carrier Settings/Cellular Settings兼容资源与iPhone运营商配置已补充。
CommCenter的cellular/noncellular配置及真实baseband匹配保持。Pong的蜂窝基带传输
尚未接入，不能把配置文件、菜单或手机产品名当作可通话、可用移动数据的证据。
电话App执行仍需要配套arm64e用户空间适配，移动数据仍需要真实Pong基带后端。

## 构建与安装

Darwin24内核、匹配运行时与启动镜像已构建。上轮Metal改动、亮度、八核、
已有Data和SystemOS保持。定时重启保持关闭。仅更新当前OS窗口并临时启动；
实际安装和Fastboot结果保存于本机services112目录，不能替代功能实测。

本轮OS差量216MiB安装完成；Data/SystemOS未更新。Fastboot临时启动实际结果：
Sending 2.335秒、Booting 10.131秒，均OKAY。未写永久boot分区，未安排自动回REC。
手机键盘布局已进入OS，kbd原生job已按本轮策略写入。实机功能结果尚待观察。
