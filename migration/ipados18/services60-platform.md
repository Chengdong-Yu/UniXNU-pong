# services60：内存、PCIe、亮度和 iPadOS18 音量适配

## services59 运行证据

用户确认已进入桌面。本轮 REC 抓取位于 `/Volumes/Untitled/Pong-iPadOS18/audit-services59/`，console 1085917 字节、pmsg 3317 字节。

- 原生 Adreno/Metal provider 和真实 GPU scanout 均有完成记录。
- Wi-Fi PHY 通过 ready 检查，ELBI=1、Link status=0x10110008，DLL 未激活且训练超时；不是固件扫描成功。
- `PongBacklight` 在发布服务前失败，UserEventAgent 无可用 backlight service。
- 两个音量键的 PMIC down/up 与 HID `sent=1` 都已记录；用户同时确认电源键正常。音量故障位于后续系统动作路径。

## 修改

### 内存

保留原有 4 GiB 连续主启动窗口、其页表保护/映射 helper。额外固件空闲 DRAM 区间用最终虚拟地址直接建立 4 KiB/2 MiB 页表，不再制造 `gVirtBase + PA - gPhysBase` 的旧连续别名来调用原 helper。分配元数据仍从主窗口取得，切换 TTBR 前不读写额外区间。

- loader 只交接成功 AllocatePages 的 EfiConventionalMemory，排除 DT reserved-memory 和实际 GOP。
- 最终固件池保留 256 MiB，避免交接前占尽分配资源；保留区、BootServices 和 RuntimeServices 不伪装为可用内存。
- 完整检查 bank 版本、总数、对齐、范围、重叠和总量后才提交。可选契约无效时保留主窗口，不在日志初始化前为此 panic。
- ADT 内该属性实际位于 0x314，仅 4 字节对齐；早期解析先逐字节复制到 8 字节对齐的局部数组，避免开启对齐检查时直接 uint64_t 读取异常。不能仅凭此认定是 services58 的唯一停点。
- PV 和 VM 紧凑索引与交接顺序一致；PMAP 初始化前仍保持 PA 非受管语义，区间查询排除物理空洞。
- `pong_ram_banks=1` 请求额外区间；`=0` 可保留主窗口。实体 12 GiB 不等于 OS 可分配 12 GiB，实际数量以本轮 XNU 日志为准。

### Wi-Fi PCIe

按 Nothing SM8475 的 DesignWare 定义启用 PORT_LINK_DLL_LINK_EN、清 FAST_LINK，明确设置初次 Gen1 训练目标。继续要求 ELBI XMLH_LINK_UP、DLL Active 和 QCA6490 真实端点 ID。

使用内置 DesignWare MSI receiver；Pong DT `int_global_int` 为 SPI140 / INTID172，Nothing pci-msm 枚举 MSI0 为 PARF bit23。端点 MSI 数据 0..31；IRQ base800 仅作为 Linux shim 的内部编号。使用单独保留的 MSI 落地页，不再要求 GIC MBIS 或声称配置了 ITS。中断先清接收状态再 EOI，持续空中断会关闭该接收器。

### 屏幕亮度

只把 DSI_CTRL 的 DSI_EN 作为运行条件，允许固件保留视频引擎。事务中临时设置 CMD_MODE_EN，完成后恢复原控制位。只拒绝真正开启的 TPG，保留其非活动配置。若 22H374 library-only AppleARMPlatform 没有注册 AppleARMBacklight，则用本原生亮度实现提供该类；先检查已有类，避免重复注册。原 DCS 0x51/0x52 写入/回读、渐变和有界超时保持。

### 音量

22H374 SpringBoard 的 selector 字符串确认包含 SBVolumeControl、increaseVolume/decreaseVolume。只在真机运行时方法 ABI 匹配后注册音量桥接：backboardd 将实际音量按键状态发送给 SpringBoard 主队列，再调用原生音量控制。短按计数防通知合并丢失，长按延迟 400 ms 后以 100 ms 重复。没有虚构音量数值或音频设备。电源键保持 HID，ABI 不匹配则继续原 consumer HID 路径。

OS 更新只替换 `/pong/libpong-ui18.dylib`，不修改 init、服务 plist、壁纸或 Data。新增 refresh_ui 命令避免无关资源更新。

## 构建与结果

内核、DarwinBoot24、UI adapter 已编译。services59 保留作为用户已确认的可启动基线。真机扩展内存容量、PCIe/扫描、亮度滑块和音量动作尚待 services60 结果；编译通过不能视为驱动运行成功。

- 最终内核：`out/ipados18/kernel-services60-final/mach.development.pong`，包含早期 DT 对齐修复。
- 最终镜像：`/Volumes/Untitled/Pong-iPadOS18/boot-services60-final/pong-xnu-initial.img`。
- 构建日志：`/Volumes/PongDev/build/Pong-iPadOS18-native/build-20261027T094803Z.log`。
- OS 更新：`out/ipados18/install-services60/installation.json`，已完成 OS 单窗口 6845104128 字节写入，SystemOS/Data 保留。
- 结果记录：`out/ipados18/services60-ready.json`；实际临时启动回执：`out/ipados18/temporary-boot-services60.json`。`boot-services60` 中较早的镜像不包含最后的对齐修复，应使用 `boot-services60-final`。

依用户设置，不进行镜像哈希、镜像审计、设备镜像回读、测试或基准。只临时 fastboot boot，不刷写 boot 分区，保留现有 8 GiB UFS Data。
