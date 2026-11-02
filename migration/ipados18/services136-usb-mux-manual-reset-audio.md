# services136：USB 原生服务通道、串口重启与播放端点

## 当前证据

services135 REC 保留完整 console/pmsg。CDC 已在 macOS 实际枚举并读取日志，lockdownd 为真实 stock 22H374 进程。音频有原生 Speaker 打开、非零 committed PCM 和 DSP WRITE_DONE，两个 TFA9873 的 PLL/reference 已就绪，但没有 AMPS/SWS 输出状态；尚无可听输出证据。

## USB

- CDC 接收不再丢弃。仅接受 ASCII 整行 `help`、`reboot rec`（或 `reboot recovery`）、`reboot fastboot`，过长/包含控制字符的整行丢弃。按真实 TRB residual 取数据，SHORT/IOC/LST 不误当失败。
- 显式重启单独工作线程执行，500ms 留给 ACK；使用 Pong PMK8350 SDAM restart cell 与 IMEM 标记，再调用启用文件系统同步的 `reboot_kernel(RB_QUICK)`。失败恢复原标记；没有开机定时器或无条件重启。
- `scripts/pong-usb-command.py rec|fastboot|help` 提供电脑端固定命令工具。原日志捕获工具仍然只读日志，不发送重启命令。
- `pong_usb_mux=1` 增加 Mux bulk endpoints，与 CDC 使用不同 TRB、DMA buffer 和 interface。USB 32KiB lease/SID0、Pong eUSB2/NXP 电源路径保留，未借用音频或 UFS DMA。
- `PongUsbMuxTransport` 是实际 DWC3 字节通道，root 独占 user client、有界队列和断连 generation。没有伪造 IOUSBDeviceController/IOUSBDeviceFamily 类或服务。
- 配套 `/pong/usb-mux` 实现公开 USB Mux v1/v2 和受限连接状态机，转发到真实 iOS loopback 服务；stock lockdownd 继续负责配对、信任、TLS 和服务启动，不返回假设备/配对响应。每个 USB IN frame 保留边界，整包时发送真实 ZLP。
- 只有实际 root bridge 已打开时才枚举 Mux，避免主机 VERSION 超时；60 秒仍无 bridge 则保留真实 CDC，不阻塞系统启动。
- 原生主机发现兼容 function 使用 05ac:12a8 协议标识，实际产品字符串仍为 Pong iOS USB + console。此标识不表示硬件是 Apple SoC，也不表示 Finder/配对已验证。
- 新 launchd job 为 root / RunAtLoad / LaunchOnlyOnce，同时写入 canonical plist 和实际 launchd cache。没有添加不存在的 stock usbd/usbmuxd 作业。

## 音频

Pong 的 MI2S-LPAIF-RX-TERTIARY 硬件端点与其播放流采用相同 RX 方向，硬件端点使用 device container，移除继承自 Sheng 的 TX/stream 放置。保留真实 48kHz、stereo Q31、3.072MHz PRM、TFA9873 本机 CNT、保护和原生 IOAudio2；不写 MTP/OTP，不制造输入端点或关闭保护。服务类注册移至可选 worker 启动之前，USB 不再依赖音频硬件成功，但音频服务仍仅在真实 route 成功后发布。

## 部署与结果

既有 services135 codecs、HAL、全局 Metal、亮度、WLAN、HID 和 rootful 包状态保留。只更新 OS 中的 USB bridge/job，Data/SystemOS 不清空。构建与临时启动结果单独补记；编译通过不等于 Finder 配对成功或扬声器出声。
