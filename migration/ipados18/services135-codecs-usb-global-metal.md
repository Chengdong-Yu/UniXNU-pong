# services135：系统解码、USB session 与全局 Metal

## services134 实机证据

REC console/pmsg 共1,193,168字节，没有捕获到 kernel panic。USB 的真实 Pong eUSB2/NXP 初始化已经通过，revision a2、ID 22:32:11；DWC3 id 33313130 与 SID0 DMA 启动成功，但没有主机 configured 事件。不能再把问题归为 QUP 时钟未启动。

AudioConverterService 已由原生 launchd 拉起；原生 Speaker stream active0/1、非零 committed PCM、DSP WRITE_DONE 均存在。两个 TFA 的控制0=4218、输入1=8004、TDM20=2883、sink23=b；实际非零 PCM 后 status11 仍8040，尚不能认定模拟输出已工作。上一版 audiomxd 内部 HAL 设备图查询没有返回日志，本轮移除该额外查询，保留已有物理设备连接与原生 PCM 所有者。

Metal 不是未启动：有真实 Vulkan 设备、GPU scanout completion，60秒记录1030个实际 command buffer completion、平均约18.9ms。此前 OS 的 root-owned clients 文件已经是 `*`，启动参数已有全局发现。DRM modifier 的拒绝有布局缓存和现有 GPU shadow 处理，Apple binary archive 的不兼容返回原生失败；不能把这两条信息等同于整个系统仍在 CPU 渲染。

## 实际修改

### 系统解码

独立 `libpong-audio-codecs18.dylib` 使用选定 iPadOS18 SDK 的公开 AudioConverter / AudioFormat ABI。只注入 audiomxd、AudioConverterService、AUHostingServiceXPC_arrow；不把 UI 适配器注入音频服务，也不给桌面新增 AudioToolbox 依赖。

默认 compressed → LPCM 请求从实际安装的 decoder 列表选择 Apple software manufacturer，避免 Apple 硬件 codec 延迟到首次 fill 才申请不可用的 Apple 音频硬件。明确指定 class 的成功请求不改动；其打开报 format/operation unsupported、hardware in use 或 no hardware permission 时才尝试实际软件 decoder。不存在或创建失败时保留真实错误，PCM转换、编码和应用显式成功的 class 请求不被替换。没有假 PCM、清零代替解码或吞掉解码失败。

新增独立构建配方 `scripts/build-pong-audio-codecs.py`。现有用户构建脚本未编辑。init 只在该文件实际存在、root所有且不可被组/其他用户写入时添加有界的独立注入规则。现有 UI、ElleKit 和用户 tweak 选择保留。

音频规则置于可选ElleKit规则之前；现有tweak-refresh只截断最后的ElleKit规则再重建，不会移除板级codec规则。审阅发现此顺序问题后已重新编译init并重新暂存。中断安装回执单独保留，重新开始完整OS代际传输，禁止带着部分更新进入启动。

### USB

参考 Nothing SM8475 源码中的 dwc3-qcom.c（c402fcdb3a1523d10b15ef1123bced45456f7592）和 Pong 实际1MiB controller window，补齐0xa6f8800 QSCRATCH 的外部 session 控制。

电池服务只读 PMIC 的真实 BmUsbOnline，独立原子快照在未知/断开DSP时为-1，不改动电池容量、充电电流和现有充电策略。USB等待真实online后设置 UTMI_OTG_VBUS_VALID、SW_SESSVLD_SEL 和 LANE0_PWR_PRESENT，再打开 DWC3 RUN_STOP。真实拔线时停控制器，有界确认HALT后撤销session；DMA与端点资源保持，下一次主机reset重新建立EP0。保留 root-ready、独立USB DMA和共享SMMU lease，不制造AppleUSBDeviceFamily或Mux服务。

### 全局 GPU

保留 `pong_metal=1`、`pong_metal_global=1`、全局 `*` 发现、真实CP/fence前提和GPU scanout。iPadOS18的 `maxComputeThreadgroupMemory` 现在返回运行时实际线程组内存限制；`maxVertexAmplificationCount` 根据实际支持谓词求上限，避免原来的缺失查询。未知的GPU核心数和未经实现的Apple私有能力没有被虚构为支持。

按官方 GPU 适配说明保留 Pong A730、4KiB、22H374 和 ABI1 的完整匹配，不把 A740 / 24A446 的地址或实验性 ABI2 直接套入。本轮没有禁用所有 CPU 处理；解码、系统调度及不支持的操作仍由真实可用实现承担。

## 构建与部署

内核、独立codec库、static init、HAL、UI库、匹配Metal bridge和bundle均已编译。主机构建与设备实际功能分开记录。OS使用完整一致代际传输；保留Data/SystemOS和203项已有包，最新guest dpkg状态及tweak配置保留。没有测试、基准、镜像哈希或镜像读回比较。自动重启保持关闭；安装结束后临时Fastboot启动，不刷永久boot分区。

实际配套安装回执 `/Users/desktop/Downloads/Pong-install-services135/installation.json` 为complete，OS 9,663,676,416字节写入完毕，os-storage pending已清空。第一次传输因会话中断停止，恢复ADB libusb后续传；随后为修复ElleKit规则顺序主动停止，保留 `Pong-install-services135-interrupted` 回执，重新完整传输了最终配套代际。

最终镜像 `/Volumes/PongDev/build/boot-services135-final/pong-xnu-initial.img` 已执行一次临时Fastboot启动：Sending2.424s、Booting10.178s均OKAY，总计15.152s。手机保持运行，没有主动返回REC。命令成功不证明本轮解码、可听输出或USB枚举成功，须依后续设备日志/实际现象判断。

### 本轮实际 USB / GPU 结果

主机已实际枚举 `Pong XNU console`，创建 `/dev/cu.usbmodemPONG_SM84751`。通过标准CDC DTR打开后，在12秒内收到108,518字节真实启动/运行日志，保存在 `/Users/desktop/Downloads/Pong-usb-console-services135.log`。因此本轮CDC日志传输已接通；AppleUSBDeviceMux/Finder配对仍属于未实现的另一后端。

实时USB日志含真实GPU scanout completion、新进程Vulkan设备发现，以及25个已完成的实际GPU command buffer记录。两项新增设备限制已编译部署；未知的其他私有能力仍报告缺失。没有在这段实时日志中观察到codec选择消息或可听声音证据，不能宣布音频全部修好。

新增 `scripts/capture-pong-usb-console.py` 可有界保存CDC日志，不发送命令、不重启设备、不依赖pyserial；既有REC日志路径保留，USB作为实时附加通道。

来源：[GPU接口](https://unixnu.com/zh/porting/gpu)、[音频与电源](https://unixnu.com/zh/porting/power)、[Nothing dwc3-qcom.c](https://github.com/NothingOSS/android_kernel_msm-5.10_nothing_sm8475/blob/c402fcdb3a1523d10b15ef1123bced45456f7592/drivers/usb/dwc3/dwc3-qcom.c)。
