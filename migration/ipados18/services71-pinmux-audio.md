# services71：Pong 引脚所有权与 Darwin24 音频链路审计

## 实机证据

读取 services70 REC pstore（console 852978 字节，pmsg 2876 字节）；selected Data 诊断只读取约 2.16 MiB，未进行镜像哈希或镜像回读校验。

- PCIe 两次训练停在 Detect，ELBI=1、DLL 不活动；GPIO204 配置为 0xa01，EGPIO_PRESENT=1、EGPIO_ENABLE=0。六路 WLAN PMIC 供电物理就绪，不能把这次失败归因于请求未上电。
- APM ready、TFA9873 0x34/0x35 revision=0xb73、SID1801 DMA、HAL factory/Initialize/connect 均成功；没有 StartIO/PCM 记录。audiomxd 约 51 秒后自行 SIGKILL，未找到同轮新的崩溃 ips。
- 两个物理音量键均有 PMIC down/up，Consumer HID dispatch sent=1。事件送达不等于系统音量路由已生效，不宣称音量/音频已修复。

## 已修正

1. 对照 Nothing 原厂 pinctrl-msm.c（c402fcdb3a1523d10b15ef1123bced45456f7592），在 PRESENT 位为真时置 EGPIO_ENABLE，并保留 GPIO_CTL 上位字段。应用于 WLAN XO 和音频 reset/I2C/MI2S pinmux。不是套用 Sheng 的 SM8550 GPIO。
2. HAL IsActive 接受真实 0/1 状态，状态改变发送属性通知；停用时输出静音。
3. 物理服务发布时建立稳定时钟锚点，idle 时也支持 HAL 时钟查询；PCM 不再把立即播放的样本标成未来。
4. DSP 命令等待期间的 WRITE_DONE 归还正确的 DMA slot；stop 先停止新提交，再等待图停止。校验地址、slot token，保持真实失败传播。
5. 对齐 UniXNU 当前播放策略：共享环由 16384 扩为 65536 帧，最小提前量 300ms；已消费样本清零，防止旧声音循环；用主机时间和 DSP 稳态提前量进行有界漂移修正。
6. HAL 精确检查实际 Float32 stereo/48k 格式，避免接受驱动无法解码的格式；属性大小查询中的转换操作使用明确初值，消除未初始化栈读取。不发布虚构麦克风。
7. 构建默认不再把 pong_audio=2 改成 0。当前镜像仍明确指定音频启用。未实现的 Darwin24 属性只记录有限次错误，保持原始错误返回。

## 已逐项核对的其余链路

- Pong live DT 的 MI2S active pinctrl 为 121/122/123/124，function1、8mA、bias-disable；I2C=48/49，reset=1/89；保留真实三级 LPAIF RX 端点和 TFA CNT profile。
- 系统原始 VirtualAudio 位于 /Library/Audio/Plug-Ins/HAL，board HAL view 只覆盖 /System/Library/Audio/Plug-Ins/HAL，未遮掉它。HAL .driver/MH_BUNDLE、ASPL UUID、Darwin24 target、4KiB 签名和内核映射 ABI 已接入。
- Wi-Fi 使用 Pong PCIe0、SPI140、SID1c01、QCA6490 hw2 固件选择，先 link/identity/BAR，再隔离与 MSI，再 MHI/QMI；事件先注册再启动，无虚构 MAC/网络成功。
- 核对 Consumer keyboard usage 0x0c:e9/ea 和按下/释放；原始 power 路由保留。未默认启用会吞掉 HID 的直接通知桥。
- Wi-Fi、电源归档和初始化调用存在，native source 均参与构建；GPU/显示和现有 Data 不做额外改动。

## 构建与设备记录

待本轮编译、OS 更新及一次临时启动完成后补记。静态审计不能保证所有硬件问题已消失；扫描、实际播放和系统音量仍须本轮运行确认。

编译完成：Darwin24 format2 内核链接成功；PongAudio.driver 与系统侧运行时编译成功。HAL 仅更新 OS 内的 board view，新旧 Data/SystemOS 保留。内核构建的已有平台测试汇编对齐 warning 和 AppleFeatures 目录 warning 未作为硬件成功证据。尚未临时启动。

交付记录：OS 已写入 6845104128 字节并完成同步，安装收据 complete / os_generation_complete=true。Data、SystemOS 和启动分区未写入。实际临时启动路径为 /Volumes/Untitled/Pong-iPadOS18/boot-services71/pong-xnu-initial.img（102514688 字节）；Sending 2.320s、Booting 10.176s 均为 OKAY。此结果仅确认 Fastboot 命令完成；本轮 Wi-Fi 扫描、真实 PCM 播放和系统音量响应尚待运行反馈，不将源码修补或服务发布等同于硬件功能成功。
