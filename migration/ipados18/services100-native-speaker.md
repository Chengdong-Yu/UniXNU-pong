# services100：扬声器文档的 Darwin24 适配与 WLAN 内存修复

## 参考与边界

阅读用户提供的 `unixnu-speaker-docs/给作者的说明.md`、
`docs/drivers/sm8850-speaker.md`，以及 data-a-live 的 `wsl/PandoraAudio.cpp`。
借鉴原生 IOAudio2 发现、音频环、硬件就绪后启动时钟、原生控制值和独占请求的结构。
不复制 SM8850 / CS35L43 的 GPIO、固件、增益、ACDB 实例、PAS 或 SID。
不增加虚构的 Hawking / 麦克风，保留 Pong TFA9873、SID1801 和现有 AudioReach V2 路径。

上一轮已采用远端 GLINK intent 表48项及共享 SCM 会话锁，本轮保留。
文档提出的完整安全世界 WAITQ 中断唤醒仍未实现，不能把串行锁等同于完整 WAITQ 支持。

## 实际原因与代码

services99 REC console 明确记录 GPU 连续池512MiB成功后，WLAN128MiB
`IOBufferMemoryDescriptor` 构造失败；没有网卡注册、固件启动或扫描。
这与之前已能扫描但 stock wifid 控制未接通的故障不同。
XNU 非 pageable IOBuffer 路径会先请求一个大块共享 kernel allocator extent。
WLAN现改用 VM-backed 内存对象并显式 `prepare()` 固定全部页面，再建立原生4KiB SMMU映射；
保持128MiB连续IOVA、WC缓存、36位物理范围检查和总线主控隔离。
这样避免依赖同一大块内核分配路径；是否解决该真机分配失败，须看本轮启动。
GPU池不以退回旧256MiB冒充问题解决。

services99 音频日志只有 ASPL 物理设备发现，没有原生 VAD 输出端口、StartIO 或播放成功。
增加 `PongIOAudio2.h`，编入现有 power archive 的正常迟延元类初始化，
实际 DSP / 功放 / DMA 就绪后才发布输出设备；现有 ASPL 兼容路径保留。
核对22H374 CoreAudio的 IOAudio2Manager匹配、内存type0状态页、
SetStreamActive方法6、StartIOWithFlags方法10、SetControlValue方法2与原生控制字典。
使用独立 S32 stereo环、硬件启动确认后的19.2MHz主机时间、16档真实数字音量和静音，
将原生环实际样本送入现有 DSP V2数据命令，未伪造DSP完成或新增按键直接写音量的旁路。
保留UniXNU通用driver-port路由策略，不宣称实现Apple板载codec及其输入依赖。

## 上游与验证范围

UniXNU core更新至066c8d8，本轮新提交为设备文档；独立 Ath12kWifi f6bf561、
AdrenoMetal ce5e19e 与本地已用版本一致。Pong远端main更新至f088c08，
新userdata脚本、工作流和旧iOS16树清理已合并；用户脚本内容未改，本地修补保留。

只做所选恢复版本的代码/ABI检查和编译，不运行测试、基准、镜像哈希或镜像回读。
保持services99现有OS和Data，自动重启继续关闭。构建/临时启动成功不代表
系统Wi-Fi开关、关联、音量键或媒体声音已真机通过。

## 本轮构建与启动回执

Darwin24完整内核编译及预链接打包完成。Fastboot临时启动：Sending 2.353s、Booting 10.177s 均OKAY。保留services99 OS/Data，未写永久boot分区，设备留在运行状态。系统界面、Wi-Fi开关、路由、音量与实际出声仍待本轮设备反馈，不能以命令回执声称这些功能已修复。
