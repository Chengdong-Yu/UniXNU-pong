# Pong Darwin24：上游合并与运行时联合审计

## 源码基线

- UniXNU-pong 已从原有基线快进至用户上游 `9c232305f414760007e3fd824888600a1832a29d`。
- uefi-pong 已快进至 `0e29a5f22bf00c1a8364816526c0485ce9e612b6`，同步其 Mu_Basecore 和 Silicium-ACPI 子模块。
- 保留现有板级修补；合并前 tracked patch、状态记录在本机 `out/ipados18/services79-audit/`，恢复用 stash 保留。
- 内核仓库的路径解析、Intel macOS 构建入口、IOStorageFamily pin、AEA/fat ARM64 输入处理已合并。CI 不带私有固件的配置与本机完整硬件配置分开。
- UEFI 更新主要是可独立构建的 vendored 包、DEC、Linux 构建和依赖固定。核对了其中的音频低地址 DMA 与额外 RAM 交接实现；本轮没有重新构建或刷写 UEFI 固件，启动包继续使用已有 DarwinBoot24 EFI。

## REC 中的实际失败点

证据保存于本机 `out/ipados18/logs-services78/`。同时读取 console、pmsg 和 Data 中的 `pong-sileo.log`。

| 路径 | 已观察到的结果 | 本轮修改 |
| --- | --- | --- |
| WLAN | PCIe0 link/BAR 已建立，128 MiB DMA 页表已构建；SMMU phase6 失败，TCR2=`0x38071`、SCTLR=`0x27` | 保留显式 TBI 策略，再选择与实际硬件读回相符的 shared-ASID 策略，租用独立 ASID `0x1c01`。继续严格检查根表、寄存器、SID 路由和 fault，不用放宽比较代替修复 |
| 音频 | APM ready，两颗 TFA9873 均读到 revision `b73`，stage4 DMA 失败；HAL connected=0 | 对照 Sheng 的真实音频传输，用 UEFI 已预留的连续低32位64 KiB缓冲区和 Pong SID `0x1801` 的物理直通；去掉对物理 DSP 地址的第二次翻译及不需要的页表分配 |
| 音量键 | 已有 physical HID、SpringBoard native increase 事件 | 保留真实按键入口，补齐 HAL 设备级 scalar/decibel/mute 属性与 level-control 对象之间的对应；共同操作实际 PCM 增益并发出属性通知 |
| Sileo | dpkg 完成，但旧 uicache 及后续注册器均注册失败，没有有效 application proxy | 去掉 pre-launchd fork，改为真正的 launchd job，并同时写入 daemon plist 与 launchd cache；在 mobile 域注册实际安装路径，仅当代理有效且路径匹配才写成功标记 |
| Sileo 提权依赖 | 安装的 Sileo 2.5.1 在现代 iOS 上使用原生 persona99，替换 legacy giveMeRoot 无法覆盖该路径 | 使用选定 Darwin24 的348字节 persona ABI、原生管理接口与明确 entitlement 初始化 root persona99；已有身份冲突则拒绝覆盖。保留可重试失败，不伪造注册成功 |

Sileo 首次启动脚本只配置可选载荷，不重置 Data。旧 uicache 的全量系统应用注册触发已取消。registrar 在真实 launchd 服务命名空间内工作，再切换到 mobile 用户。

## 同时核对的依赖与边界

- 音频与无线共用的 APPSSMMU/RPMh 锁由 `PongPlatformResources` 提供；音频不依赖选中 Wi-Fi 驱动。
- native4K 只链接一份；ath11k 使用本轮相同构建树生成的归档，避免误链接旧产物。
- 本机固件输入沿用 pinned source 与 `local.toml`，上游环境变量可显式覆盖。没有加入主机绝对路径到可发布配方。
- WLAN 仍须依次完成 SMMU、MSI/MHI、QMI 固件和 station 初始化，才发布真实网络接口。
- 音频仍须完成实际 APM 图、DMA 和功放就绪，才发布可用端点。GLINK 断开清除就绪状态，不释放可能仍被 DSP 使用的 DMA。
- 保留命令等待期间的 WRITE_DONE、停止期间不再补充 DMA、失败图清理及 HAL 有界时间戳读取，避免音频状态机悬挂。
- 保留 CPU/GPU 板级性能策略、8核配置和完整屏幕日志。没有将 Apple 平台驱动或 Sheng 硬件常数当作 Pong 的额外硬件 owner 启动。
- console 中还有不属于本轮失败点的 XPC lint、蓝牙匹配、可选服务和 CoreData 警告。没有据此盲目删除系统包，也没有宣称已消除全部潜在缺陷。

## 主机结果

- 完整格式2内核构建完成，日志 `out/ipados18/build-services79.log`；实际 XNU 编译日志位置由该文件记录。
- init、UI、HAL、sleepserver、Sileo 注册器编译完成，日志 `out/ipados18/build-runtime-services79.log`。
- 配套启动包：`out/ipados18/boot-services79/pong-xnu-initial.img`。
- OS runtime 已更新，跟踪到5个 sparsebundle band，共40 MiB；只写 OS，现有 SystemOS/Data 与包数据库保留。
- 安装记录 `out/ipados18/install-services79/installation.json`；没有镜像哈希、镜像回读比对、基准或测试套件。

## 设备结果

OS 更新已成功提交，services79 随后通过 Fastboot 临时启动：Sending 2.323秒、Booting 10.125秒，均为 OKAY；记录在 `out/ipados18/fastboot-services79.log`。没有刷写永久启动分区，也未在临时启动后主动重启手机。

本轮 Wi-Fi 扫描、实际 PCM 出声、音量变化、Sileo 打开与包管理仍需启动后的真实结果；编译或 Fastboot 返回不能代替这些结论。
