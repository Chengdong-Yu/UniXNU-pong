# services86：亮度、station 设置接口、音量与帧交接

## services85 实机证据

- REC console 已保存到本机 `out/ipados18/logs-services85/pstore`。Backlight 先读到 DBV 32，首次写入后读数仍是 32，原处理随后永久停止接受亮度请求。
- Pong QCA6490 已完成 PCIe、SID1c01 native4K DMA、MHI/QMI/WMI、首次实际扫描，返回 11 个 BSS。固件仍来自 active modem_b 的 Pong 提取目录。后续 OS 只查询电源/接口信息，没有实际扫描或关联请求，不能把它归为 PCIe training 失败。
- AudioServerPlugIn 已连接实体 TFA9873 输出服务；尚未出现 StartIO，不能宣称系统声音已恢复。实体音量键转换已抵达 SpringBoard，并改变实际 PCM gain，但原生路由/HUD 尚不对应。
- 电池初次快照显示 full capacity、USB online=0。旧代码仅看 online 且允许不同周期读数混合，刷新周期 60 秒。
- 显示提交的等待主要在 driver/scanout 交接；不以桌面静止时的低帧数当作性能测量。

## 实际更改

1. DCS 写入优先采用原厂支持的 LP 路径，DMA 退休后留 20ms 面板锁存时间；回读不符时切换模式有界重试。仍不符则发布真实回读/错误并允许后续滑块操作。DMA 未退休或传输错误仍停止，不能复位/复用未知归属的 FIFO。保留 services85 的 22H374 空曲线初始化修补，没有伪造面板光度表。
2. 新增 `PongWifiControl.h` 有界 ABI 与真实 IOUserClient：读取 station/BSS 快照、异步扫描、连接、断开。没有实现的 Apple 私有 CWF 方法继续拒绝，没有伪造联网状态。认证过程中不允许刷新扫描覆盖连接。Preferences 的 AirPortController 分配接到 Pong 自有设置控制器；显示真实 BSS、密码框及状态，后台串行队列请求，不在主线程等待固件。不记录或持久化密码。
3. 实体按键仍使用现有 PMIC 采样和 transition 通知。显示实际扬声器 gain 的短暂 HUD，达到上下限后停止重复；只在板级增益接口失败时才使用原生动作，避免双重步进；HAL 把共享 gain/mute 的变化通知给系统属性缓存，记录真实设备发布通知的返回码。工厂交出接口前非等待连接已经存在的真实输出，防止首次 DeviceList 为空；Host clock 调整仍在 Initialize 获取 host 后进行。没有增加假麦克风或在插件内部重入客户端 HAL。
4. 每个 GLINK 周期独立记录 fresh/completed 字段，必须有同轮关键读数才能更新；失败保留上次快照并标记 stale。在线策略为 0 时，只有同轮测得的有效 VBUS 才补充外接电源存在判断。source 变化需要两轮完整读数；5 秒刷新，固件继续独占充电控制。
5. Scanout 使用独立 Nmb/Vulkan device、VA/queue 窗口与独立 cache options，只保留 scanout 和最多三个 compositor 源 buffer/linear views，减少每次 noncoherent 交接维护的资源范围。真实 GPU blit、completed status、2 秒有界 fence 等待和失败后映射保留全部保留；未提交前的失败可交回 CPU 路径。

## 来源与边界

按照本机最新 UniXNU `docs/zh/porting/{display,network,input,power}.md` 和 NothingOSS SM8475 DSI 源码实现。文档网页本轮工具不可达，使用已拉取仓库中的对应文档。没有加入 HackiOS 代码或 Sheng 固件、电气参数。

内核、运行时、Metal 插件与 8 核启动镜像均编译完成。用户维护的构建脚本未修改；Data/SystemOS 保留。没有运行测试、基准、镜像哈希、镜像审计或设备回读比较。硬件效果须在本轮启动后分别观察，不以编译或 Fastboot 成功代替。

## 安装与临时启动

192MiB OS 差量已收到设备写入完成回执；prepared pending 已清除。Data/SystemOS 保留。最终启动镜像为本机 `out/ipados18/boot-services86-final/pong-xnu-initial.img`，配套 runtime-services86 与 plugin-services86。Fastboot 临时启动 Sending 与 Booting 均为 OKAY，实际输出保存在本机 `out/ipados18/fastboot-services86.log`。未刷永久 boot 分区，启动后留在运行状态。亮度、Wi-Fi 关联/DHCP、PCM 出声、充电状态和帧延迟仍需实机反馈。

## 后续实机结果

services86 未恢复桌面：第二 Vulkan device 与当前 Turnip 进程全局连接所有权冲突，backboardd 在初始化时连续 SIGSEGV。该优化已在 services87 撤回；详见 `services87-display-recovery.md`。本页的编译/启动结果不代表此前独立上下文可用。
