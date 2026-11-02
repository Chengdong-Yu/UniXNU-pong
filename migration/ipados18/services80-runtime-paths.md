# services80：Sileo 内置与无线、音频运行路径修复

## 实机日志

读取 REC 的 `console-ramoops-0` / `pmsg-ramoops-0`，以及实际 Data 的 `logs/pong-sileo.log`。原始记录在本机 `out/ipados18/logs-services79/`。只读取诊断文件与定位文件所需的 APFS 元数据，没有镜像哈希或镜像回读比对。

- services79 的音频 APM、两颗 TFA9873、低地址 DMA、输出服务均已建立；audiomxd 的 HAL 已实际连接，`initialized connected=1`。没有 `StartIO` 或 PCM 播放完成记录，因此尚不能宣称出声。
- 音量键 down/up 已进入 SpringBoard 的 native increase/decrease。没有证据表明应更换 PMIC GPIO；继续处理实际音频输出与路由。
- Wi-Fi 已越过 PCIe link/BAR、SID1c01 native4K DMA、MHI 和固件加载。QMI 返回 chip_id `0x12`、soc_id `0x400c1211` 以及真实固件版本。新失败点是 `WMI_INIT_CMDID` 发送超时，不是 PCIe training 或旧 SMMU phase6 错误。
- Sileo launchd job 已执行，但因为注册器前置的 persona99 检查而 exit77，实际注册完成标记不存在。旧日志中 dpkg 完成不等于应用已注册。

## 实际修改

### Wi-Fi

共享 Linux shim 的 `wait_event_timeout` 会在退出时再次计算 condition。ath11k WMI 用该 condition 发送 skb；第一次成功已转交缓冲区所有权，第二次求值可能重复发送或访问已转交的缓冲区，并消耗发送 credits。

Pong 的兼容层覆盖为每轮只计算一次 condition；即使 dispatcher 始终有 IRQ/work，也检查实际截止时间。保持真实发送返回值和故障处理，不通过增加 credits、伪造 ACK 或关闭流控绕过初始化。

继续保留 services79 已在实机越过的 TBI/shared-ASID 和真实 DMA/MSI 路径。没有重新套用不同芯片的 PCIe 或 SMMU 常数。

### 音频

新增独立的 `audio-route` launchd job，通过 CoreAudio 原生 API 枚举真正的 `PongAudioDevice`、检查 alive/output streams，并在没有有效默认输出时选择该设备。保留已有有效输出，不伪造设备或播放测试音。程序最多等待60轮；错误记入独立 Data 日志 `logs/pong-audio-route.log`。

保留已实际连接的现代 ASPL HAL、低32位 DMA、Pong TFA9873/MI2S 参数和原生 StartIO。没有把主机编译成功当作功放出声证据。

### Sileo

- 将官方已暂存的实际 app、图标和资源复制到 OS `/Applications/Sileo.app`，保留其原始 entitlement，生成4 KiB签名。APT/dpkg 和软件包继续使用原有 `/var/jb`。
- 注册器不再把 persona 操作当作应用注册前提，也接受系统原生已注册且实际路径匹配的有效代理。
- 删除 job 中被 launchd 拒绝的 DYLD 环境变量；必要的子进程环境由首次配置脚本设置。
- 审查选定 Darwin24 原生 `spawn_validate_persona` 后确认：非root调用者不能以 persona 请求 uid0/gid0。仅分配 persona 无法让现代 Sileo 的 root命令工作。
- 添加 Sileo 专用 spawn 适配：只识别安装目录中的实际 Sileo 进程及其 persona99/root请求，将命令交给安装好的受限 setuid helper。内核原生权限检查不变，其它进程和普通 spawn 不走此路径。
- helper 用真实调用方文件身份、root所有权和写权限核对安装的 Sileo，不开放常驻 root 服务。

## 之前修改的影响

Sileo 的 persona 准备与注册耦合是确定存在的启动阻断，已去除。无线发送等待来自共用 shim，与 ath11k 的有副作用 condition 不兼容，已在 Pong 适配层修正。此前音频 translating context 已在 services79 替换，实机确认越过该停点，不撤回已取得的 DMA/CoreAudio 连接进展。

同时检查了驱动选择、公共 RPMh/SMMU 锁、native4K 单份链接、匹配 ath11k 归档、固件/BDF选择、HAL安装位置、原生服务缓存、真实 PMIC事件与停止/失败处理。没有凭可选服务警告删除系统包，也没有声称静态审计能排除所有硬件或并发缺陷。

## 构建和部署

完整 Darwin24 内核、配套 init/UI/HAL/Sileo helper/注册器和音频选择服务编译完成。启动镜像位于本机 `out/ipados18/boot-services80/pong-xnu-initial.img`。

OS 跟踪修改10个 sparsebundle band，共80 MiB。此次只更新 OS，保留现有 SystemOS 和 Data。主机构建、安装记录与后续设备结果分开保存；不运行测试套件、基准、镜像哈希或镜像回读比对。

已完成 OS 写入并执行 Fastboot 临时启动：Sending 2.323秒、Booting 10.174秒，均为 OKAY。没有刷写永久启动分区，临时启动后没有主动重启手机。设备端 Sileo界面/包管理、WMI初始化/扫描、音频默认路由/StartIO/实际出声结果仍待确认。
