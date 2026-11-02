# services107：接通原生网络枚举完成通知

106自动同步返回REC完成。用户要求恢复的音频原生UID优先、PowerUI注入均保留。
HideUntilCheckIn策略没有解除MIG等待：本轮仍无渲染提交，不能称界面已恢复。

## 实际双向等待栈

wifid主线程为正常CFRunLoop，但其原生管理器串行队列卡在：

`WiFiManagerScheduleWithQueue → WiFiDeviceManagerOpen → SCNetworkInterfaceCopyAll → dispatch_once wait`

持有这个once的另一线程卡在：

`CWFSCNetworkInterface → SCNetworkInterfaceCopyAll → __wait_for_IOKit_to_quiesce → SCDynamicStoreNotifyWait`

SpringBoard主线程又同步等待这个管理器的`wifi_manager_open`回复，连带阻止首帧。
以上来自35/90秒真实进程栈，并分别校正dyld cache与wifid映像slide；不是根据末行猜测。

22H374 SystemConfiguration实际读取`Plugin:InterfaceNamer`字典，等待`*QUIET*`、`*COMPLETE*`或`*TIMEOUT*`任一原生终止标记。
Pong虽然已将真实en0接入DLIL、发布IO80211Interface且SC配置成功，但自命名路径没有完成这项握手。

## 修补

在现有独立SC初始化进程内，对实际已存在的en0执行5秒`IOKitWaitQuiet`。
仅真实成功才发布`*QUIET*`；真实超时发布SC原生支持的`*TIMEOUT*`，不谎报quiet或成功。
其他错误保留错误，不发布终止状态。保留已存在的原生终止标记和其余字典字段。
随后的SC接口列表仍枚举真实IORegistry接口；没有注入假接口、假设备列表或AllowEnable。
SC工作进程仍有20秒独立deadline，不阻塞init/launchd。

API语义参照[Apple IOKitLib源码](https://github.com/opensource-apple/IOKitUser/blob/master/IOKitLib.h)；
实际等待键与分支以所选22H374实现为准。用户维护的基础构建和userdata脚本未修改。

UniXNU/UniXNU-sheng/Pong远程主分支本轮重新fetch，无比当前已合并状态更新的提交。
保留电池、ADSP、WLAN原生4KiB/固件、亮度、八核、Metal和Data，运行时构建后继续临时启动抓取。

## 本轮设备结果

107按180秒同步、提交日志后自动返回REC，capture.json为complete，console/pmsg均包含本轮ID。
原生SC wait实际返回kIOReturnTimeout（-536870186），TIMEOUT终止通知发布成功。
MobileWiFi随后收到真实系统上电请求、发现1个设备，未再出现本轮manager deadline。
SpringBoard主线程已回到正常RunLoop，不再停wifi_manager_open。
显示层出现first visible frame，后续持续swap，不能据此认定用户全部桌面交互已通过。
实际QCA6490首轮扫描29个网络；尚无系统设置关联/DHCP成功证据，AllowEnable未返回CFBoolean。
音频物理device33/VAD62仍没有确认路由和StartIO，音量与出声不能称已修复。
PowerUI与音频UID优先保留，ADSP、实际容量、电源和Metal硬件路径未撤回。
剩余日志包括CoreLocation授权同步请求等待、原生音频VAD查询无结果、BlastDoor不能在无sandbox环境运行，
以及Shortcuts本地化资源编码崩溃；这些不再当作同一启动卡死原因。
