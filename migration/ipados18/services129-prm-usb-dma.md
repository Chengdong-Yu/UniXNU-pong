# services129：上游同步、MI2S 时钟和 USB 低地址 DMA

## 上游

2026-10-30 拉取 UniXNU main 并从 066c8d8 快进到 f0721e9。唯一新增提交为 docs: refresh device records，涉及设备目录与记录，没有新的驱动、Metal、XNU 或构建逻辑。保留上游工作区已有 components.toml 和 Darwin24 profile；Pong 用户维护的脚本未修改。UniXNU-sheng main 仍为 08cde20，没有可合并的新提交。

## services128 实机日志

REC console 记录原生 Speaker 客户端、正常 start、真实 DSP WRITE_DONE，以及 committed nonzero native PCM consumed。旧 PCM 设备已不再与其并存。用户仍报告无声，因此不能以这些成功记录证明实际喇叭通路。

USB 初始化前出现多次 vm_page_find_contiguous 失败，随后退出；没有“PHY 前提缺失”、角色错误或 SID0 失败。该路径中低于 4 GiB 的连续内存申请失败，不能误判为重复器或冷启动 PHY 已经证实失效。

## 音频

参考 UniXNU power 文档、Sheng 的 PRM 自身端口及回包处理、其他设备的 AudioReach 时钟生命周期；保留 Pong TFA9873 和 tertiary MI2S 路由，不复制 Cirrus、SoundWire、SM8850 GPIO 或时钟 ID。

Pong 播放原先缺少显式位时钟请求。本版通过 PRM port 2 请求 Qualcomm audio_prm.h 的 CLOCK_ID_TER_MI2S_IBIT 0x104，频率为 48000 × 2 × 32 = 3072000 Hz，attribute 1。处理 PRM_REQUEST_DONE/RELEASE_DONE 及 token，失败不伪造时钟成功。先申请再准备/启动图；停止或失败时按顺序停图、关闭功放、释放位时钟。

每次启动读取真实 TFA 状态 0x10/0x11/0x13/0x14，区分 clocks-stable、PLL、TDM 错误、engage 和 device-manager 状态。这是随修复保留的诊断，不是模拟声音或替代实际通路。是否出声仍待本轮硬件结果。

## DMA 所有权与 USB

现有 UEFI 从主 XNU window 之外分配 EfiReservedMemoryType 的 64 KiB，并通过 /chosen/pong-audio-dma 传入。共享头 PongLowDma.h 将其明确分区：音频 [0,32768)，USB [32768,65536)。音频八个 3840-byte period 总共 30720 字节；APM 仅映射自己的 32 KiB。USB 事件环/TRB/数据仅使用后半。两者不清零或映射对方区间，不归还未知 DMA 生命周期内存。

USB 删除失败的运行时 low32 contiguous 分配；保留已有 PHY/时钟/角色前提、halt、独占 SID0、实际 EP0/bulk 事件和配置后发布策略。修正上版行内注释吞掉 GEVNTCOUNT 初始化的问题。轮询事件中断仍屏蔽，启动失败不阻塞系统。

这仍是实验性真实 CDC 日志后端，不等于 Apple IOUSBDeviceFamily/Mux/Finder。冷 PHY 供电和 Apple 控制器服务层并未凭空补齐；主机枚举必须在本轮启动后观察。

## 部署

内核编译与临时启动完成后补记。现有 services128 OS 保留，无需写入或重置 Data/SystemOS；型号 MYMC3LL/A、固定展示序列号、已配置的 rootful/Sileo、Wi-Fi、亮度、按键和 GPU 组合保持。没有运行镜像哈希、镜像读回或性能基准，自动重启关闭。

实际部署：Darwin24 内核编译成功；主机构建元数据记录 upstream_revision=f0721e9745c45ad2600cd69c74730f1cca55d56a。开发盘打包空间不足，清理本轮重复产物后将最终输出移至本机 /Users/desktop/Downloads/Pong-build-services129。该目录 pong-xnu-initial.img 已临时启动，Sending 2.317 秒、Booting 10.176 秒均 OKAY。未写 OS/Data/SystemOS 或 boot 分区，未返回 REC。

启动后约 45 秒主机观察：没有新增 Pong USB 设备，也没有 /dev/cu.usbmodem 端口，未取得 CDC console。设备仍留在运行状态；不能据此确认 USB 已通或音频已出声，下一轮需 REC 的初始化/PRM/TFA 状态记录定位后续失败。自动重启未开启。
