# iOS 16 残留审计

针对 apfs3 的真实启动日志与当前 Darwin 24 构建路径；字符串相似不等于运行时仍在使用旧二进制。

| 项目 | 审计结果与处理 |
| --- | --- |
| APFS UUID | 已移除 kern_exec / vm_unix 的旧根卷与缓存卷 UUID；OS、SystemOS、Data 均从实际卷元数据生成配置头。apfs3 日志确认三卷绑定与挂载成功。 |
| CPU 生命周期 | 已按 Darwin 24 的 processor_boot / first-boot wait / scheduler completion 改造；真机确认 GICv3 / CNTV 正常。 |
| 驱动编译目标 | ArmCpu、ArmDTPlatform、NativeUfs 均使用 iOS 18 目标与 Darwin 24 SDK；不再固定 arm64-apple-ios16.0。两个独立驱动脚本的缺省构建目录与 target 也已改为 18，避免脱离总构建器单独运行时落回 16。 |
| UEFI 交接 | 继续复用 Pong 固件硬件支持，但实际 kernel handoff 为 DarwinBoot24，命令行与结构偏移按 Darwin 24 定义。 |
| 系统认证常量 | 删除 iPadOS18 打包器中遗留的 20G81 System-volume-auth-blob 固定摘要。构建仍按用户要求跳过镜像校验。 |
| GPU 私有 ABI | 旧 Pong Metal provider 针对 20G81。当前配置不链接它，并在 Darwin 24 构建器中明确阻止误启用；硬件源码保留用于后续移植。 |
| 用户态注入 | 实际编译 ui_compat18.c 与运行时 HID 输入接口，不编译旧 QuartzCore / Preferences / 图标固定地址修补。 |
| 静态 init | 构建器明确传入 18.7.10 / 22H374 宏，apfs3 真机已打印 22H374 并 exec 对应 launchd；本分支 init.c 的缺省版本宏也同步为 18，防止单独编译回退到 20G81。 |
| SHA256 ABI | 对比了原厂 22H374 与本机 SHA256_Init / Update / Final 的调用字段与转发方式，未发现 iOS 16 摘要 ABI 混入。没有为此执行镜像散列或设备镜像回读。 |
| SystemOS 校验粒度 | apfs3 的 SystemOS 与 Data 已挂载，init 已 exec 原版 launchd；随后 SystemOS 的 dyld cache .09 在 16 KiB 文件数据校验时返回 EILSEQ。公开 VFS cluster 快速校验仅接受校验块等于 PAGE_SIZE，当前内核为 4 KiB。这是待排除的粒度兼容问题，不能把校验失败直接当成功。 |
| 新 SystemOS | 用 macOS 正常文件读取路径，将原厂 22H374 文件复制到新建普通 APFS 卷；不继承原卷封印元数据，不修改 dyld 缓存内容。副本容积 5 GiB，现有 8 GiB Data 在设备内向后搬移保留。任何复制或 I/O 错误都会停止安装。 |
| SystemOS 压缩继承 | systemos4 已无前次 APFS 数据校验错误，但 `.35.dyldlinkedit` 的算法 0x702 解码返回 EINVAL，最终使 PID 1 退出。ditto 默认保留文件压缩；构建器现明确使用 `--nopreserveHFSCompression --nohfsCompression --noclone`。本机从原厂只读挂载重新展开了 1,806 个压缩文件，保留 SystemOS UUID、容量与 Data 位置。 |
| 部署错误处理 | systemos5 日志仍选择旧 SystemOS checkpoint xid 18；本机故障文件在 xid 37 已无 UF_COMPRESSED / decmpfs 属性。旧 raw exec-in 写入路径缺少远端状态与回执，仅主机返回成功不足以确认应用。两个安装脚本现使用无 PTY 的 shell-v2，每块写完要求设备端完成回执后再记录进度；不进行镜像散列或设备镜像回读。 |
| 文件所有权 | systemos6 真机已选择 xid 37，并越过压缩错误；随后共享缓存映射被 uid=99 拒绝。SystemOS 构建器原先没有启用源/目标卷真实所有权，复制继承了 unknown UID。现启用源卷 ownership、以 `-owners on` 挂载目标，并逐条恢复原厂 UID/GID/mode（本机 2,891 项）；原厂缓存为 uid=0/gid=80。保留 Darwin 24 的 root-owned cache 要求，未修改成接受任意 UID。OS/Data 原有 apply 挂载已使用 owners on，无需套用这次 SystemOS 元数据修复。 |
| 剩余显示协议 | MiniIOSurface / MiniIOMobileFramebuffer 仍包含从 iOS 16 演进的私有消息布局；需要在 dyld / launchd 正常后按 iOS 18 实际调用继续适配，不能据其已编译就宣称 18 UI / Metal 可用。 |
| 旧版本命名 | prepare-ipados18-components.py 调用的 fetch-ios16-assets.py 是复用的 restore/ZIP 读取器；输入由 22H374 BuildManifest 指定。遗漏的公共 DriverKit/TrustCache 头文件来自历史公开 XNU tag，是声明兼容层，未导入旧系统 kext。 |

