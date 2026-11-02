# services125：唤醒后 scanout 资源修复与原生 USB 缺口

目标：Pong SM8475 / A730，iPadOS 18.7.10 / 22H374。

## 真机证据

services124 的 REC pmsg/console 已确认 `actual framebuffer handoff available`、`real GPU scanout worker ready` 和真实 `GPU frame completed`，因此 services124 的 GPU scanout 已实际执行。用户报告熄屏再亮屏闪屏。22:28:26 清空显示层，随后电源键唤醒；22:28:28 三次 `scanout copy not submitted ... status=0x3`（presenter 的 kNoResource），旧 worker 永久退出。后续驱动回到 CPU 拷贝。该资源/映射生命周期问题与唤醒切换时间一致；它不是 GPU 没初始化的证据。是否还有实际 DPU 扫描/背光时序问题，要用下一轮实机观察确认。

## 显示修改

- 内核仅保留三个 compositor source 的子描述符、prepare 引用和用户映射，以原始 descriptor 身份 + offset + bytes 匹配。owner 被真实 retain，避免指针回收后命中旧缓存。
- 私有 scanout wire v2 仍为 48 字节，最后一个原 reserved word 改为非零递增 source generation。不是 NativeAdreno ABI2：GPU BO/submit ABI1 及 services122 的稳定 wired 分配模型不变。
- Metal presenter 接收 generation，实际使用已有三槽 buffer/texture 缓存。原实现传入默认 0，缓存从未命中，而且内核每帧销毁 source 映射，无法安全跨帧复用。
- 只在真实完成或完全未提交的帧后回收/驱逐，未知 DMA 完成仍保留 backing 并隔离。断开连接在无在途 DMA 时清理缓存，VM complete 失败也不伪造成功。
- 未提交的资源失败释放不用的 user import、有限速地等待重试，不再三次失败就永久退出 worker。真实已提交 GPU fault 仍停下并保留内存；CPU/GPU 不并行写同一未知 DMA 的 framebuffer。
- 保留 120 Hz 模式、亮度、Data、Wi-Fi、触摸、现有音频/Metal 接口和自动重启关闭的策略。

## USB：已确定缺失的真实接入层，尚未完成

当前内核 prelink 没有 IOUSBDeviceFamily/AppleUSBDeviceMux，也没有板级 DWC3 device-controller backend。因此不能仅靠开启 usbd/lockdownd 或添加一个同名 IOService 实现 Finder/原生配对。

已下载并审阅官方目录列出的 [NativeDwc3](https://github.com/Lux12138/NativeDwc3)，commit `895f7cef1c8904f5f25633f76a254e09849fe07c`。README 比源码落后：源码已包含 EP0/CDC-ACM 实验，但仍是 SM8150/nabu 的 PHY/时钟/RPMh/PMIC 实现，没有 Apple IOUSBDeviceController backend。不能把 CDC 串口枚举当作苹果原生 USB 功能。

本轮从 Pong REC 读取实际 DT，记录到 `configs/pong-usb-sm8475.json`：

- DWC3 0x0a600000 / 0xd93c，SPI133 / GIC165，Apps-SMMU SID **0**；不是 nabu SID0x140。
- Pong 是 `qcom,usb-snps-eusb2-phy`，base **0x088e3000** / 0x154，另有 EUD 寄存器 0x088e2000 / 4。
- 必须驱动真实 I2C0x994000 上 0x4f 的 eUSB2 repeater。不能把 nabu 的 0x088e2000 FEMTO PHY 序列写到 Pong 的 EUD 寄存器。
- QMP DP-combo PHY 0x088e8000 / 0x3000，实际供应/时钟/复位/PHY sequence 也来自本机 DT。

22H374 原始 kernelcache 包含 IOUSBDeviceFamily、AppleUSBDeviceMux、AppleUSBDeviceNCM、AppleS5L8960XUSB 等。生成的原生依赖/接口清单仅保存在 ignored `out/ipados18/usb-services125/22H374-kext-interfaces.json`。AppleS5L8960XUSB 是 Apple SoC 控制器，不能用 Pong 地址冒充兼容。原生 USB 后续仍需完成 SM8475 eUSB2/repeater 供电与角色、DWC3 endpoints/events、独占 SID0 DMA 生命周期和匹配的 IOUSBDeviceController backend，再接入原生设备族/Mux/配对。此次未发布虚假 USB 服务，也未执行错误 SoC MMIO/复位。

## 构建与部署

内核和匹配 Metal plugin 已编译/链接。只替换 plugin，保留已有 Rust/Turnip ABI1 组合；OS 因已被 guest 修改，仍用完整逻辑写入，Data/SystemOS 不重置。不运行镜像哈希、镜像回读比较或性能测试。临时启动完成后补记实际结果，USB 不能记为已修复。

实际部署结果：OS 完整安装 receipt state=complete；Data/SystemOS 保留。services125 Fastboot 临时启动 Sending 2.333 s、Booting 10.129 s 均 OKAY，记录 out/ipados18/fastboot-services125.log。设备保持运行、自动重启关闭。唤醒后闪屏效果待设备观察；原生 USB 仍未接通，不能算修复完成。
