# 接口、服务与构建依赖审计

范围为当前 Pong / SM8475、22H374 / Darwin24 源码和构建路径。本轮没有连接手机、读取 REC 或既有启动日志，没有安装系统或临时启动。以下结论来自源码、编译和链接符号；不代表新的实机功能结果。

## 已修复

| 问题 | 修补与连接位置 |
| --- | --- |
| ADSP/电池/音频的启动调用被 `XNU_PONG_WIFI` 包住 | 新增 `CONFIG_XNU_PONG_POWER`，在 `ArmDTPlatform` 中独立启动，并由构建器传入对应宏和归档 |
| 音频引用的 RPMh 与 SMMU lease 锁定义在 Wi-Fi 对象中 | 移到 `drivers/PongPower/PongPlatformResources`；保留 Pong active TCS4，锁初始化使用原子发布；GPU TCS3不变 |
| Wi-Fi/音频所需 native4K 函数只随 GPU 编译 | 建立唯一 `libPongSmmu4K.a`，由三个使用者共享；Darwin24 GPU归档不再重复包含这些对象 |
| 音频 SID1801 仍在 TCR2/TCR 之前写 TTBR，且使用旧故障位掩码 | 使用 `ArmSmmu4KProgramDisabledContext`，检查禁用状态、写序、回读与TLBSYNC，成功后才连接SID；FSR改为 `0xc00001fe` |
| 网络 ioctl、扫描完成与缓存回调并发修改请求状态 | 使用实际已分配的 mutex；检查短请求、SSID/密钥长度和timer分配失败；命令提交后通知worker |
| 按键转发的 ready=true 可在 SpringBoard 退出后保留 | ready记录实际接收进程PID；发送前检查存活；重新注册从现有计数基线开始；退出时释放native/HID按住状态和notify token |
| HAL时间戳读取存在无界seqlock循环 | 使用acquire语义和有界读取，未取得一致快照时返回真实不可运行状态 |
| `pong_adsp-fw` 被解析但目录参数未使用 | 读取实际指定目录的Pong `adsp.mbn`，默认仍为 `/pong/firmware`，拒绝相对路径和截断 |
| 构建器忽略部分主机配置，隐式使用绝对路径和旧ath11k归档 | 统一读取忽略的 `local.toml`；ath11k使用当前tree输出；继承所选Xcode；Apple源码覆盖也进入实际构建器 |
| 公开依赖缺少Linux、无线shim、RPMh和存储头文件的获取入口 | `configs/pong-native-sources.toml`固定公开来源；`fetch-pong-native-sources.py`获取它们和Apple开源feeds；不获取私有固件 |
| 组件记录将实际 `PongAudio` 类写成 `PongAudioService` | 修正 `runtime_identifiers`，保持HAL匹配类、`IOUserClientClass`和真实元类一致 |
| 打包器无条件启用未必编译的音频/Wi-Fi启动参数 | 根据内核构建记录中的开关生成参数；无效依赖组合在构建入口报错 |

## 当前连接关系

| 链路 | 实际路径 |
| --- | --- |
| 平台与CPU | `ArmDTPlatform::configure` → `pong_cpu_start` →独立电源/Wi-Fi启动入口 |
| Wi-Fi | Pong PCIe0/SID1c01/MSI → `nw_wifi_start/step` → Linux ath11k/MHI/QRTR/QMI → station callbacks → ifnet/Apple80211 ioctl →保留的原生wifid/configd |
| DSP与音频 | `pong_power_start_optional` → PAS1/GLINK `adsp_apps` → APM/TFA9873/独立SID1801 → `PongAudio` / `PongAudioUserClient` →现代 `PongAudio.driver` → audiomxd |
| 音频路由服务 | init在launchd前准备原生AirPlay feature override；原始服务定义、RPC和实际provider保留 |
| 输入 | Pong PMIC只读轮询 → `/dev/pong-buttons` 24字节记录 → backboardd Consumer HID；音量桥接只调用检查过ABI的真实 `SBVolumeControl` |
| 背光 | 真实 framebuffer → `pong_backlight_start` →独立workloop和Pong DCS接口；构建不再隐式依赖无线开关 |

元类延迟注册入口、HAL factory UUID、内存类型0/1、输出start/stop selector以及19.2MHz host clock调整均继续使用同一组22H374接口。Pong不公开capture端点。

还核对了选定XNU的 `Call_continuation`：回调返回后会跳转到 `thread_terminate`。因此没有把音频/ADSP worker的正常返回误判为缺少手工线程退出，也没有添加不必要的线程终止包装。

## 主机结果与限制

格式2解析、DTS编译、Darwin24内核链接和用户态init/UI/HID/HAL编译已完成。主机符号核对确认电源、无线、RPMh、SMMU、音频、背光及按键入口均为实际定义，最终原生内核没有未解析符号。SMMU helper只有一个链接提供者。

本轮没有运行测试程序、基准、镜像哈希或镜像回读。也没有更新本机OS镜像或手机。修改后的驱动和用户态需要配套部署；仅替换内核不会安装新的按键转发库。

Wi-Fi运行时尚不能由静态审计确认：QMI/BDF选择、固件启动、MSI、扫描/认证和wifid实际请求仍需之后的设备结果。当前网络入口实现的是ifnet/ioctl路径，未提供Apple私有无线用户客户端；不支持的请求保留明确错误。PCM发声、独立电源键动作、系统挂起恢复和全部物理内存同样没有新增结论。没有通过成功占位服务掩盖这些边界。
