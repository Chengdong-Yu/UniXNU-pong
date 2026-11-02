# Pong 外设接口重构记录

## 来源和设备证据

从 GitHub 执行 fetch 并快进检查后，两个仓库均已处于远程 main 的当前版本：

- UniXNU：`40d21cc65171c3a24faa130a2a8e899f85790485`。
- UniXNU-sheng：`08cde20edc3254ed6f7df8d0f5384fb887786e8f`。

已逐篇阅读 UniXNU `docs/zh/porting/display.md`、`network.md`、`power.md`、
`input.md`，并对照其中的板级实现。网页读取工具不可访问站点，本轮阅读的是上述
GitHub 官方仓库中的对应页面源码。保留这两个工作区现有本地改动和 Pong 用户更新的脚本。

services82 的 REC 记录保存在本机 `out/ipados18/logs-services82`：

- 背光收到 `rx=40000 data=2120003b`：四字节 DCS 回包，类型 `0x21`，
  仅一个亮度数据字节，旧解析器只接受 `0x22` 双字节。
- QCA6490 的 PCIe、SMMU、MHI/QMI 已接通，固件登记 102 个通道；首次创建 station
  后进入 RDDM。没有扫描或联网成功记录。
- 实体音量输入已到达 SpringBoard，并实际写入 PCM 共享增益。日志末尾是持续按住
  状态，不能仅据此判定硬件卡键，因为手动回 REC 也会按住实体键。
- TFA9873 双功放和 DSP/APM 服务已经发布，CoreAudio HAL 已连接，但原生路由管理器
  尚未接纳设备，未观察到 StartIO/PCM 播放。
- 电池已发布真实 `100% / 4408mV`。同一设备回到 REC 后原厂驱动读数为
  `100% / 4458mV`，证明当前有实际协议读数；两者不是同步采样。

## 接口对应关系

| 功能 | 硬件/传输层 | 消费者和 OS 接口 | 本轮修改 |
| --- | --- | --- | --- |
| 显示 | Pong GOP 1080×2412、stride4320；A730/SMMU | 22H374 MiniFramebuffer、配套 Metal scanout ABI2 | 复用已工作画面与帧完成所有权；没有替换成 SM8550 DPU 地址 |
| 亮度 | NT37705、SM8475 DSI0 `0xae94000` | 工作循环、AppleARMBacklight 亮度属性门面 | 分开执行最大回包长度设置与 BTA GET，支持单/双字节真实回读，按测量精度比较写入结果 |
| Wi-Fi | Pong PCIe0、QCA6490、SID1c01、SPI140 | Ath11k/MHI/QMI、共享 station、Apple80211/ifnet | 在 station 创建前检查身份；缺少有效固件 MAC 时使用安装身份产生的本地管理地址；公布信道与实际 2.4/5GHz 扫描策略一致 |
| 实体键 | PMK8350 PON/RESIN、PM8350 GPIO6、SPMI v7 | 内核事件队列、既有消费类 HID 与 22H374 音量桥 | 双边沿中断唤醒，工作线程读取/应答；去抖重读、失败释放和持续挂起限流回退 |
| 音频 | Pong ADSP/GPR/APM、TFA9873、tertiary MI2S、SID1801 | PongAudio 用户客户端、AudioServerPlugIn、audiomxd | 修正无输入设备的输入首选声道返回；初始化返回后通知 HAL 重新枚举；音量读回使用实际共享增益/静音 |
| 电池 | Nothing 单电池 PMIC GLINK `PMIC_RTR_ADSP_APPS` | 有界接收队列、单请求状态机、IOPMPowerSource | 严格检查通知类型/长度和温度范围；关键读数完整后发布；断线标记过期而不虚报电池被拔出 |

## 板级约束

按键中断取值已核对本机 Android DT：SPMI `interrupts-extended` 为 PDC pin1、level high，
PDC 范围把该 pin 映射到 SPI481；PON bit7 是电源，bit6 是音量减，GPIO6 为音量加。
只在 APID 的 EE 和 IRQ 所有权均为当前环境时配置该按键外设，不清除其他 APID 的中断。
处理函数只屏蔽 PDC 并唤醒线程，SPMI 读写和事件队列操作位于线程侧。64ms 重读覆盖去抖与
丢失边沿；中断持续挂起或前提不满足时保留有界读取路径。

无线固件继续使用 Pong 活动 B 槽 `modem_b/image/qca6490` 的本机提取物。
本地管理地址明确是软件分配的地址，不冒充工厂 MAC；有合法固件地址时优先使用原地址。
没有有效地址来源时返回错误，不创建全零地址 station。该修补是否解决 RDDM 尚需设备日志。
保留实际 DMA 同步、MSI、RX 复制和崩溃后隔离；没有发布虚假的联网结果。

音频复用真实 DSP graph 和功放配置，不增加不存在的输入端口。首选声道和布局原本已有实现，
本轮修正其 input scope 与实际输出专用设备不一致的问题，不能把它表述成此前完全缺少布局。
默认输出选择仍由 audiomxd 内的插件对真实设备 UID 操作。实际路由接纳和发声仍需确认。

电池保持充电控制由固件管理，移除未使用的充电限值配置入口。GLINK 断线后取消定时器、
丢弃旧接收队列并清理协议状态；保留最后一次真实快照并设置过期属性，重连后重新订阅和刷新。
新增工作循环资源清理，初始化失败时释放已经创建的事件源和锁。

## 构建和安装范围

完整 Darwin24 内核、原生驱动归档、运行时/音频 HAL 和临时启动镜像均编译完成。
配套 Metal 继续使用 services82 ABI2，因本轮未改帧交换 ABI，OS 中保留对应前端。
本轮未修改用户维护的构建脚本，未运行测试、性能基准、镜像哈希或镜像回读比较。

本机 `out/ipados18/build-services83-final.log`、`build-runtime-services83.log` 和
`package-services83.log` 保存编译结果。安装与临时启动结果随后写入本机回执及 `porting.toml`。
编译成功不表示扫描、亮度滑块、音频播放和按键长按已经实机成功。

已完成 80MiB OS 更新，4 个写入范围均收到设备完成回执。安装记录位于本机
`out/ipados18/install-services83/installation.json`；现有 Data/SystemOS 保留。

services83 已临时启动：Sending 100112KB（2.339秒）与 Booting（10.170秒）均 OKAY。
输出保存在本机 `out/ipados18/boot-services83/fastboot.log`。未写永久启动分区，启动后
留在设备上等待用户观察。上述结果只证明镜像已交接，不能证明外设功能已完成。
