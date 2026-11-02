# Sileo、APPSSMMU 与音频运行时修补

本轮读取了手机REC中的console/pmsg，以及Data上的`logs/pong-sileo.log`，没有进行镜像哈希或镜像内容回读比对。

## 日志证据与修改

| 实际结果 | 修改 |
| --- | --- |
| PCIe0链路、BAR和128MiB DMA页表已建立；SID1c01配置失败，TCR2读回`0x38071`而请求为`0x38011` | 对仅允许未标记低32位IOVA的Pong WLAN上下文显式选择现有native4K `ignore_top_byte`策略；继续严格核对其余寄存器、根表、ASID与故障状态 |
| APM ready、TFA地址34/35识别到revision b73；音频stage4失败，HAL没有物理输出 | SID1801位于同一APPSSMMU，使用相同TBI策略。增加失败时的实际context寄存器信息；不提前发布不可用端点 |
| 按键加减均抵达SpringBoard，native increase/decrease执行；电源HID保持工作 | 核对22H374实际SBVolumeControl入口，保留原生控制路径。当前没有物理音频端点，修复其前置DMA配置；不能仅凭按键送达声称可调音量 |
| Sileo的dpkg配置完成，但旧uicache反复显示Unable to register并仍返回零，错误写下完成标记 | 增加mobile用户的原生LS注册器，确认代理有效且路径匹配才记录成功。OS保管更新后的首次启动脚本，保留设备Data；禁止旧uicache触发所有系统应用重新注册 |

注册器接口核对自本机22H374 CoreServices ObjC记录；权限名参考[Procursus uicache源代码](https://github.com/ProcursusTeam/uikittools-ng/blob/v2.1.8/uicache.plist)。没有采用其他OS版本的地址或签名验证绕过补丁。

## 构建与设备边界

完整格式2内核和init/UI/HAL/注册器均已编译完成，启动镜像保留8核、Pong CPU/GPU策略及完整屏幕日志。构建使用`/Volumes/PongDev/source/UniXNU-upstream`的完整格式2profile；另一本地文档checkout缺少arm64 profile，不作为构建输入。

本轮OS改动为5个已跟踪sparsebundle band，共40MiB；不替换Data或SystemOS。编译成功和Fastboot成功单独记录，不代表Sileo UI、PCM播放、音量控制、无线固件和扫描已在新一轮实机确认。