日志：本机忽略目录 `out/ipados18/logs/boot-apfs3/`。镜像、原厂文件与 Apple 私有组件不提交仓库。


## services24 增量审计

已按原厂 22H374 框架修正 IOSurface bulk attachments、共享页颜色字段、5 个时间戳与 detach 状态，清理 xpcproxy 身份版本旁路、缺失 persona 旁路及未实现 work_interval 空成功。旧的无 UUID 文本 fixup / VM fault 写页钩子已移除；4 KiB 原生签名兼容缓存保留。详细的 ABI 偏移、内存生命周期修复、日志证据和仍未实现的显示能力见 `services.md` 的 services24 段。

这里的审计覆盖实际构建路径与已识别的私有 ABI；不代表整个闭源 iPadOS18 系统已经完整适配。需要启动结果确认后续 UI 出帧与服务状态。


## services25 增量与未完成确认项

已继续修正 22H374 IOSurface 非 bulk 读写的 12 字节协议头、CoreVideo 清理入口、字典变更分配失败的事务回滚，以及显示 VSync 端口/定时器生命周期。缩放基准已移除旧 iPhone X 固定点数。键袋 descriptor 输出错误现在真实返回；用户态 persona 文件原子发布及 QuartzCore 修补范围检查已编译。详见 services.md。

| 要求 | 当前证据 | 尚需确认 |
| --- | --- | --- |
| 实际构建为 Darwin24 / 22H374 | 最终 build report、编译日志与预链接清单 | 新镜像运行日志 |
| 已识别的旧 IOSurface 头部与共享字段 | 原厂 22H374 框架调用、已更新源码与成功编译 | services25 属性传递与实际 frame submission |
| 内存引用与失败事务 | 源码路径审计、成功编译 | 新 REC 记录及界面运行；尚不代表整个内核内存审计完成 |
| 服务和 APFS 路径 | services23 已记录核心服务 running、三卷挂载；当前 recipe 使用 native18 分支 | services24/25 的运行结果 |
| 固定地址显示修补 | 22H374 UUID、入口指令和可执行段边界；不使用 20G81 vtable | 更新 UI library 后的运行日志 |
| GPU 私有接口隔离 | 当前配置不链接旧 20G81 档案 | iPadOS18 Metal 尚未移植，不能宣称加速可用 |
| 未实现的 IOSurface 扩展 | child alias、ownership、共享事件等当前明确不伪报成功 | 用本轮 trace 确认启动实际需要的调用，再实现对应 18 ABI |
| 部署状态 | services25 内核/运行时/24 MiB OS 增量均已生成 | 等设备重新进入 REC 后安装与临时启动 |

目标保持进行中，未以“没有新的报错”或“编译成功”替代完整适配和运行确认。

## services26 显示内存与启动事务

已修复缩放行缓冲部分分配失败后容量丢失、完全离屏矩形的列索引越界、巨大离屏行遍历，以及显示模式/DT 节点准备失败时的资源回滚。绘制线程仅在完整准备模式与节点后启动，控制台仅在有实际可绘制像素时交接。相关 22H374 framebuffer 标量/结构接口已对照本机原厂实现，未套用未经确认的 iOS16 ABI。

最终内核及 services26 启动包已构建；services25 用户态增量继续配套使用。设备连续未被 ADB/Fastboot 枚举，尚无 services24 之后的运行日志，也未部署 services25/26。剩余的实际出帧、未实现 IOSurface 扩展是否被启动路径依赖，以及真机内存/通知生命周期结果，必须等设备重新连接后确认。详细构建路径与部署顺序见 services.md。

## services27 部署更新

