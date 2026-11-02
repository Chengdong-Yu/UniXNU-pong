# services138：iPhone USB 身份与保留的 CDC

按用户要求在 `pong_usb_iphone=1` 下使用 `Apple Inc.` / `iPhone`、正常模式 VID/PID 05ac:12a8 和 class0。真实链路仍为已实现的 USB2 High Speed，不虚报 USB3、PTP、NCM 或额外硬件端点；保留 ff/fe/02 Mux 及 CDC IAD。电池供电板型如实声明 self-powered，GET_STATUS 与描述符一致，不改变实际充电策略或电流限制。

产品型号继续沿用当前 iOS 的 iPhone17,1 / iPhone16Pro / MYMC3LL/A 配置，不替换 Pong 的 SM8475 驱动或硬件寄存器。USB 的 iSerial 优先使用当前系统真实 `MGCopyAnswer(UniqueDeviceID)`，使它与 lockdownd 的身份保持一致，不用另一台 iPhone 的 UDID/配对记录。查询在独立线程，最多等750ms；查询失败保留现有 board serial 和工作通道。身份只在实际 USB attach 之前提交，在线不能更换序列号。

CDC 控制接口保留独立 `Pong XNU console` interface string。电脑工具根据实际 USB 节点/该接口查找串口，而不是假定设备名包含 PONG，因此切换原生 UDID 后仍能抓日志、`reboot rec`、`reboot fastboot`。旧串口名称和显式 `--port` 继续支持。

services137 的实机日志已确认 Pong SD1 修复后的两颗 TFA9873：真实非零 PCM 后 status11=e040、status14=14f，即 AMPS/SWS 均出现；此前为8040/4f。尚未以主观听感作出已出声承诺。

原生 macOS 实际发送 SETUP code5，libimobiledevice 发送7。桥接程序现接受这两种基本 v2 setup，其他值仍返回真实失败；保留 FACEFACE device→host、逐帧 v1/v2 检测、native fixed-header TCP、ACK|PSH 和有界 socket/队列。没有替换 stock lockdownd 或假配对。

构建、安装和实机连接结果分开补记。Data/SystemOS、现有音频/HID/亮度/WLAN/Metal、rootful 包状态均保留。该模式是 Pong 的兼容身份配置，不表示物理设备变成 Apple SoC；额外 CDC 接口意味着描述符不会与原机逐字节相同。
