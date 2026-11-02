# services133：Pong / 22H374 的 GPU parity、扬声器输入与 USB 板级准备

## services132 实机证据

REC console/pmsg 共约 1.31 MB。SpringBoard 与 backboardd 已运行，有真实 GPU frame/copy completion，运行到 04:42:30，没有 kernel panic。没有本轮唤醒失败记录，因此保留 services132 的成对源缓存退役和音量锁修复；不能用未复现的日志宣布所有按键问题已解决。

音频实际发现两个 TFA9873 revision b73，原生 IOAudio2 Speaker 打开、MI2S PRM 3.072 MHz 请求成功、DSP WRITE_DONE 和非零 committed PCM 均存在。此证据止于数字传输，没有可听输出确认。原 ampState 在首次 PCM 提交前读取，不能将 AMPS/SWS 当作稳态结论。

USB 记录 stage=2、EFI_STATUS=0、flags=2：固件 StopController 成功；PHY retention 没有被认证，原来按预期在首次控制器 MMIO 之前跳过。不能把“无 USB 枚举”说成 Apple 配对已实现。

## 上游分支合并范围

已拉取 feat/ipados27-gpu-parity，当前 FETCH_HEAD 为 88604d01c191727bf4b4113269b3a9518bb730d3。该远程默认只配置 main refspec，本轮已把确认拉取的 FETCH_HEAD 显式同步到本地 origin/feat/ipados27-gpu-parity，避免继续使用陈旧分支指针。组件 pins：AdrenoMetal 278333f、Adreno740 c0a2f39、GopFramebuffer 4975bc8。

实际接入当前 ABI1 的变更：

- BGRA GPU opaque copy shader，以一次计算 dispatch 处理两边 pitch 和最终 alpha；资源创建失败仍走已有未提交帧回退，真实提交失败保留 DMA 页。保留 source_id、三项源缓存及 services132 idle/reset 生命周期。
- 内核 BO 池耗尽后的连续分配尝试，按 Pong 的真实 output width 和 4 KiB 页面处理；可选 256 KiB 到 2 MiB 范围，失败继续原普通 wired 分配，不引入此前回归的 pageable descriptor/cache ownership。
- frame GPU/encoding 时间桶、pass/pipeline format 统计，20/60 秒各输出一次有界真实工作摘要，不运行基准。
- 着色器预热按函数续接，以实际库 size/mtime 和加载的 runtime/Turnip UUID 区分代际；BGRA 与 RGBA16Float 变体。配套 bundle 内嵌本机 22H374 OS 元数据列出的 184 个 metallib 路径，不在手机上扫描目录。保留 128 pipeline、单库两分钟、每次五分钟的预算，忙碌时退出并保留进度。

适配边界：24A446 的 QuartzCore 指令地址/布尔表、A740/SM8550 DPU/SID、2ms 持续扫描在屏 surface 的哈希实验不能原样应用 Pong。22H374 继续使用实际 QuartzCore UUID 和可写选项表。ABI2 per-ASID/页表切换及 pageable BO 仍保存为此前实验源，不在本次破坏已工作的 ABI1/Turnip 配对。没有声称所有上游实验路径都已启用。

## 音频实际修补

TFA9873 的 TDMSPKS 仅选择 slot；TDMSPKE(0x2300)才连接 sink0。当前从 CNT 导入的是槽位/格式与保护参数，没有显式设置该输入使能。本轮为两个真实功放打开 TDMSPKE，保留每颗功放原本的左右 slot、保护配置、PRM 与 native CoreAudio 接口；不写 OTP/MTP 或提高功率。功放状态移到初始 PCM 排队之后读取，用后续实机日志区分数字输入与模拟 engage。

## USB 实际修补

新增 PongUsbPhy.h，寄存器顺序参考 NothingOSS SM8475 的 phy-msm-snps-eusb2.c 和 repeater-i2c-eusb2.c，保留 GPL-2.0-only 来源。硬件输入再次从本轮 REC FDT 读取。

1. 真实 cmd-db 解析 ldob5=880mV、ldoc10=1200mV、smpb10=1800mV、ldob2=3075mV 和 xo.lvl。请求通过 PongPlatformResources 的共享 active TCS4，不依赖 Wi-Fi 初始化。
2. USB30 GDSC、bus/UTMI/reference branch 都有有界就绪检查，之后才允许 eUSB2/DWC3 MMIO。保留独立 32 KiB USB DMA、原 64 KiB audio DMA 以及 root-ready/SID lease 顺序。
3. QUP0 SE5@994000 复用实际 I2C firmware/pin/timing，拒绝偷取未完成命令。访问真实 NXP@4f revision/chip ID，应用且读回 Pong DT 的 06=40、07=22、08=64。GPIO7 没有猜测式 SPMI 写；NXP 不响应则停在依赖失败。
4. 在 0x088e3000 配置 Nothing 的 19.2MHz eUSB2 程序并读回，独立于固件 PHY flag。成功后才进入 DWC3；固件 handoff 仍必须证明旧 DMA 停止，不能静态填 flags=3。

这完成了此前缺失的板级准备实现；CDC 枚举和实际电气就绪仍待本轮设备结果。IOUSBDeviceFamily/AppleUSBDeviceMux/Finder 不是现有 CDC 后端的功能，尚未宣称原生 Apple USB 接通。

## 构建与部署

内核、匹配 ABI1 bridge 和 Metal 插件已编译完成。使用现有 Turnip；记录新版 shader 和配套 bridge，不混用 ABI2。配套 OS 已更新；Guest dpkg 包版本/状态与上轮保留代际一致，保留最新 status，Data/SystemOS 不重置。用户修改的构建/CI 脚本未编辑。没有镜像哈希、镜像审计、读回对比或基准。自动重启关闭，安装完成后执行一次临时 Fastboot 启动并保持运行。

构建记录：Pong-kernel-services133.log 的 kernel_built=true；ABI1 Rust release 及插件链接完成。启动镜像102514688字节，八核ADT和原RAM布局保留。配套OS已在本机挂载后完成组件替换、卸载及full-transfer跟踪收尾；设备 OS 窗口 9663676416 字节已全部写入，36 个写入回执齐全，installation.json state=complete。Data/SystemOS保留，未写boot分区。

最终部署：`/Users/desktop/Downloads/Pong-build-services133-final/pong-xnu-initial.img` 已通过 fastboot -s dd72e429 boot 临时启动，Sending 2.466s、Booting 10.178s 均 OKAY，总计13.138s。配套安装回执位于 `/Users/desktop/Downloads/Pong-install-services133/installation.json`；os-storage pending已清空。首次主机枚举快照没有发现 Pong CDC 或 Qualcomm EDL，不能证明 USB 已连接或 GUI/音频效果。设备保持运行，自动重启关闭，等待本轮实际使用结果。
