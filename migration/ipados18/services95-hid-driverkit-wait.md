# services95：Pong 音量键匹配等待修补

## 本轮日志

从已连接 REC 重新提取 pstore，保留在忽略目录 logs-services94-rec-current。本轮仍为 services94：没有内核 panic、GPU BV 故障或纹理 OutOfMemory；自动同步及 REC reset commit 标记存在。QCA6490 完成真实固件初始化，发布 en0 并扫描 19 个 BSS。系统仍只有初始 GET，没有设置端扫描/关联 SET，不能推定系统联网已经修好。audiomxd 内 ASPL 连接实际立体声服务，但没有 StartIO。

日志还显示 Pong 音量键创建 IOHIDUserDevice 后，先尝试 AppleUserHIDEventDriver 并等待 com.apple.driverkit.AppleUserHIDDrivers。约 120 秒的用户服务超时后，内核 IOHIDEventDriver 才打开同一个报告设备。设备创建成功与输入驱动及时就绪是两个不同阶段；前面的注册记录没有排除这段延迟。

## 修补边界

保留 22H374 原有 HID 栈和所有通用匹配规则。新增板级 personality，只匹配 Transport=SPMI、Manufacturer=Pong、Product=Pong PMIC volume keys、Keyboard Usage 1/6 的现有报告设备，以不延迟的内核 IOHIDEventDriver 处理真实 Consumer E9/EA 报告。没有全局停用 DriverKit、删除系统音量控件、恢复自制增益/HUD 或添加虚拟硬件能力。

匹配定义在 configs/pong-hid-match.json，经本项目新增的独立 peripheral-boot 入口加入选定 IOHIDFamily 的元数据。用户维护的基础构建脚本与通用 NativePrelink 工具未改；Apple 原始二进制和元数据不提交。

## 构建与交接

使用已完成的 services94 Darwin24 原生内核重新预链接并构建 services95 启动镜像，保留相同已安装 runtime、可写 Data、亮度、八核和 Pong 无线固件。此次匹配更改不需要再次写入 OS/Data。没有运行测试、基准、镜像哈希或镜像回读比较。

Fastboot 临时启动完成：Sending 2.319s、Booting 8.535s 均为 OKAY，未写入永久 boot 分区。已启动一次有界 180 秒自动 REC 捕获，沿用 /sys/fs/pstore，目标记录为 out/ipados18/logs-services95；交接时捕获状态为 waiting-for-timed-return。匹配延迟是否消失、系统音量提示与 Wi-Fi 设置是否可用仍需本轮设备结果，不能以 Fastboot 成功代替。
