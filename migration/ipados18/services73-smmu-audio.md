# services73：SMMU 顺序与本地音频服务

## services72 实机日志

REC console 及 pmsg 共 1126516 字节。本机选择性提取约 3.5 MiB Data 诊断，包含 `stacks+audiomxd-2026-10-27-182231.ips`，没有进行镜像校验或回读比对。

- PCIe link/BAR 成功；128MiB arena 分为2049个物理span，native4K建表67页成功。失败为 context readback，尚未接入SID或启用 endpoint DMA。
- audiomxd 的报告明确为 `RPCTimeout`，请求 `com.apple.coremedia.routediscoverer.xpc` 的 DiscoveryMode。等待链为 MediaExperience discovery manager → CoreMedia remote endpoint manager → airplayd。airplayd 反复 exit(1)，endpointmanager 服务被 launchd 节流。HAL 已加载、实际功放和DSP就绪，但没有 StartIO。
- 真实音量 down/up 到达 SpringBoard 的 SBVolumeControl；这些动作记录不能证明音量已生效。电源 down 到达 Consumer HID；日志末尾为用户重启组合键，没有足够证据证明单独电源按下/释放后的锁屏动作。
- SpringBoard 在35秒、90秒快照中主线程均处于正常事件循环，不能把这次音频超时归因于 UI 主线程锁死或内存不足。

## 修补

1. NothingOSS SM8475 arm_smmu_write_context_bank 明确要求先 TCR2/TCR、后 TTBR，因 TCR2.AS 控制 ASID[15:8] 写入。旧Wi-Fi适配器顺序相反，而ASID=0x1c01，首次写入可丢高位。改用现有 ArmSmmu4KProgramDisabledContext：正确写序、禁用时完整配置检查、有限TLBSYNC、检查通过再启用。没有屏蔽原来的失败或放宽context比对。仅成功后绑定SID并允许endpoint bus master。
2. 同步SMMU真实FSR fault位掩码，出现故障保留实际FAR/FSYNR0并复位endpoint、终止可选worker，正常系统启动继续。没有全局重置APPSSMMU或改动UFS/GPU所有者。
3. 音频已使用22H374支持的现代AudioServerPlugIn .driver，匹配工厂和用户客户端，物理HAL不是本轮崩溃点。静态init在launchd之前选择 `AirPlay/airplayd=false`，使CoreMedia和launchd一致使用audiomxd真实内置endpoint实现，避免等待当前没有可用实现的独立AirPlay服务。原始守护进程和Mach服务定义保留，不伪造XPC答复，不关闭超时处理。
4. 配置在本机生成，只创建不存在的Data覆盖文件，保留既有操作员配置；更新入口同时暂存匹配init与策略。无需清空Data或复制Apple私有配置。
5. 核对按键：Pong PMIC输入、Consumer page0x0c、Power0x30、Volume Increment0xe9/Decrement0xea，与选定系统的Keyboard事件接口一致。保留实际按下/释放、内建标志和既有控制器ABI检查。音量的无响应目前与未完成的音频路由共同存在；未另造假按键成功状态。

## 发布与边界

按官方格式2迁移说明整理四层配置、固定源、板级DTS、社区组件、CI和13项porting记录。AdrenoMetal使用规范源码身份；A730/QCA6490聚合保留PongPlatform24名称。Apple私有二进制、固件、IPSW和生成Data不进入仓库。

本轮用户要求完成后不启动：只编译内核、运行时和启动镜像，未安装到手机或执行Fastboot。SMMU固件启动、Wi-Fi扫描、PCM发声及电源/音量动作仍须后续设备结果确认。

## 主机构建结果

| 步骤 | 结果 |
| --- | --- |
| 格式2解析、组装与板级DTS | 成功，配置为 `pong_ipados18` / `ipados-18.7.10-22H374` |
| Darwin24内核与预链接 | 成功，`kernel/mach.development.pong` 为43834376字节 |
| 用户态运行时 | init、UI兼容库和现代音频HAL编译成功；本机OS暂存匹配init及音频策略 |
| 启动镜像打包 | 成功，`pong-xnu-initial.img` 为102514688字节 |
| 公开源版本配置 | SoC子目录及PongPlatform24固定到提交 `f62243e56a30472deed9112631b2480ab461df3e` |
| 结构化记录 | 13个区域通过检查；主机结果与设备结果分别记录 |
| 本轮设备操作 | 未安装、未临时启动，手机保留上一轮系统 |

本机构建日志为 `build-services73-format2.log`、`build-services73-boot.log` 和运行时构建日志，按发布边界保留在Git忽略目录。公开源元数据最后又执行了一次prepare-only解析；该次报告中的 `kernel_built=false` 仅表示没有重复构建，实际内核结果见前述成功构建记录。未执行镜像哈希或镜像回读比较。