设备已重新连接 REC；当前 pstore 为空，没有新的 XNU console 可分析。已加入首个有效可见图层的统一控制台交接：复用 iOS16 的 GraphicsMode + ReleaseScreen 顺序，并在绘制前清除整屏旧文字，避免局部更新遗留日志。屏幕关闭不关闭 ramoops 写入，未更改真实 panic 处理。

新内核构建成功，services25 的 24 MiB OS 增量已完成安装；services27 经 Fastboot 临时启动，Sending/Booting 均为 OKAY。services25/26 的累积修改现已一起部署。实际 UI 出帧和无日志叠加仍待本轮屏幕或 REC 确认，不能以 Fastboot 接受镜像替代。

## services28 像素布局审计

已移除旧的 w30r/l10r/w30R、b3a8 像素格式重命名/plane 丢弃逻辑；未转换像素时不再向客户端声称数据是 BGRA。22H374 的输出格式仍在匹配的 QuartzCore 适配中选为 BGRA。软件 scanout 现在检查 1×1 元素、真实行跨度和最后可见行范围，再读取像素或交接控制台，避免把块元素当逐像素数据读出分配区。plane 存储上限由 4 同步为原厂回复的 20，回复最大大小的断言绑定该常量；use-count 递增检查回绕。

内核最终编译/链接成功，日志为 `build-20261024T232835Z.log`。这些是源码证据支持的兼容性和内存修正；没有新 REC 运行日志证明它们是否影响当前启动。设备最后实际启动仍为 services27，services28 的部署状态见 services.md；未以编译代替真机确认。

## services29 发布与统计事务

修正 IOSurface/软件 KeyStore 关键属性失败后仍发布服务的行为，返回失败并进入已有清理路径。移除旧表面创建性能日志的无锁累加/清零分支，保留的总耗时与复用计数使用原子操作；不改变实际内存缓存或像素清零行为。

设备本轮重新进入 REC，但只保存了 50 字节损坏诊断头，无 XNU console。已保存原始记录，并核对实际 ramoops 地址/容量/ECC；记录不足以证明 services27 的运行状态。最终内核（`build-20261024T233626Z.log`）已预链接，services29 通过 Fastboot 临时启动，Sending/Booting 均成功；services28 累积改动现随之部署。

| 剩余要求 | 当前能证明的范围 | 尚缺证据 |
| --- | --- | --- |
| 移除已识别 iOS16 地址与协议混用 | 实际源码补丁、22H374 本机二进制调用和构建配置 | 新运行日志中实际依赖的扩展请求；不能据有限审计宣称所有私有接口完整 |
| 内存分配/回滚/计数与服务发布 | 已审查并修正的路径、最终成功编译与链接 | services29 真机运行结果 |
| 图形首帧后无文字叠加 | 有效 BGRA 图层触发统一释放控制台和整屏清理，RAM 日志通路保留 | services29 屏幕及持续更新画面 |
| 审计后临时启动 | services29 的 Fastboot Sending/Booting 回执 | iPadOS18 界面实际出现、可操作及后续日志 |

目标尚未达到可完成的证据标准，下一步需要本轮屏幕结果或新的 REC 运行记录。


## services33 活动源码与构建入口

移除 18.x 对 iOS16 mkjobs 子串过滤表及 iOS16-v45 构建脚本树的依赖。init 的旧 APFS2142 参数块、MetalExtensions/壁纸覆盖及用户态旧挂载分支已删除，并用 PONG_NATIVE_APFS18 限定本配方；内核 helper 路径收敛为 /pong/init。22H374 原厂 jetsamproperties.J171/J172 配置资源此前被 jobs 筛选误删，现与服务定义分开保留。PerfPowerServices 按用户要求从当前任务目录和原生服务缓存移除。具体 RunningBoard 等待快照、配置路径与编译记录见 services.md；当前证据尚未确认 18.x UI 出帧。


## services34 Darwin24 XPC 能力查询

原厂 22H374 的 libxpc 新增服务端口关联检查，不能直接按已工作的 iOS16 用户态行为推断兼容。services33 Data 快照定位了服务端“Bogus check-in”路径；检查发现当前无私有 Mach 消息过滤 provider，而内核返回 KERN_NOT_SUPPORTED 被 libxpc 拒绝。Pong 开发构建现保留两个真实端口的接收权检查，再报告 KERN_INVALID_CAPABILITY，让原厂库使用其既有无过滤关联兼容路径。注册 provider 的处理和真实无效端口错误保留。详见 services.md；待本轮启动确认连接与首帧。
