# services92：外设系统接口和界面延迟

依据 services91 自动 REC 采集的 console/pmsg、Data 诊断文件、22H374 CoreBrightness/IO80211/CoreAudio 和原始 kernelcache 的依赖及人格表。Apple 二进制和拆解结果留在本地忽略目录。

## 实际证据

- QCA6490 完成 Pong 固件初始化和 10 个热点扫描；原生客户端只有版本、能力、物理角色、父接口、电源、SSID 和 WoW GET，尚无系统扫描 SET/关联。
- 音频物理端和 AudioServerPlugIn 已连接，未开始 StartIO。安装中的 audio-route 只是打印“由插件处理”，插件本身却遵守不在服务端回调重入客户端 HAL 的规则；实际选择输出路径因而缺失。
- 音量 PMIC 转换已经送到消费端，但 IOHIDUserDevice 创建失败。旧 prelink 列表没有 IOHIDFamily，也没有标准 Consumer/Keyboard EventDriver 人格，IOHIDResource 没有实现提供方。
- NT37705 服务有真实回读并发布。22H374 的物理亮度写入包含可选 backlight-level 字段；旧 setProperties 只允许两个字段。
- GPU 上一轮未再见 BV opcode 故障，完成约 1184 个 command buffer，但出现纹理 OutOfMemory(Device)。次级客户端只有约 170 MiB IOVA，已有页表中留下大量未分配地址；等待 fence 的调用还会持有所有 GPU 客户端共用的门锁直到 500 ms 超时。日志中的帧率不能视作面板刷新率。

## 修改

1. 增加独立 peripheral boot 入口，调用原有打包脚本并选择同一 22H374 的 IOHIDFamily 和 codeless IOHIDEventDriver。保留用户维护的现有构建脚本；没有引入 Apple 触控控制器或 AppleSPU 板级驱动。按键仍只来自物理 PMIC，交给原生报告解析，不恢复独立 HUD、增益快捷键或自制重复事件。
2. 背光接受并校验第三个数字字段，保留 brightness/FadePeriod 语义，未知属性仍拒绝，实际 DSI 事务、退休和回读保护不变。
3. 独立 audio-route 在 audiomxd 之外发现 UID 为 PongAudioDevice 的真实可用输出，选择 CoreAudio 的普通/系统默认输出并读取结果。有限重试，错误写入原有日志；没有虚构输入设备或假定已经出声。
4. 在既有 32 位 GPU 上下文中，将次级 IOVA 扩为每个 512 MiB，所有窗口不重叠且在已发布的 2.5 GiB 用户范围内；未扩大物理连续池或跳过页面/所有权检查。
5. fence 等待改成锁内非阻塞轮询、锁外睡眠；每客户端操作锁仍在，真实完成后才退休。保留 BO 映射前必须完成既有作业的约束。
6. Wi-Fi socket ioctl 在锁外 copyin/copyout，仅持锁处理驱动状态；保留原生用户客户端、实际扫描/关联、Pong 固件和上一轮锁外事件投递。

## 交接和限制

内核/驱动和配套 runtime 编译完成，OS 差量更新保留设备现有 Data/SystemOS。8 核、X2 空闲核排序、GPU ring 同步、真实电池订阅和自动 REC 抓取沿用。未运行测试、性能基准、镜像哈希或设备镜像比较。是否能调亮度、显示系统热点、响应系统音量和播放 PCM，需要本轮实机结果，不能以编译或服务发布代替。

## services92 实机反馈

75,497,472 字节 OS 差量安装完成，Sending 2.326s、Booting 10.170s 均 OKAY。180 秒后完成同步并自动返回 REC，console/pmsg ID 与提交标记一致。用户确认亮度调节可用；Wi-Fi 仍未修复。设备日志首次出现 volume report device=native，并由 IOHIDEventDriver 打开。该轮无旧 BV 故障或纹理 OutOfMemory，但声音仍无 StartIO，外部路由尚未发现物理 UID；不能宣称已出声或系统音量已可用。

新亮度日志中所有有效长回复都为写入值的四倍，下一版将归一 GET 量程、保留 SET 数值，避免把成功写入误判成拒绝并反复切换 LP 模式。Wi-Fi 客户端反复找不到 com.apple.wifi.manager；已从同一 wifid 确认 WiFiServerCreate 与 bootstrap_check_in，下一版将该端口注册到真实 wifid 作业。
