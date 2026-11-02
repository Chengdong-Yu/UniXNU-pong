# services88：原生外设接口与控件保留

## 依据

本机 services87 REC console 及 Data 诊断；实际 22H374 的 IO80211、CoreWiFi、WiFiKit、IOKit、CoreAudio、VirtualAudio 和 SpringBoard 调用路径。拆解结果仅保存在忽略的 out 目录，不随仓库发布 Apple 二进制或 ABI 转储。

本轮 fetch 后 UniXNU origin/main 为 33da813（设备文档更新），Sheng origin/main 为 08cde20。四份外设移植文档在现有本地版本与远程版本之间没有差异；保留脏工作树和用户更新的构建脚本，未盲目覆盖。参考结构来自官方 [显示](https://unixnu.com/zh/porting/display)、[网络](https://unixnu.com/zh/porting/network)、[输入](https://unixnu.com/zh/porting/input)、[电源/音频](https://unixnu.com/zh/porting/power)。

## 实机证据和修补

- **亮度：** services87 首次 SET 后返回 `2200b421`，有效载荷为高字节 00、低字节 b4，实际 DBV180。旧小端解码把它视为 46080 并永久停用服务。改成高字节在前；固件继承的初值 8192 单独保留原始读数，控制值限制在 DT 的 0–4095。工作循环、真实回读、渐变和未退休 DMA 的保护继续保留。退休后的畸形回包保留错误并允许后续请求，不删除服务/滑块。
- **Wi-Fi：** QCA6490 已完成真实固件初始化与扫描；系统只见枚举和电源 GET，没有原生设置的扫描/连接。原接口 type0 UserClient 一直拒绝打开。依据 22H374 库新增标准 ioctl tunnel：selector0 的 0x3c30 envelope（command、length、0x3c00 payload、0x28 request），selector1 标示已实现的 ioctl 通道，selector7 不宣告未实现的快速路径。内联/OOL 缓冲均有界，忽略嵌入的用户地址，凭证使用后清零。复用同一真实 station、扫描/关联和事件处理，不增加假联网状态、P2P 或 AWDL 能力。删除自制设置页面/私有 Pong 控制 API，交回系统界面。保留实际 Pong active modem_b 固件、电气和 native4K SMMU 配置。
- **按键：** 删除 notify 直接调增益、独立 HUD、按键重复定时器和 SpringBoard 的增益用户客户端。真实 PMIC 队列仍是唯一输入源；音量键使用标准键盘 Application collection 和 Consumer E9/EA 报告，经 IOHIDUserDevice 进入原始 HID parser/event driver，由系统管理 sender、音量和提示。系统原有 backboardd 的 user-access-device entitlement 与 Apple 公共资源用户客户端要求一致；不复制 Darwin22 固定虚表。原生报告服务无法建立时保留 Consumer 事件和真实错误。电源键、触控保持原有路径。
- **音频：** 保留真实 ADSP/TFA9873 输出与 AudioServerPlugIn，新增标准静音控制对象、OwnedObjects/ControlList、class qualifier 和属性通知；与现有真实共享静音/增益对应，不发布假输入端口。仅 audiomxd/mediaserverd 获得音频客户端。进一步追到 VirtualAudio 的调用方后确认，保留名分派比较的是 **UID**，不是显示名；现有 PongAudioDevice 本来就是通用驱动 UID。因此显示名调整不作为出声修复证据。services87 仍没有 StartIO，出声和路由效果须继续依据本轮设备日志确认。

## 控件与错误策略

保留按钮、Wi-Fi 开关、音量和亮度滑块。失败报告具体错误并允许后续重试；不通过隐藏整页、移除控件、宣告虚假硬件能力或返回假成功来掩盖未接通的链路。硬件事务未退休时仍不复用 DMA/FIFO。

## 主机构建

完整 Darwin24 内核、Pong 静态驱动、配套 runtime-services88 和 8 核启动镜像已编译。保留 services87 的单设备 Metal/独立 command queue，未重新引入第二个 Vulkan device。用户维护的构建脚本本轮未改动，Data/SystemOS 不重置。本机 OS 差量 56MiB。没有运行测试、基准、镜像哈希、镜像审计或设备镜像回读比较；编译和 Fastboot 成功不表示外设已可用。

## 安装与设备交接

56MiB OS 差量已收到三段设备写入完成回执，安装状态 complete；Data/SystemOS 保留，pending 已清除。临时启动镜像 `out/ipados18/boot-services88/pong-xnu-initial.img`：Fastboot Sending 2.341s、Booting 10.177s 均为 OKAY。未写永久 boot 分区，设备留在运行状态。尚未获得本轮桌面、亮度、Wi-Fi、按键或实际声音的用户反馈，不将交接成功当成功能成功。
