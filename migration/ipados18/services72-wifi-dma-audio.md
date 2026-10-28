# services72：Wi-Fi DMA、Darwin24 音频与音量键

## REC 日志证据

services71 的 pstore 保存于本机 `audit-services71/pstore/`。选择性读取约 2.63 MiB Data 诊断文件；未读取镜像进行比对、哈希或校验。

- 六路 WLAN 供电物理就绪；QCA6490 PCIe0 link 和 BAR 已建立，bus master 尚未开启时发生 `WiFi DMA allocation failed`。这次实际停点已经越过 link training，不继续改动已成功的 PHY/引脚时序。
- 原申请包含 128 MiB 物理连续 DMA arena。失败后没有发布网络接口，日志无可用 station 或扫描成功证据。设置中 Wi-Fi 界面等待是否完全由此导致，仍需运行确认。
- DSP APM、两路 TFA9873（0x34/0x35，revision 0xb73）、SID1801 DMA、物理输出服务和 audiomxd 中的 HAL Initialize/connect 均成功。尚无 StartIO、PCM 消费记录，不能据此宣称有声音。
- PMIC 音量按键已经产生事件，Consumer HID dispatch 返回 sent=1，用户仍反馈无反应。电源键路径正常。

## 实际修改

1. Wi-Fi arena 使用有线普通物理页和连续 CPU 虚拟区，保持写合并 CPU 属性；不再请求 128 MiB 物理连续区。按真实 4 KiB 页取得 PA、检查 36 位输出范围并合并相邻 span，通过专用 SID1c01 映射为原有连续 IOVA。MSI landing page 和 SMMU table pool 继续保持小块物理连续。
2. 同属性 SMMU 范围不会产生缓存/共享性/特权属性冲突，预先识别该条件使碎片化内存的别名检查为线性。混合属性范围保留原检查；未修改 PTE、页尺寸、权限和激活顺序。
3. native4K 建表、context 和 SID 安装成功之前保持 endpoint bus master 关闭；失败记录原因并继续正常启动，已有 DMA 存储保留至硬件复位。没有发布虚构 MAC、接口或 Wi-Fi 成功状态。
4. 从本机原始 22H374 CoreAudio 确认 `siso` 是 SupportsIsolatedIO，`drte` 是 WantsDisplayRouting，两者数据大小均为 UInt32。Pong 实现共享扬声器输出、不实现隔离 IO，也不是显示器音频端点，明确返回相应不支持能力；未把未知属性统一返回成功。
5. 默认启用已有 22H374 `SBVolumeControl` 适配，只有已有对象、ivar 和 increase/decrease 方法 ABI 检查通过才接收物理音量事件。按键桥只走一个音量路径，电源保留 HID。可用 `pong_volume_direct=0` 选择原 HID 路径。该修改仍需确认实际音量路由和声音输出。

## 其余接口审计

- 对照 Nothing live DT 保留 PCIe0、PERST94、CLKREQ95、WAKE96、WLAN_EN80、XO204、SPI140 和 SID1c01；services71 的 EGPIO 申请已使 link 成功，不采用 Sheng 的 SM8550 地址。
- nw_abi 的 scan/connect/disconnect/TX 本身已经排队，由单一 worker 执行，不在设置 ioctl 里运行 MHI/QMI 长等待；没有再套一层可能锁死的同步队列。
- 表、arena 和 MSI 的 IOVA 范围不重叠；arena suballocator 保留原连续 IOVA、对齐与范围检查。全部页先校验再安装映射。
- 保留与 Pong live DT 相符的 MI2S 121/122/123/124、I2C48/49、功放 reset1/89，以及真实输出就绪门槛、48k stereo 格式、19.2MHz host clock 修补。未新增虚构输入设备，也不把功放识别当作 PCM 播放成功。
- Native Wifi 和 Power 档案均进入本轮 Darwin24 内核链接；HAL 使用现代 .driver/MH_BUNDLE。未导入 Hackios 实现。

## 构建和启动

Darwin24 format2 内核和系统运行时已编译完成。下列安装、临时启动状态待实际执行后补记；功能状态保持未确认。

OS 更新已提交 6845104128 字节并同步，收据 state=complete、os_generation_complete=true。Data 与 SystemOS 保留；启动镜像大小 102514688 字节。即将执行一次 fastboot boot，尚不宣称 Wi-Fi 扫描、实际声音或音量响应成功。

已临时启动 services72：Sending 2.330s、Booting 10.180s 均 OKAY，总计 15.129s。启动镜像为 `/Volumes/Untitled/Pong-iPadOS18/boot-services72/pong-xnu-initial.img`；未刷写启动分区。此后停止设备操作，等待本轮实际 Wi-Fi、声音与音量键反馈。Fastboot 成功不等于功能已确认。
