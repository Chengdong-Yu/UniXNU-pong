# iPadOS 18 服务与 SpringBoard 启动

## owners7 真机证据

REC 日志位于本机忽略目录 `out/ipados18/logs/boot-owners7/`。本轮没有 kernel panic，原版 22H374 launchd 已完成 early boot，SpringBoard 的 18 适配库构造函数也已进入。尚未看到系统界面。

当前明确的阻塞：

- `backboardd` 的文件任务定义含 `_Conclave`，launchd 以“仅允许 trusted service”拒绝导入；Pong 当前不构建 Exclaves 后端。
- launchd 反复查找 type 6 / `PERSONA_SYSTEM_PROXY`，得到 ESRCH。
- `notifyd` 反复以 Foundation namespace 19 / code 1 退出；本轮日志没有具体退出描述，不能直接把它归因于 persona 缺失。
- 目录导入模式还有非 trusted 服务发布事件流的警告，需要后续按实际依赖继续处理。

## services8 实际修补

内核使用 Darwin 24 的 persona API，在静态 init 之前创建真实的系统代理 persona：`persona_alloc → persona_init_begin → persona_init_end`。沿用原生对象引用、凭据派生和进程计数，不把 persona 查询错误改成无条件成功。

用户态构建器继续读取 22H374 自身的原始任务定义。22 个核心任务保留启动行为，457 个其余入选任务按需启动，104 个定义因实际缺少程序、变体或现有硬件策略而排除。核心定义缺失列表为空。iPadOS 18 的条件特性、MachServices、RunningBoard 属性与程序路径均保留，未导入 20G81 的私有地址修补。

文件导入策略处理了日志中已确认的问题：

| 字段 | 处理依据 |
| --- | --- |
| `_Conclave` | 当前没有 Exclaves 后端；该字段直接阻止 backboardd 导入，移除任务的 Conclave 绑定。 |
| `HighPriorityIO` | 22H374 文件导入器报告 unknown key；移除两个任务中的该字段。 |
| `HopefullyExitsLast` | 导入器明确表示该旧字段不再受支持。 |
| `NO_EnvironmentVariables` | 源模板中的禁用占位字段，不当作真实环境变量启用。 |

services8 曾在用户态主动退出路径记录 Foundation 描述。后续源码审计确认 notifyd 的不可恢复断点直接由异常路径设置 Foundation 原因，未经过这个函数；services9 移除该无效记录点。

## 更新方式

`userland/build-ipados18.py refresh_jobs` 当前更新准备好的 OS 副本中的真实 `/System/Library/LaunchDaemons`、服务缓存和下述 launchd 配置，使用 owners on 挂载并设置系统所有权。

`install-ipados18-ufs.py --windows OS --stream-compression gzip` 只写 OS 窗口，要求现有 SHNGROOT 布局相同、每块设备端完成回执有效后才记进度。SystemOS 与实际 Data 不重写。未进行镜像散列或设备镜像回读校验。

剩余显示用户客户端和 Metal 私有 ABI 仍需通过 22H374 的实际调用逐项适配；本轮任务导入和 persona 修补不代表已验证界面渲染成功。

本机已完成新内核编译、22H374 组件预链接与 OS 窗口更新，收到 13 个设备端区块完成回执；services8 的 Fastboot Sending / Booting 均返回 OKAY。SystemOS、Data 和 boot 分区未重写。运行时服务和界面状态等待本轮屏幕或 REC 日志确认。

## services8 日志与 services9 修补

services8 的 REC 日志未出现 kernel panic，SpringBoard pid 5 与 backboardd pid 8 均已进入 launchd 的 running 状态。该状态仅说明进程已创建，尚不能确认界面完成初始化。

日志还明确记录：

- `system/com.apple.notifyd: Non-system service tried to claim event stream com.apple.notifyd.matching`。
- notifyd 反复以 Foundation namespace 19 / code 1 退出。
- 系统域请求 type 6 / UID -1，mobile 域请求 type 6 / UID 501；services8 仅有 UID 0 的代理 persona，二者均未匹配。

22H374 notifyd 的程序含 `Event publisher error` 致命断点；源码中 Darwin 24 的不可恢复异常处理会将这类断点直接记为 Foundation 退出。事件流被拒绝是当前有明确证据的服务配置错误；具体断点位置仍以运行日志确认。

services9 的用户态构建恢复 22H374 自身的 Version 7 launchd 服务缓存。仅替换其中的 LaunchDaemons 为当前入选并适配后的 479 个真实任务，保留该版本的 AppExtensions、AppRemovalServices、SystemLibraryTreeState、Symlinks 和条件特性结构。缓存采用 `/System/Library/LaunchDaemons` 索引，但该版本尚保留了旧目录别名，未能完成预期的原生缓存任务导入；该不匹配在 services10 中继续修正。

进一步对照 22H374 启动程序确认：launchd 先根据缓存文件大小分配共享内存，再由 `launchd_cache_loader` 填入内容；restore 策略原先跳过该任务，只恢复 plist 文件仍然不足。因此构建器按 Mach-O 节表定位 `__TEXT,__config`，只为 `Boot.launchd_cache_loader` 增加 `PerformInRestore=true`，保留其 RequireRun / RequireSuccess 和其他启动任务。没有修改指令或套用 iOS 16 地址。原始程序保存在 `/pong/stock-config/launchd.original`，配置后的副本重新生成 4 KiB 页的开发签名，保留原始标识符和 entitlement。

开发 boot args 增加 loader 本身支持的 `launchd_unsecure_cache=1`，让原版 loader 装载本地适配缓存，不将修改后的 plist 与原厂旧签名配对，也不伪造 AMFI 服务。真正的缓存装载结果仍需由本轮真机日志确认。

services9 在原生 persona 注册表创建系统域与 UID 501 的代理对象，persona ID 由 Darwin 24 分配。该版本将系统域 UID 写为 `KAUTH_UID_NONE`，但后续日志与头文件确认它实际为 -101，未能匹配 launchd 请求的 -1；services10 修正此值。保留对象查找、凭据派生与进程计数，不把查询失败伪装为成功。不可恢复断点进入已有的限量异常记录函数，加入 notifyd 与其断言字符串所在寄存器，随后继续原生退出处理。

本机最终镜像位于忽略目录 `out/ipados18/boot-services9-loader/`，内核构建日志为 `build-20261024T094108Z.log`。完整 OS 更新收到 13 个设备回执后才启动；Fastboot Sending / Booting 均返回 OKAY。SystemOS 与手机 Data 保留。较早的中间打包结果已清理，首次传输记录保留为 `install-services9/interrupted-installation.json`；最终完整写入记录位于 `install-services9-loader/installation.json`。界面和服务运行结果等待本轮屏幕或 REC 日志。

## services10：按 18.x 规则修正 notifyd 导入路径

services9 的 REC 日志显示 cache loader 已执行，early boot 完成，未再出现缺少或无法解析 MRM 缓存的信息；notifyd 仍因事件发布错误退出。普通文件导入路径仍显示 `/pong/LaunchDaemons`，其事件流声明被判为 non-system。

对比 iOS 16 / 20G81 与 18 / 22H374 的原厂 notifyd 任务定义，MachServices、PublishesEvents、事务和 System session 声明均相同。iOS 16 的目录导入策略使用 `/System/Library/LaunchDaemons → /pong/LaunchDaemons`，而 22H374 的事件注册路径增加系统任务资格判断。22H374 的目录缓存按路径前缀查询，旧符号链接解析到 `/pong` 后，与缓存中的 `/System/Library/LaunchDaemons` 索引不一致。

services10 将生成的任务直接安装到真实的 `/System/Library/LaunchDaemons`，移除这个特定旧别名，缓存索引与实际目录一致。保留原版 notifyd、通知中心 Mach 服务、事件流以及其所有正常错误处理。未将 20G81 程序或私有地址替换到 22H374 中，未增加检测线程或屏蔽 notifyd 断点。

系统代理 UID 同时改为 `(uid_t)-1`。Darwin 24 的 `KAUTH_UID_NONE` 定义为 `~(uid_t)0 - 100`；services9 的 UID 501 已匹配成功，系统域则仍请求 -1 并报告 found 2，不能把这个差异当作已修复。

services10 内核构建日志为 `build-20261024T100458Z.log`，启动镜像在本机忽略目录 `out/ipados18/boot-services10/`。OS 更新收到 13 个完整区块回执后，Fastboot Sending / Booting 均返回 OKAY；未重写 SystemOS 或手机 Data。本轮 notifyd 是否稳定及界面状态仍等待实际屏幕或 REC 日志确认。


## services10 日志与 services11：修正 PGZ 别名的 IPC 端口校验

services10 的 REC 日志确认系统域 UID -1 与 mobile UID 501 的两个代理 persona 均已注册。本轮没有再记录先前的 notifyd 事件发布错误、non-system 事件流拒绝或 persona 匹配错误，系统继续启动 UserEventAgent、containermanagerd_system 等任务。随后发生真实 kernel panic，尚不能据此确认 notifyd 长时间稳定或界面启动成功。

panic 发生在 cfprefsd（pid 19）通过 kqueue 接收 Mach 消息时。`ipc_kmsg_copyout` 校验 voucher 的 IPC 端口，经 `ipc_port_lock_check_aligned` 进入 `zone_id_require_aligned`。错误地址 `0xfffffff1810e9ec0` 位于日志公布的 PGZ 区间 `0xfffffff181046000–0xfffffff18180f000`。旧校验直接把这个保护别名当作普通 zone 页读取，PGZ 的已分配标记因而被误判为过大的 chunk。panic 文本中的 “being freed” 来自通用错误函数；实际调用栈不是释放操作。

iOS 16 源码中已有对应的 `pgz-aligned-validation.patch`。services11 将相同修复适配至 Darwin 24：先调用原生 `pgz_decode(addr, 1)` 取得 backing allocation，再计算元素偏移、核对 zone 与对齐。原生 PGZ 的存活状态、越页和 use-after-free 检查，以及后续 zone 校验均保留。没有关闭 PGZ、放宽错误返回或伪造 IPC 端口。

同时修正 iPadOS 18 编译脚本中将功能修复与可选探针捆绑的条件：仅 `offline-pgz-alignment-probe.patch` 受探针选项控制，真正的地址转换修复不再受该开关影响。探针选项继续关闭。本轮仅重编内核，沿用 services10 已部署的 OS、SystemOS 与 Data，不重写用户态镜像。


services11 内核构建完成（`build-20261024T102511Z.log`），已导出聚合源码补丁并打包至本机忽略目录 `out/ipados18/boot-services11/`。从 REC 请求进入 bootloader 后，Fastboot Sending（2.321 秒）与 Booting（10.170 秒）均返回 OKAY。实际命令与回执保存在 `temporary-boot.json`。未重写 OS、SystemOS、Data 或 boot 分区；未做镜像散列与回读校验。源码空白检查通过，未另行增加或运行测试。PGZ panic 是否消失及后续界面进展，等待本轮屏幕或 REC 日志确认。


## services11 日志与 services12：8 核与原厂 CPU 合成器

services11 的 REC 日志未出现 kernel panic，notifyd 退出循环也未再记录。原有 IPC PGZ 校验已越过，系统持续运行至多个用户态服务。内核公布了 1080×2412 的 MiniIOMobileFramebuffer；backboardd 则反复因 `NSURL initFileURLWithPath:` 接收到 nil 而 SIGABRT。按配套共享缓存符号解析，调用链为 BaseBoard 的 `BSSystemContainerForCurrentProcessPathProvider.libraryPath+0x2c → cachesPath+0x10`。这不能归因于单纯 CPU 性能不足。

services12 的启动打包默认选择 Pong 的全部 8 个实际 CPU 节点（MPIDR 0x000…0x700）与 `cpus=8`。使用已有 SM8475 PSCI、GICv3 SGI/PPI 和每核 CNTV 定时器；先注册全部处理器并安装向量，再走 Darwin 24 的 `processor_boot → processor_wait_for_start → ml_cpu_init_completed`，没有调用在 Darwin 24 中已经无效的旧 kext `processor_start`。等待每核首次启动完成后记录一条完成信息。保留 `--cpus 1` 作为隔离选项，核数与实际调度状态仍需本轮日志确认。

CPU 渲染沿用 iOS 16 选择原厂软件后端的做法，但重新解析本机 22H374 自身的 QuartzCore。本版本的类是 `CA::WindowServer::AccelServer`，不是 20G81 的 EAGLServer。其 `render_update` 在 renderer 返回空时明确跳转至 `Server::render_update → sw_renderer → SWContext`。仅在 backboardd 与 `pong_ui_software=1` 下选择该路径，保留原版 SWContext、IOSurface 与实际 framebuffer 拷贝后端。按 22H374 Mach-O UUID 与入口指令匹配后，修改进程私有代码页，恢复 RX 并同步指令缓存；没有使用 20G81 地址或伪造 Metal 设备。

backboardd 的 BaseBoard 容器 provider 改为本端口实际创建、由当前进程拥有的可写目录 `/private/var/mobile/Library/Pong/backboardd`，Library/Caches 一并创建。该改动局限于 backboardd 的这一个 provider，未全局修改 NSURL 或捕获并吞掉异常，也未改动 launchd 必需的系统代理 persona。

本轮 OS 新增 `/pong/init` 和 `/pong/libpong-ui18.dylib`；静态 init 的注入路径由 iPadOS 18 构建选项选择。后续小型用户态运行时更新随 OS 更新完成，保留手机当前 Data 与已生成状态。`refresh_runtime` 命令仅挂载、修改准备好的 OS 副本，实际设备部署仍通过原有 OS 窗口写入流程。初次 prepare 也包含同样文件，避免只有本机增量操作可用。


services12 内核构建日志为 `build-20261024T104009Z.log`，原生代码与 22H374 用户态适配库均已编译成功，未另行增加或运行测试。准备好的 OS 副本更新后，设备 OS 窗口完整写入 6,845,104,128 字节并收到 13 个完成回执；SystemOS 与实际 Data 保留。随后 Fastboot Sending（2.326 秒）与 Booting（9.925 秒）均返回 OKAY，临时启动镜像位于 `out/ipados18/boot-services12/`，没有写 boot 分区。8 核实际上线、容器路径修补效果与软件合成帧输出仍等待本轮屏幕或 REC 日志确认。


## services12 日志与 services13：同步 OS helper 的内核路径

services12 的 REC 日志逐一确认 CPU 0–7 first boot completed，ArmDTPlatform 登记 cpus=8，所有 UFS 窗口以及 SystemOS、可写 Data 均已挂载。随后在 CPU 3 上运行的 PID 1 触发 `Unsupported Pong init path @kern_exec.c:7892`。这发生在 helper 执行前，CPU 软件合成器尚未运行，不能据此归因于 QuartzCore 地址或多核启动。

已先对照 iOS 16 的 `zzzzzzz-pong-hackios-userland-abi.patch`：旧端口刻意只允许 Data 中的 `/private/var/pong/init`。services12 将实际 helper 迁到 OS 的 `/pong/init`，却遗漏同步该 allowlist，这是上一版的配置衔接错误。

services13 将 Darwin 24 的允许路径明确限定为 `/pong/init` 与原有 `/private/var/pong/init`，保留 `load_init_program_at_path`、原生 Mach-O/签名加载和所有真实执行错误处理。没有删除路径检查或将 exec 错误改为成功。重新核对 18 构建器的 helper 路径、UI 库路径、boot args 与已安装的 OS runtime，仍使用 22H374 组件与 4 KiB 开发签名，不套用 20G81 的地址或私有结构。

本轮最初仅修补内核路径；按追加要求继续审计并合入以下显示 ABI 与原厂初始化修补，因此还需更新 OS 中的启动配置。SystemOS 与实际 Data 保留，继续启用已完成首次启动的 8 核。


### 同批补齐的 22H374 显示 ABI 与初始化

进一步对照 iOS 16 后端和实际 22H374 的 IOSurface / IOMobileFramebuffer，确认旧数据布局会阻挡接下来的 CPU 合成路径。这些是实际接口变化，没有将新请求长度改成无条件成功。

| 接口 | iOS 16 后端 | 22H374 用户态要求与修补 |
| --- | --- | --- |
| IOSurface selector 13 初始化 | 0x18 字节，32 位上限 | 0x28 字节，三个上限字段改为 64 位，version=1 位于 0x22。 |
| IOSurface create / lookup 结果头 | 0x68 字节 | 0x88 字节；分配尺寸、宽高、行字节数和偏移为 64 位，planeCount 位于 0x58。 |
| IOSurface plane | 0x80 字节 | 0x98 字节；compression 位于 0x6c；结果最大 0xc68 字节。 |
| IOMFB SwapEnd | 0x46c 字节，ID 位于 0x50 | 0x514 字节，ID 位于 0x98；surface、矩形、设置/清除掩码的已用字段均后移 0x48。 |

IOSurface 内部的实际分配、512 MiB 上限、维度限制与共享页仍保留，只有输出序列化改为新的 wire ABI，避免把内部 32 位界限与外部 64 位字段混为一谈。所有结构尺寸和关键偏移在正常编译中检查。IOMFB 按 22H374 `_kern_SwapBegin / SwapSetLayer / SwapEnd` 的实际数据地址解析，仍处理四层与真实 surface 引用，保留原来的 CPU 行缩放和 framebuffer 写入。另启用最多 4 个既有 scanout 行工作线程，单核参数时自动保留 1 个。

services11 日志显示 `usermanagerd` 初始化 boot task 被 restore 策略跳过，随后真实 daemon 反复因 persona manifest 加载失败而退出。本轮为原版 `Boot.usermanagerd` 的 `/usr/libexec/usermanagerd --init` 增加 `PerformInRestore=true`，保留原任务的参数、AllowCrash 和错误处理，使用 18.x 自己的初始化代码生成状态；没有编造 plist schema 或屏蔽 daemon 崩溃。

最终批量镜像位于 `out/ipados18/boot-services13-batch/`，使用 `build-20261024T110201Z.log` 的内核。较早 `boot-services13/` 仅为未启动的中间打包产物；后者未设置 4 个 scanout 工作线程。实际启动回执以最终目录为准。编译完成，未另行增加或运行测试；源码差异空白检查还报告 kern_exec 中三个本轮修改前已存在的缩进问题，不影响本轮生成内核。


services13-batch 的 OS 更新已完整写入 6,845,104,128 字节并收到 13 个完成回执；之后 Fastboot Sending（2.326 秒）和 Booting（10.138 秒）均返回 OKAY。实际启动路径记录在最终目录的 `temporary-boot.json`，不是较早的中间包。SystemOS、实际 Data 与 boot 分区未重写，未进行镜像散列或设备镜像回读校验。本轮能否越过 init、完成 IOSurface 初始化与 persona 初始化并产生 UI 帧，等待新的屏幕或 REC 日志。


## services13 重试日志与 services14：容器、原生 manifest 和显示格式

services13 的第一次 REC 仅保留 50 字节残缺 pmsg，不能用作用户态停点证据。重新临时启动同一镜像后，REC 保留 281,573 字节 console；原始日志及摘要位于本机忽略目录 `out/ipados18/logs/boot-services13-retry/`。没有 kernel panic，CPU 0–7 首次启动完成，SystemOS 和可写 Data 挂载成功。原厂 CPU 合成器策略已安装，backboardd 的 nil 容器路径异常未再出现。SpringBoard 和 backboardd 已创建，但没有观察到提交画面的记录，用户也未见界面。

原版 `usermanagerd --init` 实际完成了 early boot，daemon 仍因 persona manifest 失败退出 8 次。只恢复这个 boot task 没有解决问题。从 REC 以只读 APFS 路径解析提取诊断文件时，确认当前 Data 没有 `/keybags`、`/logs`，两个 usermanagerd 日志文件也不存在。只读取 9 个缓存的 128 KiB 数据块用于定位这些目录，未做整盘复制、镜像散列或回读校验。

services14 的实际修补：

- 静态 init 在已有 Data 创建缺失的 root-owned `/private/var/keybags`（0700）及 `/private/var/logs`（0755）；初次 Data 生成也加入同样目录。已有目录及文件不重置。
- 仅向配套 usermanagerd 增加运行时适配。`persona.kb` 不存在时，调用 22H374 自身 `UMDPersonaManifest.manifest → dataValue` 生成初始序列化注册表，并以 O_EXCL、0600、fsync 写入。原生平台类决定格式与版本，没有手写 plist schema、伪造用户或 keybag 材料。存在的注册表不替换，原生 daemon 的解析与错误处理继续保留。
- 合入 iOS16 已运行的 `MCMUserIdentityCache.personasAreSupported` 单用户容器策略，在实际 18.x Objective-C 类及参数/返回类型匹配时应用，继续使用原生 UID 容器。它与 launchd 必需的两个 Darwin24 system-proxy persona 分开，后者保留。
- iOS16 的软件 framebuffer 格式策略重新定位至 22H374：`IOMFBDisplay.framebuffer_format` 的四字节 thunk 跳转到 `0x184396060` 实现。匹配该版 QuartzCore UUID 和三个入口指令后，选择 MiniIOMFB 真正支持的未压缩八位 BGRA；没有覆盖相邻 thunk、套用 20G81 vtable 或声称有 Metal 加速。私有页恢复 RX 失败时还原原始指令。
- 复用 iOS16 主线程唤屏流程，只在原版 18.x singleton 与方法签名存在且匹配时发出 wake/home 请求，保留正常锁屏认证决定，不改对象私有字段。

日志中的 IOMFB selector 15 对应可选 SetDebugFlags，selector 88 在该版 OpenByName 中发送附加提示，调用后的返回值未参与打开结果。没有证据表明这两个请求阻挡启动，保留真实不支持的返回。assistantd 的非启动必需异常、PerfPowerServices 的 group container 失败仍在日志中，未全局屏蔽异常。

本轮保留已经验证越过 init 的 services13 Darwin24 内核、8 核和 4 个 CPU scanout 工作线程，实际修改位于静态 init 与 22H374 UI 适配库。新运行时已编译，启动镜像位于 `out/ipados18/boot-services14/`；真机结果以之后的临时启动和 REC 日志为准。


services14 的 OS 运行时更新已完整写入 6,845,104,128 字节并收到 13 个区块完成回执。随后对 `out/ipados18/boot-services14/pong-xnu-initial.img` 执行 Fastboot 临时启动，Sending / Booting 均返回 OKAY；实际命令与回执保存在同目录 `temporary-boot.json`。没有重写 SystemOS、手机 Data 或 boot 分区，未进行镜像散列与回读校验。本轮运行时已编译，未另行增加或运行测试。usermanagerd 稳定性、容器响应与界面出帧等待下一轮实际日志，不能以 Fastboot 回执代替。

## services14 日志与 services15：Darwin 24 XPC 对端约束接口

services14 的 REC 保存了 398,183 字节 XNU console，没有 kernel panic。原生 usermanagerd 初始 manifest 已写入 136 字节，先前的 manifest 退出循环未再出现；仍有 PerfPowerServices 退出、lsd 的 SIGABRT 和一次 powerd SIGSEGV，尚未观察到 UI 帧。PerfPowerServices 并非已证实的唯一阻塞点。

按 22H374 自身 libsystem_containermanager 的错误表，`systemgroup.com.apple.powerlog : 52` 对应 `XPC_INTERRUPTED_REPLY`，不是空间不足或文件系统 errno。PowerlogCore 的原版容器请求失败后执行 exit(1)。从 Data 只读提取的 logd 文本同时反复报告连接 logd_helper 中断。相关原始记录保存在本机忽略目录 `out/ipados18/logs/boot-services14/`，仅提取诊断文件及定位它们所需的 APFS 元数据，没有整盘复制、散列或镜像回读校验；原生 log 工具未成功解析提取的 tracev3，不将其视为已解析的服务调用证据。

对照 iOS16 内核适配及 22H374 实际 libxpc，发现新版本 `_xpc_connection_token_satisfies_lwcr → amfi_launch_constraint_matches_process` 使用 AMFI mac syscall `0x66`；旧适配只有 `0x5a/0x60`，对新请求返回 ENOPOLICY。services15 补齐该版请求 ABI：0x40 字节请求包含 audit token、DER 指针和长度、0x404 字节结果缓冲区。开发约束关闭策略仅在实际设置 `amfi_get_out_of_my_way=1` 时生效；保留进程存在与 PID 版本检查、完整有界 copyin、DER 外层长度检查以及真实 copyout 错误，不将其描述成 Apple 签名或信任缓存校验。

同时为已有 miniamfi 的平台签名分类填写 Darwin 24 `CS_OPS_VALIDATION_CATEGORY`：按现有开发平台策略分类的 blob 设置 PLATFORM，其余设置 NONE，避免保持未初始化的 INVALID。未改变 iOS16 容器单用户策略，没有用固定目录替代 powerlog 容器，也没有删除 PerfPowerServices、lsd 或 powerd。

本轮是内核修复，保留 services14 已部署 OS、SystemOS、当前 Data 和原厂服务。Darwin24 内核已编译（`build-20261024T130031Z.log`），聚合补丁已导出。启动镜像使用独立目录 `out/ipados18/boot-services15/`。容器 XPC、lsd 和 UI 是否越过停点，仍需本轮运行日志确认。

随后已从 REC 请求进入 bootloader，对 services15 镜像执行 Fastboot 临时启动；Sending（2.323 秒）与 Booting（10.131 秒）均返回 OKAY，完整回执在同目录 `temporary-boot.json`。未更新 OS、SystemOS、Data 或 boot 分区，未进行镜像散列、镜像回读校验或额外测试。该回执仅说明临时启动命令已被接受，不能代替服务稳定性和界面结果。

## services15 日志与 services16：权限查询、缺席电源设备和 GPU ABI 隔离

services15 的 REC console 为 299,213 字节，没有 kernel panic。PerfPowerServices 仍报告 group container 错误 52，lsd 仍反复 SIGABRT；两个 containermanagerd 进程和 logd_helper 已启动，没有记录它们退出。Data 中 logd 文本仍显示 helper 连接中断。services15 没有记录 AMFI 0x66 的成功分支，不能认定上一轮修补已经解决容器连接。原始 pstore、可搜索文本和本轮选取的 Data 诊断文件保存在 `out/ipados18/logs/boot-services15/`。

原厂 22H374 服务同时包含 XML 与 DER 权限声明，libxpc 和 Security 的实际代码选择 DER 查询。审计另确认 Darwin24 `CS_OPS_ENTITLEMENTS_BLOB` 的 XML 导出改为依赖闭源 OSEntitlements 对象，而当前开发 AMFI 没有创建这个对象：原路径会返回成功但不填写调用者缓冲区。services16 在对象缺席时恢复原有内核 `csblob_get_entitlements → csops_copy_token` 路径，返回程序实际携带的 XML blob 或标准空头，保留原生 blob/special-slot 检查、短缓冲区 ERANGE 及 copyout 错误。没有伪造权限字典，也没有把现有 DER 缺失当作已证实的容器故障原因。

SpringBoard 的 22H374 `BCBatteryDeviceController.sb_deviceInternalBattery` 方法仍先请求 connectedDevices，再选择内部电池。沿用已运行 iOS16 的策略，按当前运行时的方法签名安装：仅当 IOKit 没有发布真实 IOPMPowerSource 时返回原接口允许的 nil，避免缺席电源后端的同步等待；存在真实电源设备时调用原方法，不编造电量或健康信息。该处理针对已知板级缺失，尚不能证明它就是本轮主线程的唯一等待点。

GPU 对照结果：

- 当前 defconfig、实际原厂预链接列表和日志均未启用 Adreno/Metal 或 Apple IOGPUFamily，没有旧硬件 GPU 驱动在本轮直接启动的证据。
- `PongIOAcceleratorABI.h`、PongMetalPlugin 和 Metal bridge 明确绑定 20G81 的 IOAcceleratorES、_MTLDevice 与像素格式接口；它们尚不能直接链接进 Darwin24。读取实际 22H374 Metal 的 `_MTLDevice` class RO 元数据，实例大小为 600 字节，现有 `PongMetalAbi.h` 则要求 528 字节；插件还明确要求 kern.osversion=20G81。这是确定的版本接口差异。构建入口增加明确的旧 GPU ABI 开关拒绝，避免修改配置后静默混用这些库。未删除后续适配所需源码，也未把扩大类大小当作完成 Metal 迁移。
- 已保留的 22H374 IOSurface 0x88 结果头/0x98 plane、0x28 初始化回复、IOMFB 0x514 SwapEnd 以及匹配该版 QuartzCore 的 BGRA 软件输出策略继续使用。此轮没有观察到 surface 分配或提交失败记录，也没有观察到 UI 帧；软件合成策略安装成功不代表已完成出帧。
- 已存在的选择性 os_log 路由扩展至 containermanagerd、lsd、PerfPowerServices、logd_helper、powerd，便于保留服务原本的具体错误；没有添加轮询探针或吞掉异常。

services16 同时更新内核与 OS 中的小型 UI 适配库，继续保留实际 Data、SystemOS、8 核和 4 个 CPU scanout 工作线程。实际运行效果以随后临时启动及新一轮日志为准。

Darwin24 内核编译完成（`build-20261024T131752Z.log`），源码聚合补丁已导出；22H374 UI 库及静态 init 编译完成，本机 OS 副本已更新并正常卸载。最终镜像位于 `out/ipados18/boot-services16/pong-xnu-initial.img`，只部署 OS 窗口，SystemOS 和实际 Data 不替换。未进行镜像散列、镜像回读校验或额外测试。

OS 窗口已完整写入 6,845,104,128 字节并收到 13 个完成回执，记录在 `install-services16/installation.json`。之后从 REC 请求进入 bootloader，对 services16 镜像执行临时启动，Sending（2.323 秒）/ Booting（10.132 秒）均返回 OKAY；实际路径与回执保存在同目录 `temporary-boot.json`。没有写 boot 分区。新一轮容器连接、lsd 稳定性和界面进度仍等待屏幕或 REC 日志确认。

## services17：重置 Data，并使用 LaunchServices 原生目录覆盖

用户报告 services16 仍只有文字日志，明确要求以开机为目标、尝试绕过服务并清空 iOS Data。REC 可用后先保存本轮 pstore（总计 509,961 字节）至 `out/ipados18/logs/boot-services16/`。日志仍有 lsd 的短时 SIGABRT 循环、PerfPowerServices 的 exit(1) 循环，以及 profiled 的 powerlog 容器错误 52。没有将 PerfPowerServices 的退出等同于已证实的唯一 UI 阻塞点；部分日志字符存在损坏，保留原始字节供后续分析。

继续解析实际 22H374 CoreServices，定位到 `_LSDefaults.systemContainerURL` 使用的 `__LSDefaultsBaseSystemContainerURL`：先 `getenv("LS_DATABASE_DIR")`，未设置时才调用 `container_system_path_for_identifier("com.apple.lsd", ...)`；后者失败时构造 `Error getting container url (%s)! Cannot continue.` 异常。原版方法随后在这个真实目录下附加 `Library/Caches`。这是 18.x 自带的配置入口，不需要移植 iOS16 的函数地址、替换 lsd 或吞掉异常。

services17 为原厂 lsd 任务设置 `LS_DATABASE_DIR=/private/var/mobile/Library/Pong/LaunchServices`。init 按 iOS16 可写路径策略创建该目录及 Library/Caches 父链，owner/group=501、0700，已存在的目录不重置。继续运行原厂 LaunchServices 的数据库、schema、注册逻辑及全部 MachServices，没有实现假的 lsd 应答。该路径处理是根据退出模式与原版失败路径制定的修复，尚需本轮日志确认是否越过 SIGABRT。

暂时排除已观察到退出循环的三个可选任务：PerfPowerServices、managedconfiguration.profiled、assistantd。保留全部核心任务（含 lsd、两个容器管理服务、notifyd、runningboardd、powerd、backboardd、SpringBoard）；排除的任务没有替换成返回成功的空服务。

Data 重置使用此前从 22H374 原厂模板生成、未导入手机运行状态的本机初始 APFS 镜像，整体替换既有 8 GiB Data 窗口，清除这几轮产生的数据库、容器、日志和 keybag 状态，由新 init 与原厂 daemon 重新初始化。保留卷 UUID、窗口大小和起点：`952D79E9-0CD4-47B2-B817-22889BF8AC50`，offset=12,231,639,040，size=8,589,934,592。写入器按实际 SHNGROOT 目录及 userdata 几何限制写入范围，不格式化整个 Android userdata 分区。此次 Data 清空已获得用户明确授权。

本轮重新编译静态 init 并生成独立 services17 启动包，沿用 services16 已完成的 Darwin24 内核及现有 CPU 显示路径，硬件 GPU 不启用。系统任务缓存和运行时同时更新到 OS 副本，部署范围为 OS 与 Data，SystemOS、boot 和固件不修改。未增加或运行额外测试、镜像散列或镜像回读校验；真机进度以随后临时启动和新日志为准。

OS（6,845,104,128 字节）和 Data（8,589,934,592 字节）已完整写入并收到共 29 个区块完成回执，记录在 `install-services17/installation.json`。iOS Data 重置已完成，窗口外的数据和 SystemOS 保留。随后从 REC 进入 bootloader，对 `out/ipados18/boot-services17/pong-xnu-initial.img` 执行临时启动，Sending（2.332 秒）/ Booting（10.130 秒）均返回 OKAY；完整命令和回执保存在同目录 `temporary-boot.json`。新 Data 的首次初始化、lsd 数据库建立与界面结果仍等待运行日志确认。


## services17 日志与 services18：修复 Data 重置输入和共享 UFS 故障范围

services17 的 REC 保存 14,845 字节 XNU console 与 50 字节 pmsg。8 核上线，原厂 APFS 已启动，OS 和 SystemOS 的首读完成；Data 首读随后报告 filesystem signature mismatch。旧错误路径调用 transportFailure 清除了全局 gHost.ready，导致已经发布的 OS 也不能继续读，最终 Failed to mount root device。此次并未运行到 lsd 或 Data 数据库初始化，不能据此判断 services17 的用户态修补效果。

为定位该错误，仅读取本机 Data 输入和设备 Data 起点的 4 KiB 文件系统头，未做整盘镜像回读、散列或扫描。两处均为相同的非 GPT / 非 APFS 内容；本机先前标作初始 Data 的 cdr 已不能用于重置。损坏原因尚未确定，不能将传输完成回执等同于有效文件系统。services17 记录中的“初始 APFS 镜像”指预期来源，实际部署内容在本轮发现已损坏。

services18 从仍由 macOS 正常挂载的原始 PongData18.sparseimage 重新转换独立的原生磁盘副本，用原有 22H374 Data seed 和数值 owner/mode 填充，正常卸载后原子替换本机损坏 cdr。保留 8 GiB 几何与 Data UUID，完成此前已经授权的干净 Data 重置；只重写 Data 窗口，不重复更新 OS 或 SystemOS。新增 rebuild_data 构建子命令保存这一可重复流程，不读取手机镜像、不修改原始模板。

同时修复 NativeUfs 错误范围：某个窗口文件系统头无效时，拒绝发布该窗口并明确记录名称，不再将格式错误当作控制器/DMA 故障清除全局 ready，也不使其他已发布的有效磁盘失效。真实读写、控制器和队列失败仍停止后续 DMA；格式和边界限制继续保留，没有接受损坏 Data 或伪造卷。


Darwin24 新内核编译完成，构建日志为 `build-20261024T144451Z.log`。UFS 驱动在同轮重新编译并链接，保留实际 22H374 的 190-slot IOBlockStorageDevice 接口和原生 APFS。新 Data 从原始空模板转换后已成功通过 macOS 的可写挂载、22H374 seed 填充、数值 owner/mode 恢复和正常卸载；本机替换流程完成，记录为 `apple/ipados18-22H374/prepared/data-rebuild.json`。当前 EXTRA 目录策略也同步到 seed 元数据，修正旧 seed 将 keybags 等端口目录预置为 0755 的问题。没有对镜像执行额外校验或测试。


最终启动镜像位于 `out/ipados18/boot-services18/pong-xnu-initial.img`，启动参数携带 `pong_revision=services18`。沿用 services17 已部署的原厂任务与运行时，不重复写 OS / SystemOS，也不启用未完成 22H374 ABI 迁移的硬件 GPU。Data-only 安装回执记录到 `out/ipados18/install-services18/installation.json`；临时启动结果以随后 `temporary-boot.json` 为准。


Data-only 写入已完成 8,589,934,592 字节并收到 16 个区块完成回执，安装器状态 complete；OS、SystemOS、boot 和固件保留。随后从 REC 请求进入 bootloader，对 services18 执行临时启动，Sending（2.322 秒）与 Booting（10.177 秒）均返回 OKAY，实际命令与回执保存在同目录 temporary-boot.json。没有写 boot 分区、执行额外测试或做镜像散列/整盘回读校验。是否越过根挂载、lsd 与 UI 初始化仍需本轮屏幕或 REC 日志确认。


## services18 日志与 services19：修补 lsd 实际选择的用户数据库路径

services18 REC console 为 282,339 字节，没有记录 kernel panic。8 核上线，SystemOS / 可写 Data 正常挂载，静态 init 显示 created=0、seed-errors=0，原生 persona manifest 已写入。上轮根挂载 panic 未再出现，lsd 仍以 SIGABRT 在约 20–60ms 内退出，累计 18 次；指定的 LaunchServices/Library/Caches 目录仍为空，没有观察到 UI 出帧。保留损坏字符的原始 pstore，未把文字损坏还原为不存在的日志证据。

对照 iOS16 / HackiOS 的容器处理，旧端口使用真实 UID 容器并在容器管理器中禁用不适用的 persona 模式，不删除 lsd。解析本机 22H374 CoreServices 又发现 services17 的具体遗漏：普通 lsd 的 databaseContainerURL 在非教育、非 lightweight-system-server 模式下选择 userContainerURL；LS_DATABASE_DIR 只作用于 systemContainerBaseURL。22H374 userContainerURL 的 server 分支执行 class-10 persona 容器查询，查询失败或缺失路径时最终直接 abort。这个确定存在的失败路径与当前早期 SIGABRT 一致，但 services18 没有保存 lsd 的调用栈，尚不能把它标成已经真机确认的唯一原因。

services19 将适配库接入真实 lsd，在 UID=501 且四个原厂零参数/对象返回方法的运行时 ABI 全部匹配时，为 _LSDefaults 的 systemContainerBaseURL、systemContainerURL、userContainerURL、systemGroupContainerURL 配置实际 mobile-owned 可写目录。用户库为 LaunchServices/User/Library/Caches，共享状态为 LaunchServices/Shared/Library/Caches，系统分支沿用原目录。只调整 lsd 自己的数据库存储位置，继续使用原厂数据库格式、初始化、应用容器与所有 MachServices；不编造 MCM 查询应答、不覆盖其他服务的容器目录、不更改私有对象字段或套用 20G81 地址。

保留原厂 fatal 行为：仅为 lsd 的 SIGABRT 保存最多 12 个有界帧后重新触发原信号，补足目前缺席的 osanalytics 崩溃记录；没有吞掉 abort 或让异常继续执行。此轮包含实际路径修补，并非只加入检测。保留 services18 内核、Data 及现有系统状态，只更新 OS 的静态 init 与 22H374 适配库。


services19 静态 init / UI 适配库已编译完成。构建命令把 lsd 加入 kern.exec_inject 的精确进程列表，避免只编译路径方法而没有实际载入 lsd。refresh_runtime 已更新本机 OS 的 /pong/init、/pong/libpong-ui18.dylib 与 /sbin/launchd fallback，并正常卸载 disk31；无需改动原厂 lsd 描述符或 Version 7 服务缓存。未额外运行测试或镜像校验。


services19 的 OS 窗口已完整写入 6,845,104,128 字节并收到 13 个区块完成回执，安装状态 complete。随后从 REC 请求进入 bootloader，对 out/ipados18/boot-services19/pong-xnu-initial.img 临时启动，Sending（2.322 秒）/ Booting（10.176 秒）均为 OKAY，完整实际回执记录在同目录 temporary-boot.json。本轮 Data、SystemOS、boot 和固件不重写，没有镜像散列或回读校验、没有额外测试。仅清理本轮可再生成的 FAT/cpio 打包输入以释放工作盘空间，最终启动镜像、原生预链接内核和 manifest 均保留。lsd 能否完成数据库初始化及后续界面结果仍需屏幕或 REC 日志确认。


## services19 日志与 services20：Cryptex 独立模块和 installd 存储路径

services19 REC 保存 400,881 字节 XNU console，未记录 kernel panic。lsd 已输出 writable single-user paths，未记录 lsd 退出；对 Data 中 LaunchServices 的有界文件读取发现 User/Library/Caches 下已建立 SystemDataOnly-com.apple.LaunchServices-6291460-v2.csstore 和应用保护 plist。此前的数据库初始化阻塞已有实际进展，不重置 Data。

新退出循环为 pkd（13 次 DYLD 缺库）、installd（13 次 OS_REASON_LIBSYSTEM，Failed to execute lookup query for daemon container）和 duetexpertd（10 次 SIGTRAP）。部分 console 字符损坏，保留原始 pstore；未将日志中的硬件变体拒绝、lsd 缺少 cpumon entitlement 警告等一律视作必须删除的启动限制。

pkd 缺少的 PlugInKitDaemon.framework/PlugInKitDaemon 在相同 22H374 SystemOS 上真实存在，而 OS 上同框架只有元数据。审计准备脚本发现原流程仅桥接 usr/lib，漏掉了其他 Cryptex 目录。新增通用缺失叶文件映射流程，覆盖 usr/lib、libexec、bin、sbin 和两个 Frameworks 目录，保留 OS 已有文件，遇到 OS 符号链接祖先时跳过以避免跟随 iOS 绝对别名进入主机。refresh_cryptex 对实际 OS 副本补入四个缺失文件链接：PlugInKitDaemon、BatteryAlgorithms、NeighborhoodActivityConduitIntents、VisualTestKit。链接指向实际挂载的 /private/preboot 对应文件，没有拷入别的版本库或替换存在的 OS 文件。四项记录见 prepared/cryptex-links-update.json。

解析真实 22H374 InstalledContentLibrary，将 installd 的致命文本定位到 MIDaemonConfiguration.daemonUserDataLibraryDirectory 的 class-10 com.apple.installd persona 查询失败分支。原方法构造 NSURL 并附加 Library；currentUserDataDirectory 和 currentUserCachesDirectory 继续附加 MobileInstallation、Caches。services20 只为原厂 installd 的这个方法提供实际 /private/var/installd/Library NSURL，动态匹配零参数/对象返回 ABI，不写私有字段、不复用 iOS16 地址。_installd uid/gid=33、home=/var/installd 来自本版 passwd；init 只创建缺失的目录并设置 33:33、0700，适配库还检查实际所有权与目录类型，失败时保留原路径和 fatal 行为。将 installd 纳入精确注入名单，仍使用原安装服务、签名检查、应用容器和 MachServices。后续真机日志仍需确认此修补是否越过退出。

duetexpertd 的 SIGTRAP 尚无足够栈证据归因于同一路径。本轮暂时排除已重复退出的可选预测任务，保留核心 SpringBoard、backboardd、lsd、pkd、installd、容器服务等原始端点，不伪造成功服务。未绕过原内核 fatal、未启用不匹配 Darwin24 的旧 Metal 用户态、未添加硬件 GPU 驱动。

services20 静态 init 和 UI 适配库已编译完成。继续沿用 services18 的原生 Darwin24 内核、8 CPU 和 4 软件 scanout worker；本轮实际修复均在用户态命名空间、可写目录和任务配置。启动包放到外置盘，避免已满的 PongDev 工作盘中途耗尽；本机 out/ipados18/boot-services20 保留入口。部署计划仅更新 OS，保留已生成数据库的 Data 和现有 SystemOS，不执行镜像散列、额外测试或整盘回读。


本轮 OS-only 更新已写入 6,845,104,128 字节并收到 13 个区块完成回执，安装状态 complete。随后从 REC 请求进入 bootloader，对 /Volumes/Untitled/Pong-iPadOS18/boot-services20/pong-xnu-initial.img 执行 Fastboot 临时启动，Sending（2.327 秒）和 Booting（10.178 秒）均为 OKAY；命令返回 0，总耗时 15.187 秒。完整回执位于 out/ipados18/boot-services20/temporary-boot.json，本轮源码和编译命令快照保存到同目录 source-runtime。手机是否越过 pkd / installd 退出循环以及是否显示界面仍待本轮屏幕或 REC 日志确认，未将 Fastboot 回执等同于已启动 UI。


## services20 日志与 services21：PlugInKit 状态容器和 Darwin24 路径审计

本轮 REC 保存 services20 XNU console 317,939 字节及 pmsg，未记录 kernel panic。日志中的 pong_revision=services20 与临时启动回执一致。SystemOS 按 UUID 选择 /dev/disk3s1，实际只读挂载在 /private/preboot（error=0）；Data 按 UUID 选择 /dev/disk5s1，实际可写挂载在 /private/var，8,589,934,592 字节。磁盘编号随枚举变化，内核继续使用 UUID，而非固定 diskN。不能将 REC 自身的 Linux /proc/mounts 当作 XNU 挂载结果。

installd 已加载适配库，输出 native installd uses writable _installd Library，未再记录前轮 daemon container 查询退出。对 Data 的有界目录元数据读取也确认 installd/Library、MobileInstallation 真实存在且 owner/group 为 33:33、0700。lsd 未退出，User/Library/Caches 又建立了常规 com.apple.LaunchServices-6291460-v2.csstore 和 OpenOperationStaging。此次保留这些运行状态和原有 Data，不清空数据库。

pkd 已越过前轮缺库错误，但随后 12 次 SIGABRT，原厂异常为 NSInternalInconsistencyException: unable to locate user container: error 52。匹配实际 22H374 PlugInKitDaemon 的符号与反汇编，异常返回地址相对该 image 的偏移 0xa038，对应 -[PKDAnnotationStore containerURL] 在 class-10 com.apple.pkd 查询失败后 raise 的返回点；调用者 initWithDatabase:externalProviders: 也对应日志栈。services21 仅为这个 daemon 注释状态目录提供实际 UID 所有的 NSURL：mobile 使用 /private/var/mobile/Library/Pong/PlugInKit，root 实例使用 /private/var/root/Library/Pong/PlugInKit。不替换 PKDContainerProvider 的应用容器或安全查询，不吞掉异常；仍运行原数据库、序列化、插件发现和 MachServices。方法按运行时零参数/对象返回 ABI 匹配，目录和 UID 检查失败则保留原厂错误。pkd 已加入静态 init 的精确注入名单，库和 init 均已编译完成。

路径审计确认本机准备 OS 的三个别名均已正确：/var -> private/var，/System/Cryptexes/OS -> /private/preboot，/System/Library/Caches/com.apple.dyld -> /private/preboot/System/Library/Caches/com.apple.dyld。新增 runtime_namespace 准备流程将这些别名和真实挂载父目录统一记录到 prepared/runtime-namespace.json，遇到非预期真实文件/目录则保留并报错，不跟随 iOS 绝对符号链接进入主机。本轮 changed=[]，没有假称修复原本正确的挂载点。匹配的 App Cryptex 尚未提供，不创建虚假的 App 挂载；不添加 macOS 的 /System/Volumes/Data 路径。既有四项 SystemOS 独立文件映射仍保留，本轮未发现新增缺失项。

源码中残留的 iOS16 可选绑定视图在 services20 实际都是 SKIP，尚无证据表明它们造成了本次停点。services21 用明确的 PONG_NATIVE_APFS18=1 编译分支，将 Data 内旧 boot-jobs、MetalExtensions、HAL 与壁纸覆盖排除出 iPadOS18 的执行路径，同时不再调用旧 APFS2142 用户态 Data mount fallback；继续使用已成功的 Darwin24 原生内核 UUID 挂载。原版 22H374 规范任务目录和 Version 7 缓存仍由 OS 提供，不从可写 Data 覆盖。

按用户要求，本轮额外排除明确访问未接入后端的可选任务：contextstored（17 次 AudioSession 查询失败及多次定位/媒体端点失败）、routined（重复定位同步/注册请求失败）、coreduetd（请求已经排除的 ProactiveSuggestionClientModel）、suggestd（请求未接入的 powerlog）。保留所有核心启动/显示/注册服务，不伪造端点或修改无证据的内核错误处理。logd 文件仍报告 logd_helper connection interrupted，此问题尚未定位，不能据此断言 Data 挂载失败，也未删除核心日志服务。

services21 使用 services18 原生 Darwin24 内核，8 CPU、4 软件 scanout worker 和匹配本版 QuartzCore 的 CPU 输出路径。新启动镜像已经打包到 /Volumes/Untitled/Pong-iPadOS18/boot-services21，项目 out/ipados18/boot-services21 提供同一入口。接下来只更新 OS 任务与运行时，再临时启动；没有镜像散列、额外测试或整盘回读校验。


services21 OS-only 更新已完整写入 6,845,104,128 字节并收到 13 个区块完成回执，Data、SystemOS、boot 和固件保留。随后从 REC 请求进入 bootloader，对 out/ipados18/boot-services21/pong-xnu-initial.img 执行临时启动，实际 Fastboot 返回 0：

```text
Sending 'boot.img' (100112 KB)                     OKAY [  2.326s]
Booting                                            OKAY [ 10.178s]
Finished. Total time: 15.156s
```

完整回执位于同目录 temporary-boot.json；源码、编译命令、路径合同和本轮审计记录保存到 source-runtime。没有额外测试、镜像散列或整盘回读校验。是否越过 pkd 状态容器退出及是否显示界面仍待本轮屏幕或 REC 日志确认。


## services21 日志、依赖审计与 services22

services21 REC 保存 210,805 字节 XNU console，未记录 kernel panic。launchd 日志确认 SpringBoard、backboardd 已进入 running；唯一明确的进程退出是 aggregated.addaily 正常 exit(0)。backboardd 输出 22H374 CPU compositor 和 BGRA scanout 已选择；读取服务日志未发现软件合成器缺失。touch/key HID 事件继续出现。`vm_shared_region_start_address() failed` 仅有 launchd.ios 一条，没有触发 panic 或后续启动服务退出，不将此孤立兼容查询当作本轮卡点。

services20 原来误排除了 22H374 的 audiomxd：它不是旧 iOS16 常见的 mediaserverd，而是这份 iPadOS18 restore 中 AudioSession、CoreMedia systemcontroller、routingcontext 等真实端点的发布者。services21 console 中此前反复缺少这些服务；同一 Version 7 cache 中 audiomxd 条目因 `#IfFeatureFlagEnabled=CoreAudioServices/audiomxd` 包住完整的 MachServices/PublishesEvents 而未被端口启用。services22 将其加入真实 core job 列表，在原厂可执行文件和 ABI 上展开这个明确标注的 22H374 条件段，保留其它 AirPlay 子级 feature flags，仍移除不支持 Exclaves 的 `_Conclave`。`jobs-report.json` 确认 audiomxd boot、52 个原厂服务 endpoint、PublishesEvents 已从实际条件段展开。

同时恢复 services21 中 lsd 请求的 `com.apple.managedconfiguration.profiled.public`。profiled 原厂 job 有两个 MachServices，当前设置为 on-demand，不再因过往另一容器错误被排除；jobs cache 记录中不存在缺失 core labels。实际依赖检查解析 SpringBoard、backboardd、lsd、pkd、audiomxd、profiled、containermanagerd、logd/logd_helper 和 PlugInKitDaemon 的 Mach-O load commands，并在匹配 22H374 shared cache 与 OS/SystemOS 中查找必要 dylib。没有必需直接导入缺失。两个 AppleInternal 弱链接（TapToRadarKit、liblinkguard）保留为可选项，不把它们误报为阻断启动的缺库。结果记录在 `out/ipados18/logs/boot-services21/direct-dependencies.json`。

准备脚本中的 OS 副本和目标设备分别符合实际挂载合同：SystemOS UUID D96FCEC3-6DE9-474F-8F38-E40FA084948B 挂载 `/private/preboot` 只读；Data UUID 952D79E9-0CD4-47B2-B817-22889BF8AC50 挂载 `/private/var` 可写；OS 根 UUID B07DF049-7AD9-47B5-9F8F-9EE728056398。`/var`、`/System/Cryptexes/OS` 和 shared-cache 别名都已正确，因此未误报为修复。App Cryptex 仍未提供，不制造它的路径或服务。

源树中剩下的 HackiOS DENY `audio` 还会排除 audiomxd 所需的物理 HAL 插件。services22 保留所有 OS 文件，只在启动时将实际 `/System/Library/Audio/Plug-Ins/HAL` 绑定到 root-owned 空目录；单独的 `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin` 不被覆盖。这样由真实 audiomxd 提供系统音频/CoreMedia IPC，同时不加载未经适配的板级音频驱动。Profiled 和 audiomxd 以外，仍只跳过有 SIGTRAP 或无可用服务后端证据的可选任务；核心 UI、数据库、安装、容器和电源任务保留。

### services22 OS 增量格式和部署

为了降低以后每次部署的 UFS 流量，首次在主机将原始 UDTO 转为可写 sparsebundle，耗时约 6 分 47 秒、15.3 MB/s；保留原始 `.cdr` 基线，逻辑窗口仍为 6,845,104,128 字节，APFS UUID 和 UFS 布局不变。后续用 sparsebundle band 修改时间/大小记录本轮变更，合并成三个 8 MiB 写区段；只将 25,165,824 字节写入 OS 窗口，约为整窗的 0.37%。Delta 安装器按 22H374 build、服务端设备序列号、用户数据块设备几何、现有 SHNGROOT 窗口和 sparsebundle 代次匹配后才执行写入。每段均收到设备写入回执，状态 complete；没有镜像散列或整镜像回读。数据记录位于 `out/ipados18/delta-services22/delta.json` 和 `install-services22/installation.json`。完整 OS 仍保留在 UFS 窗口中，稀疏格式只影响主机工作副本和增量传输。

services22 新 init/UI 适配库与原生 Darwin24 内核（services18 构建）已编译。OS 更新写入 services22 audiomxd/profiled 服务定义、native runtime 和空 HAL 视图；Data、SystemOS、boot 与固件未改。随后对 `out/ipados18/boot-services22/pong-xnu-initial.img` 执行 Fastboot 临时启动，Sending（2.324 秒）和 Booting（10.172 秒）均 OKAY，完整记录在同目录 `temporary-boot.json`。此回执仅确认临时启动请求被接受，界面状态与新日志仍待本轮 REC/屏幕确认。


## services22 REC 审计与 services23：收紧 Darwin24 UI 注入清单

services22 的 REC pstore 保存 211,734 字节 XNU console，未见 kernel panic。Darwin24 的 8 个 CPU 完成启动；APFS 载入成功，OS、只读 SystemOS 与 8 GiB 可写 Data 按 UUID 挂载到实际路径。launchd 将 SpringBoard、backboardd、lsd、audiomxd、profiled、installd、notifyd 和两个 containermanagerd 都推进到 running；没有核心 UI 或容器服务退出循环。触摸 HID 事件已进入内核。

服务日志仍有 424 次相同的 `lsd denied attempt to escape CPU monitor`。原厂 22H374 lsd 的签名 entitlement 中没有 `com.apple.private.kernel.override-cpumon`；进程保持运行，日志没有 lsd 退出或重启。因此这条是缺少可选 CPU-monitor override 权限的重复拒绝，不是本轮启动阻塞点；没有通过给系统服务额外授予内核权限来掩盖它。其余高频失败查询来自未接入的 CommCenter、Location 和 CarKit 平台端点，消费者仍运行；不虚构这些无线/定位服务的成功应答。

启动规则审计确认 iPadOS18 使用 22H374 的 Version 7 原厂 launchd cache，`audiomxd` 的 52 个实际 MachServices 和事件发布者已启用，`profiled` 仍是按需任务；没有把 iOS16 的 mediaserverd job、20G81 launchd cache 或旧 APFS/Metal/壁纸覆盖规则用于 Darwin24。launchd 对部分不属于 Pong 的硬件/OS 变体任务（例如 safetyalertsd、threadradiod、checkerboard、DarwinOS 专用诊断任务）拒绝导入，是对应的硬件和系统变体门控；NanoLaunchDaemons / LaunchAngels 目录在本 restore 中不存在，未创建伪造任务。它们没有阻止核心 job 导入。

显示路径审计以 pstore 和本机 22H374 IOMobileFramebuffer 二进制为依据：MiniIOMobileFramebuffer 发布 1080×2412、4320-byte row 的实体面板；22H374 QuartzCore 的 image UUID 与函数入口指令均命中，日志确认 CPU compositor 和 BGRA scanout 被选中；backboardd、SpringBoard 均进入 running，且有触摸事件。selector `0x0f` 在 22H374 框架中是调试标志设置，selector `0x58` 是 OpenByName 中的附加请求，调用者不检查其返回值后继续打开实体显示。它们虽在 MiniIOMobileFramebuffer 显示为 unsupported，但当前日志没有显示初始化中止或渲染回退失败；必要 swap / 尺寸 / 通知入口仍由驱动实现。REC 日志没有逐帧截图或显示内容校验，因此不能据此断言屏幕上每个图层都正确；当前没有证据要求更换 22H374 地址或复制 20G81 显示 ABI。

本轮发现一处遗留的 iOS16 适配名单：静态 init 无条件把 `mediaserverd` 放进 dylib 注入进程字符串，而 22H374 用 `audiomxd` 且没有 mediaserverd job；同时 `audiomxd` 和 `profiled` 被列入额外注入项，但 ui_compat18 没有对这两个进程执行任何适配。这只增加无用的 dyld constructor 和映像回调，并非已确认的卡点。services23 将 mediaserverd 注入限制在旧 iOS16 编译分支，并从 iPadOS18 的额外进程清单移除 audiomxd/profiled；它们原厂 launchd job 保持不变。保留真正需要运行时修补的 usermanagerd、lsd、installd、pkd、SpringBoard、backboardd 和容器管理进程。

本轮不改内核显示 ABI、服务包、Data 或 SystemOS。若后续仍无可见画面，下一步应依据屏幕照片或新的 compositor 日志检查实际 frame submission，而不是把两个非致命 selector、lsd 权限拒绝或无线端点缺失当成显示故障。

### services23 构建、OS-only 增量与临时启动

services23 只重编译 Darwin24 静态 init 和 22H374 UI 适配库，复用 services18 的原生内核。最终启动包为 8 CPU、22H374 UUID 匹配、预链接 APFS，镜像含 100,112 KiB payload；没有把完整 OS 放进启动镜像。构建清单确认不写分区，镜像哈希和读回均跳过。

OS-only UFS 增量写入收到 3 个区段完成回执，共 25,165,824 字节；逻辑 OS 窗口保持 6,845,104,128 字节。SystemOS、Data、boot 分区和固件没有写入。Fastboot 临时启动 `boot-services23/pong-xnu-initial.img` 返回成功：Sending 2.271 秒、Booting 10.173 秒，完整命令和回执保存于 `out/ipados18/boot-services23/temporary-boot.json`。临时启动后立即检查时 ADB/Fastboot 均未重新枚举，因此 Fastboot 回执只能证明一次性镜像交接成功，不能据此确认 iOS UI、屏幕渲染或后续启动阶段；services23 的屏幕/REC 运行结果待设备重新提供日志。


### services24：22H374 显示共享状态与旧内核规则审计

本轮先从 REC 重新保存原始 pstore（215,076 字节 console、50 字节 pmsg），设备仍处于 recovery。console 与已保存的 services23 阶段一致；没有新的 kernel panic 或核心服务退出循环，SpringBoard、backboardd 等进入 running，但没有足够证据确认 UI 出帧。原始字节保存于本机外置目录 `/Volumes/Untitled/Pong-iPadOS18/audit-services24/rec-pstore/`，字符损坏原样保留。

直接对照本机原厂 22H374 IOSurface.framework 调用后，修正实际参与构建的 MiniIOSurface：

- bulk attachments 请求由旧 `0x88` 改为 `0x90`；数据区最大 `0x74`，size/mask/surface ID 分别位于 `0x78/0x80/0x88`。补齐 18 个 dirty-mask 字段，先检查所有字段边界再修改共享状态，避免部分应用请求。
- 共享页 `+0x24` 是 wiring assertion count，不能写入 YCbCr matrix；色彩矩阵实际是 attachments `+0x3a`，即共享页 `+0x6a`。
- aux 包含 5 个 indexed timestamps，detachModeCode 在 `+0x28`。selector 33 实际写入指定时间戳；selector 43 实现位掩码更新与初始化时间戳，不再只返回成功。
- selector 24 的 serialized/event timestamp 与 33 的 indexed timestamp 不同；未实现的事件/所有权操作不再伪报完成。没有复制未知私有事务。
- 22H374 的 WrapClientImage 仍是 32 字节；结果头 `0x88`、plane stride `0x98`、最大 reply `0xc68` 与前轮适配一致，保留。framebuffer swap `0x514` 与原厂框架一致，保留。

内存/失败事务修复：foreign-task wrapped surface 映射失败后返回错误并撤销临时映射，避免把所有者地址交给其他进程；持有 wrapped owner task，释放时撤销引用；创建/lookup 输出失败时回滚新增客户端引用；避免释放 surface 后读取统计大小；宽几何参数在收窄前检查；保留 64 位 protection options；检查字典与 descriptor 输出分配/写入结果；初始化失败释放锁和 thread call；重用像素内存始终清零；四个闲置缓存条目各不超过 32 MiB，总保留上限 128 MiB（活动 surface 仍受原有限制）。

旧规则清理：Darwin24 原生 persona 已注册并在日志中成功使用；原厂 launchd 具备 persona-mgmt entitlement，因此恢复原生 entitlement/缺失 persona 的处理，删除缺失对象时直接改 credential 的旧旁路。恢复 Darwin24 每次 exec 更新 pid identity version 的逻辑，删除 xpcproxy 名称特判。删除未实现 work_interval op 6–8 的空成功分支，保留原生操作；CA_CLIENT 的开发平台兼容仅限定 Pong，重复 CA_CLIENT 恢复原生错误。删除按 basename/文件偏移、不绑定映像 UUID 的旧文本 fixup 解析器及 VM fault 写页钩子；保留原厂 16 KiB 签名到 4 KiB 内核页的兼容缓存，并修正其分配失败路径。

编译配置继续使用 Darwin24.6/xnu-11417.140.69、原厂 22H374 kext 及实际 OS/SystemOS/Data UUID；不链接旧 20G81 Metal/IOAccelerator 档案。当前显示 backend 是 CPU/BGRA，不能据此宣称 iPadOS18 Metal 加速已实现。此轮仅更换内核，设备上的 services23 userland、Data 和 SystemOS 保留；不传输完整 OS，不执行镜像哈希、镜像审计或设备镜像读回。

上述改动是源码/ABI 审计发现并修正的问题；不能把它们全部认定为已证实的本轮无界面原因。编译和临时启动结果在后续记录。

补充：wrapped descriptor 为 kIODirectionOut 时，跨进程像素映射明确使用 kIOMapReadOnly，避免 Darwin24 尝试建立可写映射。构建器使用 BUILD_STATIC_LINK=0；本机预链接器直接消费完整 mach.development.pong，不使用 KDK 的额外静态 archive，因此省去该归档输出，保留正常内核链接和符号。


### services24 构建与临时启动回执

最终增量构建成功，日志为 `/Volumes/PongDev/build/Pong-iPadOS18-native/build-20261024T183711Z.log`；MiniIOSurface 在最后只读映射修正后重新编译，完整内核已重新链接与导出。使用 22H374 原厂闭包预链接后生成 102,514,688 字节启动镜像，Pong ADT 为 8 CPU、12 GiB 物理 DRAM、4 GiB 内核连续窗口，未装入 Data RAM 盘。保留原有 services23 用户态与持久 UFS Data，没有 OS/SystemOS/Data 传输或分区写入。

已由 REC 执行 ADB reboot bootloader，并对 dd72e429 临时启动本轮 `out/ipados18/boot-services24/pong-xnu-initial.img`：Fastboot Sending（2.328 秒）与 Booting（10.124 秒）均为 OKAY，完成时间为 2026-10-24 18:44:11 UTC。完整命令与回执保存于同目录 `temporary-boot.json`。该结果证明临时镜像交接被接受；iPadOS18 UI 是否出帧、后续服务状态仍待本轮屏幕/REC 日志确认，不能以 Fastboot OKAY 代替界面确认。


### services25：继续审计非 bulk 属性、显示回调与输出事务

services24 临时启动后，当前 ADB/Fastboot 均未枚举，尚无新 REC 日志；未将无 USB 枚举当成自动重启或卡死。继续从实际编译源与本机原厂 22H374 IOSurface.framework 取证。

`IOSurfaceClientSetValue` 将序列化内容放到请求 `+0x0c`；RemoveValue 的字符串和 CopyValue 的空 key 也在 `+0x0c`。`IOSurfaceClientUpdateNonBulkValueCache` 明确从输出 `+0x0c` 反序列化并在 `+4` 读取 seed。旧适配全部按 8 字节头部读写，已改为 12 字节，并修正共享 valuesSize 对应的完整回复长度；空字典的竞态读取也返回有效的空字典序列化结果。

原厂框架还调用 trap 9 删除 bulk 字段、trap 8 删除 CoreVideo bridged values，以及 selector 54 注册 bridged keys；旧驱动只发布 0–5 六个 trap。services25 实现字段掩码清理、每个连接的 bridged key 注册及对应属性/颜色清理；6、7 未实现的 trap 空槽明确拒绝，不提供空函数指针。创建接口确认带一个 parent-ID scalar；非零 parent 需要真实共享父对象存储，当前明确不支持，停止默默另建无关像素缓冲的行为。

属性变更现先复制字典，完成 set/merge/remove 和序列化大小计算，再一次发布字典、长度与 seed。分配或序列化失败时保留原状态；输出 seed 的缓冲区在变更前检查。不是通过记录日志代替真实操作。

显示回调修正三处生命周期问题：OSAsyncReference64 先清零，避免未初始化的保留字段进入通知消息；释放对象前先禁止 timer 自身重新排队；发送 VSync 时在锁内复制独立 send right，避免注册另一通知端口后旧指针已释放。发送后释放临时 right，不改变实际刷新率或假报帧已完成。

删除缩放路径的旧 iPhone X 375×812 固定点数；显式 fbscale 现按 Pong 面板像素与 DT display-scale 计算，宽高使用 64 位中间值并保持 IOSurface 尺寸范围。正常未指定 fbscale 的物理面板路径保持原生尺寸。`/product` 是 Apple DT 中的节点，不是 Android 分区挂载。

软件键袋的输出 descriptor 同样检查 writeBytes/complete 的结果，只在完成时发布输出长度及 wrap class；多核访问 notify state 改为原子读写。未借此宣称有 SEP 或硬件密钥支持，也未复制未知 iPadOS18 私有请求。

所有修补进入 `patches/xnu-ipados18/0001-pong-darwin24.patch`；本轮仍需编译和运行结果确认。当前手机上的镜像仍为 services24，未重启正在运行的手机。


### services25 用户态与构建产物

额外修正 `ui_compat18.c`：persona.kb 改为在 root-only 目录创建 0600 临时文件，写完、fsync/close 成功后以不覆盖现有文件的 hard link 原子发布，最后移除临时名字；中断时不会留下半写入的最终注册文件。未重建、覆盖或删除现有 persona 注册表。

22H374 QuartzCore 的两个修补点在原 UUID/入口指令匹配之外，还要求落入该 image 的可读/可执行 Mach-O segment，并先检查 offset/页边界；没有换用 iOS16 地址。原厂 image slide 由 dyld add-image 回调提供。

内核已通过最终编译与链接：`build-20261024T185712Z.log`。编译时已将未导出给 IOKit 的 IPC 私有头引用替换为 Darwin24 现有的 `IOKitServer.h` / `ipc_port_copy_send_mqueue` 接口。静态 init 与 UI adapter 也已编译，产物位于 `out/ipados18/runtime-services25/`。预链接临时启动包位于 `out/ipados18/boot-services25/pong-xnu-initial.img`，仍为 8 CPU、原生 UFS Data、22H374 组件，镜像校验按要求跳过。

本机 prepared OS 副本已更新运行时；只生成 3 个改动 band，共 25,165,824 字节的 OS 增量（`out/ipados18/delta-services25/delta.json`）。逻辑 OS 容量未变，没有清理系统包或修改 Data/SystemOS。该增量基于 services23 的实际安装回执；services24 只更换内核，因此仍是正确基线。

本轮结束前电脑仍未枚举 ADB/Fastboot，services25 尚未写入设备或临时启动。手机最后一次实际临时启动的仍是 services24。需要在 REC 安装这份 OS-only 增量后，再启动 services25 并取得新日志；当前不能把源码修正或编译通过当作 UI/事务运行结果已确认。


### services26：缩放行缓冲、离屏矩形与显示初始化事务

本轮起始时 ADB/Fastboot 仍未枚举，不能读取新的 REC 日志，也没有重启手机。以 services25 的实际源码为基准继续审计，确认以下原有实现中的错误：

- 双线性缩放的 primary/helper 行缓冲逐个分配，失败时将 fBlendRowSize 清零，却留下部分已经分配的 helper 指针。下一次增长会以错误容量释放这些指针。现先分配整组新缓冲，全部成功后才替换；任何失败都按新容量释放暂存缓冲，保留旧指针与旧容量。本帧旧容量不足时不使用它做滤波。
- 完全离屏的水平矩形可能形成空或反向区间，后续仍读取 fColumn[cx0]/fColumn[cx1-1]。现在先判断实际交集，空区域直接返回；源/目标端点先用 64 位计算，排除 32 位溢出和不可表示的跨度。
- 原循环会遍历巨大的离屏 y 范围再逐行跳过，不能把这种长循环误当设备死锁。现在先裁剪可见行，多核按可见范围分工；fHelpersStarted 的读取也与写入使用同一把锁。
- 零尺寸或无效首帧原先仍会释放文字控制台。现在只有经过矩形/范围检查、确实有可绘制像素的首帧才切换到图形模式。
- 显示模式的字典/数组/OSNumber 分配结果原来未完整检查，并且在模式发布前已经启动永久绘制线程。现在完整准备模式与 DT 节点，再启动 workers 和注册服务；失败路径释放列缓冲、锁和暂存节点，不发布缺字段模式或残缺节点。

并重新对照本机原厂 22H374 IOMobileFramebuffer：GetDisplaySize 仍是两个 uint64 标量，框架随后 ucvtf 转成 CGSize；GetDotPitch 是一个标量；SetDisplayDevice 是一个设备编号；GetDisplayArea 仍请求 8 字节结构。因此没有因为注释提到 20G81 就删除已确认匹配的接口，只更新与现有证据一致的注释。

本轮只改内核 framebuffer 源码。services25 的用户态和 24 MiB OS 增量继续使用，未生成另一个 OS 基线，也没有触碰手机分区。上述问题来自源码路径证据，不等同于确认它们就是 services24 当前屏幕状态的唯一原因。编译与部署结果在后续记录。

### services26 构建结果与部署边界

最终内核编译、链接及 22H374 kext 预链接均完成。内核日志为 `/Volumes/PongDev/build/Pong-iPadOS18-native/build-20261024T191746Z.log`；临时镜像为 `out/ipados18/boot-services26/pong-xnu-initial.img`，102,514,688 字节。构建报告继续使用 Darwin24.6 / xnu-11417.140.69、8 CPU、原生 UFS Data；所有旧 20G81 GPU/legacy display 档案、缓存 carrier、union SystemOS 和旧 uaccess 配置关闭。没有执行镜像哈希、镜像读回或性能测试。

额外检查实际旧规则的可达性：NativeSandboxGrantProcess 仅由当前未链接的 PongAdreno 设置；离线 IOGPU/mobile graphics 探针需要显式 boot argument，本启动包没有这些参数；静态 init 中旧 mediaserverd、Metal extensions 与壁纸覆盖均在非 PONG_NATIVE_APFS18 分支，不会用于本构建。IOMemoryDescriptor 的 managed-page backtrace 抑制仅影响开发日志，不更改映射、权限或缓存策略；没有将它误认作图形加速启用条件。

最后可读取的启动记录仍为 services23：核心 UI 服务已 running，后续包含非致命 locationd/carkit lookup 和一次 powerlog group container 错误；没有由这些错误推出必须禁用对应服务，也没有把 running 状态当作画面完成。services24 的新 REC 记录尚未取得。

打包完成后 ADB（端口 5038）和 Fastboot 再次均未枚举设备，因此 services26 尚未安装或临时启动。下次连接 REC 时先应用 `out/ipados18/delta-services25/delta.json` 的 3 个 OS band（共 25,165,824 字节），保留 SystemOS/Data，再临时启动 services26。手机最后一次实际接受的仍是 services24；不能直接跳过用户态增量而声称 services25/26 的全部修复已在设备生效。

### services27：首帧交接后移除屏幕日志

用户恢复目标，并要求界面启动后屏幕日志随之消失。设备本轮已重新枚举为 REC，但 `/sys/fs/pstore` 已挂载且为空，没有取得新的 XNU 日志；原始目录清单及时间记录保存在外置 `audit-services27/rec-pstore/`，未用旧记录冒充新日志。

对照 iOS16 的 Pong touch-console 实现，保留 `kPEGraphicsMode` 后紧接 `kPEReleaseScreen` 的交接顺序。Darwin24 的前者只改变模式，后者才调用 gc_enable(FALSE)、同步关闭字符绘制；不应仅去掉 -v 或在某个服务 running 时提前停字。

原交接虽然停止后续文字，但不会清除局部 UI 图层之外已经画出的字符。新增唯一的 beginGraphics 入口：在实际有效、与屏幕有交集的首个图层准备绘制时，释放控制台，再按实体面板的 width/height/rowbytes 清一次整屏，随后绘制该图层。后续帧不重复清屏。CPU 路径和编译关闭的 GPU scanout 路径共用此入口，避免以后启用 GPU 时遗漏控制台交接。

未收到有效图层时仍保留启动文字。真实 panic 的原生处理未被屏蔽。console_write / console_write_unbuffered 的 ramoops 写入位于 disableConsoleOutput 判断之前，因此图形交接仅停止屏幕绘字，保留现有日志记录通路。此次没有改变 ramoops 的存储格式或宣称已恢复当前缺失记录。

改动已导出到 Darwin24 源码补丁；编译、安装和临时启动结果在后续记录。

services27 最终编译/链接成功，日志为 `/Volumes/PongDev/build/Pong-iPadOS18-native/build-20261024T232055Z.log`；原厂 22H374 kext 预链接及 8 CPU 启动包生成完成。REC 已完成 services25 配套用户态的 3 个 OS band 增量写入，共 25,165,824 字节，均收到设备完成回执；SystemOS 和 Data 未重写。安装回执为 `out/ipados18/install-services27/installation.json`，OS 增量基线已推进，pending 已清除。

随后从 REC 进入 Fastboot，并临时启动 `out/ipados18/boot-services27/pong-xnu-initial.img`；Sending 100,112 KiB（2.333 秒）和 Booting（10.187 秒）均为 OKAY，总用时 12.615 秒，没有 flash 分区。完整命令及回执在同目录 `temporary-boot.json`。本轮 services25、26 的累积内核修复和服务25用户态增量首次一起部署；画面能否出现以及首次可见图层后文字是否完全消失，仍需屏幕或新一轮 REC 结果确认。

### services28：真实像素布局与 22H374 plane 数量

services27 临时启动后的本轮连接检查中 ADB/Fastboot 均未枚举，尚无新的 REC 日志；没有重新启动或替换正在运行的 services27。继续检查实际源码的像素内存布局与引用事务，发现以下问题并修正：

- 旧 framebuffer 只凭 bytesPerElement=4、planeCount=0 就按每像素四字节读取。IOSurface 的 elementWidth/elementHeight 可以表示多个像素组成的块，创建器按块计算的行跨度可能小于 width*4，因此该条件不足以保证每行读取安全。新增统一线性 BGRA 检查：元素必须为 1×1、行跨度足够、实际可见最后一行在 allocSize-offset 范围内，才进入 CPU 读取和首帧控制台交接。通过剩余容量除以行跨度判断行数，避免大尺寸乘加溢出。GPU scanout 的编译分支同样要求该范围合法且表面覆盖实体面板，当前 GPU 配置仍关闭。
- IOSurfaceCreate 遗留规则把 w30r/l10r/w30R 数据改名为 BGRA，还将 b3a8 的 plane 信息丢弃；规则没有转换原像素字节。现保留真实 format/plane。显示输出的 BGRA 选择由已绑定原厂 22H374 UUID/入口的 QuartzCore 格式适配完成，而非让任意客户端表面的描述与存储不符。此变更不等于新增 packed-10-bit scanout 转换器。
- 原厂 `__ioSurfaceClientCreateWithLockResult` 在 0x1a66cb180 使用 20、在 0x1a66cb188 使用 0x98 来计算回复大小，上限为 0xc68。内部旧四 plane 限制已同步到 20，并用同一常量参与最大回复的编译期断言；总分配上限与各 plane 范围检查保留。
- use-count 的客户端计数和表面合计都在递增前检查 UINT32_MAX，避免回绕后 dropRef 再做减法破坏共享计数。清理不可达的重复 child-ID 分支；未假报实现 child alias。

本轮只修改内核源码，services25 用户态已在上一轮安装，无需新的 OS/Data 传输。原 services27 源码快照保存在外置 `audit-services28/before/`。以上为源码和本机原厂 ABI 证据，尚无新一轮运行结果。

services28 内核最终编译/链接成功（`build-20261024T232835Z.log`），22H374 组件预链接及 8 CPU 临时启动包已生成于 `out/ipados18/boot-services28/pong-xnu-initial.img`。本轮未执行测试、镜像哈希、设备镜像读回或新一轮 OS 写入。结束前 ADB/Fastboot 均未枚举，尚未临时启动 services28；手机最后实际接受的仍为 services27，待其屏幕反馈或手动回 REC 后读取运行结果。

### services29：服务发布失败回滚与多核统计清理

本轮设备仍未通过 ADB/Fastboot 枚举，未取得 services27 的运行结果。继续检查实际服务启动的可失败操作，修复 IOCoreSurfaceRoot 与 AppleKeyStore 在关键属性写入失败后仍 registerService 的路径：只有 user-client class 及软件 keybag 描述准备完成才发布；否则平衡基类 start/stop、返回失败，由已有调用者 detach/release。IOSurface 初始化失败时，现有 native_pong_ui_services_start 会继续跳过依赖它的 framebuffer，清理全局锁与 thread call；不会留下可枚举却打不开客户端的残缺服务。

同时删除旧的“大表面创建耗时”周期打印。该分支在并发 methodCreate 中无锁修改和清零 static count/since/timing，再用 count 计算平均值；这不是可靠的多核统计。连同只供它使用的 eviction 诊断计数与局部计时一起移除，保留表面内存复用与清零行为。MiniIOSurfaceCost 保留的总耗时现用原子 exchange 取走，避免普通清零与 OSAddAtomic64 的更新竞争；复用次数的写入也与原有原子读取一致。

没有因此修改 keybag 的磁盘格式、伪造 SEP 功能、关闭原生 panic、扩大图形内存限额或更换 22H374 地址。以上是启动失败事务和多核代码审计结果，不代表已确认服务27的界面状态。源码快照保存在外置 `audit-services29/before/`，最终编译和打包结果在后续记录。

最终构建完成后，手机重新枚举为 REC。本轮实际保存的 pstore 仅有 50 字节 pmsg，内容为损坏的旧诊断头，没有 console，也没有 services27 的版本标识，因此不能从它推断启动停点或宣称上一轮已正常出帧。原始字节、目录清单和 Linux dmesg 保存在外置 `audit-services29/rec-pstore/`。实时 DT 与 Linux 日志确认 ramoops 位于 0x85200000，4 MiB，console/pmsg 各 2 MiB、mem-type=2、ECC=0，地址和分区大小与当前实现相符；没有据损坏文字猜测并修改存储地址或纠错策略。

服务发布修复先完成一轮编译，统计清理加入后再次完成最终编译/链接；本轮使用最终日志 `/Volumes/PongDev/build/Pong-iPadOS18-native/build-20261024T233626Z.log` 对应的内核。预链接的 services29 启动镜像已生成，包含 services28 的像素布局、plane 数量与格式描述修正，以及 services27 首帧清屏交接。

从 REC 进入 Fastboot 后，已临时启动 `out/ipados18/boot-services29/pong-xnu-initial.img`：Sending 100,112 KiB（2.329 秒）、Booting（10.174 秒）均为 OKAY，总用时 12.599 秒。没有新的 OS/SystemOS/Data 写入，沿用 services27 安装回执中的 services25 用户态；命令和结果保存在同目录 `temporary-boot.json`。services28 未单独启动，其修复现随 services29 部署。实际 UI、无日志叠加以及后续服务仍需本轮屏幕或 REC 结果确认。

### services30：MCM 系统组容器父目录与 powerlog 服务

services29 的 REC console 记录 `profiled` 请求 `systemgroup.com.apple.powerlog` 后得到 error 52；22H374 将其定义为 `XPC_INTERRUPTED_REPLY`。同一 pstore 中 `containermanagerd` 和 `containermanagerd_system` 都已进入 running，Data 中有 root MCM 迁移状态。Data 的 `mobile/Library/MobileContainerManager/references.sqlite3` 虽然存在，但后续查看其表结构发现它是代码签名引用数据库，不是系统组容器映射表，不能作为容器已注册的证据。只读解析当前 Data APFS 目录树后发现 `/private/var/containers/Shared` 为空，缺少原生系统组容器父目录 `/private/var/containers/Shared/SystemGroup`；同时 `/private/var/containers/Data/System` 也缺失。此布局缺口会影响依赖系统组与系统容器的其它服务。

services30 在静态 init 挂载并确认可写 Data 后、启动 launchd 前创建这两个 root:wheel 0755 父目录；不会创建特定 UUID 子容器、写造 MCM 数据库或替换现有内容。构建器的 Data seed 也纳入相同目录，供后续新 seed 使用。原厂 `PerfPowerServices` job 因旧 error 52 曾被完全排除，导致 `com.apple.powerlog.plxpclogger.xpc` 端点缺失；本轮将它恢复为核心 provider 并保留 22H374 MachServices，`profiled` 仍按需启动。`powerlogHelperd` 是另一程序，原厂 importer 对其一个通知字段的警告与该容器请求不是同一个故障。

同一 pstore 的 Data `logd.0.log` 还记录 `logd_helper` 连接被中断，但本轮 console 显示该进程启动并保持 running，现有证据不足以把它归因于 MCM 目录缺失；保留为独立的 XPC 故障线索，未通过屏蔽核心日志服务处理。当前新增代码已合并进 services30 启动包，系统/数据镜像校验按要求跳过。实际启动后是否消除 powerlog error 52、恢复 plxpclogger 与界面进展，需看 services30 新 REC 日志确认。

services30 用户态 helper 已重新编译；启动包复用当前完成构建的 Darwin24 内核 services29，新增的 init 父目录初始化和原厂 PerfPowerServices 服务定义均进入启动镜像。OS 增量基于 services27 安装回执，只改 4 个 band、共 33,554,432 字节；REC 写入回执完成，SystemOS 与 Data 未写入。没有镜像哈希或读回校验。

随后以 `fastboot boot out/ipados18/boot-services30/pong-xnu-initial.img` 临时启动：Sending 100,112 KiB（2.307 秒）和 Booting（10.177 秒）均为 OKAY，总用时 12.585 秒；没有 flash 分区。后续 REC pstore 显示 Data 可写、SpringBoard/backboardd 运行，但 `profiled` 的 powerlog group 请求仍为 error 52，PerfPowerServices 按 KeepAlive 每 9–10 秒重试；CPU 合成 hook 已选中但没有 framebuffer 首帧或 swap。services31 按这些新证据调整重试策略和 Setup 标记，详见下节。

### services31：完整跳过 Buddy、抑制 PerfPower 重启循环

services30 的新 REC console 证明 APFS Data 可写且上述两个容器父目录已创建，但 `/containers/Shared/SystemGroup` 仍为空。`profiled` 在 17:05:18.834 仍报告 `systemgroup.com.apple.powerlog : 52`；两个 containermanagerd 都已进入 running。日志只出现这个 systemgroup 查询错误；Data 的代码签名引用库不是容器索引，因此没有伪造 UUID 目录或数据库记录。Data `logd.0.log` 中 `logd` 长时间报告 `logd_helper` XPC connection interrupted，而 `logd_helper` 本身保持 running；这是另一条 XPC 失败线索，不是同一个 MCM 错误。22H374 Pong 配置的 `config/MASTER.arm64.pong` 已从 FILESYS_BASE 排除 `config_ext_resolver`，Sweet 的 ACL resolver 修补因此不是这次缺少的配置。

`PerfPowerServices` 原厂 job 带有 `KeepAlive=true` 和每日 `LaunchEvents`。services30 中它因 inefficient 被启动，随后多次 `exit(1)`，launchd 每 9–10 秒重试。services31 将它从 CORE 移到 on-demand：原厂 `MachServices`（包括 `com.apple.powerlog.plxpclogger.xpc`）保留，按需任务导入时去掉 KeepAlive 和 LaunchEvents，不再定时重启。它的单次启动仍可能因 52 退出，MCM 请求本身是否恢复要看新的 REC 记录。

同一份 Data 快照中的主 PurpleBuddy plist 只有 `SetupDone=true`、`SetupVersion=11`、`ForceNoBuddy=true`，却残留 `SetupState=SetupUsingAssistant`。Pong 的 seed 还没有加入 Sweet/iOS16 使用的 `SetupFinishedAllSteps=true`、`daemonizeNeedsToRun=false` 和 mini-buddy not-backed-up 偏好。console 没有 Buddy/SetupAssistant 进程记录，故不能说它已被证明卡住启动；services31 在首次运行时一次性原子更新 Buddy 偏好、移除陈旧 SetupState，并仅在 SpringBoard 偏好文件不存在时补入 `BKNoWatchdogs` seed。Data 既有内容不重建，后续启动由 marker 避免覆盖用户偏好。

CPU 显示状态也已按 services30 全量 pstore 核对：bootargs 有 `cpus=8`、`fb_threads=4` 和 `pong_ui_software=1`；QuartzCore runtime hook 记录 `stock CPU compositor selected` 与 `software scanout BGRA selected`，触控输入和 SpringBoard/backboardd 均在运行。可是 pstore 没有 framebuffer display-start/first-visible-frame，也没有 selector 3/4/5（default surface / swap begin / swap end）或 swap 记录；backboardd 之后还报告线程连续 180 秒 CPU 使用率超过 50%。因此 CPU 回退已选择且在消耗 CPU，但没有证据证明它提交过一帧，不能称为可正常渲染。Setup 标记完善后需要新的帧日志确认，不能把 8 核或 `fb_threads` 当成显示成功。

services31 用户态 init、UI 库和 plist seed 编译完成。服务定义与 runtime 已进入 services31 准备 OS；基于 services30 安装回执的 OS 增量为 4 个稀疏 band、共 33,554,432 字节，并已通过 REC 写入回执。SystemOS 和 Data 没有重写，未执行镜像哈希或设备读回。OS storage revision 已推进到 services31。

随后以 `fastboot boot out/ipados18/boot-services31/pong-xnu-initial.img` 临时启动：Sending 100,112 KiB（2.309 秒）和 Booting（10.174 秒）均为 OKAY，总用时 12.553 秒；没有写启动分区。镜像复用 services29 Darwin24 内核并带 `pong_revision=services31`、8 CPU 与 framebuffer 日志开关，构建跳过镜像校验。Fastboot 返回后，ADB 与 Fastboot 在 30 秒检查点均未重新枚举，因此本轮屏幕结果和新 pstore 尚未读取；不能据此确认 setup skip、powerlog、CPU 合成或首帧已解决。


### services32：22H374 SpringBoard Setup 策略与有限线程快照

services31 返回 REC 后取得 console 443,495 字节，原始字节保存在外置 `audit-services31-after-boot/rec-pstore/`。这份 console 有大量单比特/控制字符损坏，不能将损坏行当成完整调用证据；未发现可辨识的 kernel panic。只读解析当前手机 Data 后，确认 `.pong-setup-bypass-22H374` marker、主 PurpleBuddy 的 SetupFinishedAllSteps/daemonizeNeedsToRun、mini-buddy 偏好均已实际写入，且没有残留 SetupState。系统组父目录存在但仍为空，不能称 MCM 已正常创建容器。`logd` 仍记录 helper 连接中断；本轮 console 显示 lsd 的一个线程在 180 秒内消耗超过 50% CPU，不能将它当作 UI 合成耗时的证明。

framebuffer 可辨识调用仍只有初始化阶段的 57、15 和 22，尚无 selector 3/4/5 或首帧记录；SpringBoard/backboardd 已创建。services31 中 PerfPowerServices 虽没有 KeepAlive，仍由排队的 Mach 请求反复启动并 exit(1)，故“移除 KeepAlive 即消除重启循环”并未成立。services32 保留原厂 MachServices 和真实失败，把 ThrottleInterval 提高到至少 60 秒，减少启动阶段每 10 秒一次的进程 churn；没有宣称修复容器 XPC 本身。

对比已工作的 iOS16 UI 策略，发现 18.x 适配库只有电池、容器和软件输出策略，尚未纳入 SpringBoard Setup/激活界面策略。重新查看 22H374 自身 SpringBoard.framework 的 Objective-C 元数据和方法：`-[SBLockdownManager _setupActivationState]` 为 `v16@0:8`，其 Activated/FactoryActivated 路径写本地 int state=2；`-[SBSetupManager _setSetupRequiredReason:]` 为 `B24@0:8Q16`。services32 只在 SpringBoard 和显式 `pong_skip_setup=1` 下应用同样的开发界面策略：运行时检查 `_state` 的类型、实际偏移及对象范围；原版 setup reason 转换函数继续执行并保留返回值/通知。未使用 20G81 固定偏移，未改 mobileactivationd 或伪造 SEP/激活服务应答。Setup gate 是本轮明确遗漏的策略，但现有日志尚不能证明它是无首帧的唯一原因。

另以 `pong_boot_snapshot=1` 启用两次有限的进程内线程快照（启动后 35 秒、90 秒），覆盖 SpringBoard、backboardd、lsd 和两种 containermanagerd。最多记录各进程前 16 个线程和每线程 16 个帧，保存寄存器、运行状态、CPU 使用率、cache 基址及 image slide 到 UID 可写的 `/private/var/logs/PongBoot-*.txt` 或 `/private/var/mobile/Library/Logs/PongBoot-*.txt`；之后线程退出。不增加无限探针、不显式 suspend/resume、不修改渲染/服务应答，读取失败保留真实错误。目的在于从 REC 得到阻塞函数，不再用“进程已创建”或“CPU 策略已选中”推断已绘制画面。

用户态 init/UI 库正常编译完成，包含新源文件 `boot_snapshot18.c`。沿用 services29 已完成构建的 Darwin24 内核和 services31 的现有 Data；本轮没有内核代码变更、镜像哈希或设备镜像读回校验。OS 增量安装、临时启动及运行结果在后续记录。


services32 的 OS job/runtime 更新已完成；以 services31 回执为基线的增量仍为 4 个 band、33,554,432 字节，3 段写入均收到完成回执，SystemOS/Data 未重写。首次本机打包因 PongDev 空间不足在写最终镜像时失败，留下的零字节未完成镜像已删除；本轮生成目录整体移到 `/Volumes/Untitled/Pong-iPadOS18/boot-services32`，项目 out 路径为同目录符号链接。重试完成的最终镜像为 102,514,688 字节，带 8 CPU、4 个 scanout workers、显式 setup/snapshot 参数，跳过镜像校验。较早失败日志保留；只使用重试完成的镜像临时启动。


本轮已从 REC 进入 Fastboot 并临时启动 services32 最终镜像：Sending 100,112 KiB（2.306 秒）、Booting（10.176 秒）均 OKAY，总用时 12.563 秒。命令及实际输出保存在 `out/ipados18/boot-services32/temporary-boot.json`；未 flash 启动分区。尚未取得本轮屏幕/REC 结果，不能确认 UI 已出帧、Setup gate 是唯一阻塞点，或 MCM/XPC 已恢复；需要保持运行至少 90 秒后读取 Data 中的两次线程快照。


### services33：移除 PerfPowerServices、补回 RunningBoard 资源并清理旧 recipe

services32 返回 REC 后，pstore 只保留 50 字节损坏 pmsg，没有本轮 console；不能据此判断是否 panic。已从手机 Data 的 APFS 目录树读取 35 秒和 90 秒两组进程内快照。SpringBoard 主线程停在 RunningBoard 的 acquireAssertion/连接等待，backboardd 停在 RunningBoard 进程监视连接，进而阻塞 QuartzCore shared_server_init。两者工作线程均在 RBSConnection 的原生 handshake block 中 sleep 后重试。这说明当前没有首帧的直接阻塞点在服务握手，尚无依据把它归因于 CPU 渲染太慢。原始快照和符号分析位于外置 audit-services32-after-boot；没有镜像散列或设备镜像读回。

按用户要求，将 com.apple.PerfPowerServices 从任务目录与 Version 7 原生服务缓存排除，不再保留其 MachServices 触发路径。没有删除被其它系统组件链接的框架，原厂 restore 源保留。新启动参数 applog_name 也移除了它。

发现两类实际的 recipe 遗留。第一，18.x 构建器此前读入 iOS16 mkjobs.py 的子串 DENY，连同 keybagd、lockdown 等 native 软件服务一并筛掉。现改为按 22H374 完整服务名描述未移植硬件和已观察到的可选错误，恢复符合原厂可执行路径/MachServices 条件的软件服务。第二，jobs() 将 LaunchDaemons 下所有 plist 都当任务处理，因无 Program/Label 把 com.apple.jetsamproperties.J171/J172.plist 删除。22H374 RunningBoard 的 RBJetsamPropertyManager 直接按 hw.targettype 从该目录读取此 Version4 内存策略；当前 ADT 使用 J171。现原样保留这两份原厂配置，作为资源复制，不伪造 launchd 任务，也不加入服务缓存。这是确定的配置缺口，但尚不能据静态证据断言它是握手失败的唯一原因。

静态 init 删除不再使用的 APFS 2142 用户态挂载参数块、旧 MetalExtensions/壁纸覆盖与非 native18 挂载分支；本文件现在要求 PONG_NATIVE_APFS18，UI 库缺省路径为 libpong-ui18.dylib。内核 helper allowlist 只保留当前 /pong/init，删除旧 /private/var/pong/init 入口与旧 uaccess 打印分支。Darwin24 构建器不再从 iOS16-v45 树复制脚本，而使用当前分支的构建脚本。现有 UEFI/SM8475 硬件支持和真实 22H374 launchd_rd=md0 策略保留；后者已在原厂 22H374 launchd 中确认，不因名字沿用就删除。旧 20G81 GPU 私有档案仍未链接。

RunningBoard 本身加入现有的两次有限线程快照，并将其真实 stderr 保存到 Data 的 PongService-runningboardd.log，便于下一轮在 pstore 缺失时读取服务端原因；没有替换握手回复或假报服务成功。最终用户态 init/UI 库编译完成，内核编译/链接完成（build-20261025T015237Z.log）。任务计数为 boot 23、on-demand 454、excluded 104，另保留 2 份 jetsam 配置资源。部署和临时启动结果在后续记录。


services33 最终镜像已生成于 out/ipados18/boot-services33/pong-xnu-initial.img（实际位于外置磁盘同名目录），大小 102,514,688 字节。用户态与 jobs 更新完成后，基于 services32 回执的 OS 增量为 5 个 band、41,943,040 字节，3 个写入区间均得到设备完成回执。SystemOS 和 Data 未重写，未执行镜像哈希或设备镜像读回。安装记录为 out/ipados18/install-services33/installation.json。

随后从 REC 切到 Fastboot，临时启动 services33：Sending 100,112 KiB（2.309 秒）、Booting（10.176 秒）均 OKAY，总用时 12.568 秒。实际命令和回执已存 boot-services33/temporary-boot.json；没有 flash 启动分区。当前尚无该轮 UI 或 REC 结果，不能以接收镜像成功宣称 RunningBoard 握手或首帧已经恢复。保留 -v、8 CPU、4 个 scanout workers、首帧后控制台交接和两次有限快照；运行至少 90 秒后可在手动返回 REC 时读取服务端 stderr 与线程快照。


### services34：修复无私有消息过滤后端时的原生 XPC check-in

services33 的 REC pstore 仍只有 50 字节 pmsg，没有 XNU console，无法据此排除 panic；本轮从 Data 有界读取约 5.1 MiB APFS 元数据/日志，取得完整的 35/90 秒快照及 RunningBoard stderr，保存在外置 audit-services33-after-boot。RunningBoard pid=6 主线程均在原生 CFRunLoop 等待消息，未见 jetsam 配置读取处阻塞；SpringBoard/backboardd 仍在 RBSConnection handshake block 中 sleep/retry。RunningBoard 的 90 秒工作线程正在 libxpc _xpc_connection_mach_event +0x6d4 内打印 syslog。stderr 只有 adapter 入口，不能把缺少 stderr 当作没有连接错误。

按本机原厂 22H374 cache 的实际地址回溯，该 syslog 分支对应“Bogus check-in attempt. Ignoring.”；进入条件包括 __xpc_connection_validate_check_in_is_for_service 失败。该函数调用原厂 libsystem_kernel 的 mach_port_is_connection_for_service，只接受 KERN_SUCCESS(0) 或 KERN_INVALID_CAPABILITY(20)，其它结果均拒绝连接。当前预链接未装载私有 Sandbox 消息过滤 provider；Darwin24 原生 mach_port.c 在缺少 VERSION_1 callback 时直接返回 KERN_NOT_SUPPORTED，因此与 libxpc 的兼容路径不匹配。同一内核的 ipc_service_port_label_alloc 与 derive_sblabel 已允许无 provider 的无标签端口创建，故问题集中在最终 check-in 的能力查询，而非缺少 RunningBoard 进程。

services34 只在 PONG 的 DEVELOPMENT/DEBUG、且实际未注册 VERSION_1 provider 时适配该查询：先按原生 ipc_port_translate_receive 验证服务端口和连接端口的接收权，保留无效 task/name/right 错误与锁的释放；二者有效后将 filter_policy_id 置 0，返回 KERN_INVALID_CAPABILITY，明确报告缺少可选过滤关联，不伪造标签匹配或过滤策略成功。已有 provider、其它板级及 RELEASE 路径不变。这样由原厂 libxpc 自己选择其已存在的兼容路径；未替换握手响应、禁用 RunningBoard 或跳过 UI assertion。该共用路径也可能影响 MCM 与 logd_helper，但需新运行结果确认，不能据此先宣称其它 XPC 错误均已解决。

源码变更已导出到 Darwin24 patch，并开始编译。本轮没有用户态/服务定义变更，沿用 services33 已安装的原生 jobs、jetsam 资源和 runtime，不需要重传 OS/SystemOS/Data。继续保留首帧后清除屏幕控制台的已有实现，当前不宣称已经出帧。


services34 内核编译与链接成功，最终日志 build-20261025T033810Z.log，匹配 22H374 组件预链接完成；镜像为 out/ipados18/boot-services34/pong-xnu-initial.img，102,514,688 字节，实际保存于外置磁盘。未执行测试、镜像哈希或设备镜像读回；本轮没有 OS/SystemOS/Data 写入，配套用户态仍为 services33 安装回执。

已从 REC 进入 Fastboot，并实际临时启动 services34：Sending 100,112 KiB（2.321 秒）、Booting（3.672 秒）均 OKAY，总用时 6.088 秒；未 flash。命令/回执保存在 boot-services34/temporary-boot.json。当前尚无本轮 UI 或新 REC 结果，仍需确认 RunningBoard check-in 是否完成、MCM/logd_helper 是否恢复，以及 framebuffer 是否提交首帧。原有两次 35/90 秒快照继续启用，可在手动回 REC 后读取。

### services35：22H374 Settings 图标、关于本机与原生 Metal 接入

用户随后确认 services34 已进入系统界面。手动回 REC 后，本轮保存了约 2 MiB XNU console 和 pmsg，并有界读取约 6.75 MiB Data APFS 元数据、快照和服务日志，存于外置 `audit-services34-after-boot`。console 记录 framebuffer 首个实际可见帧及释放控制台，Data 中已有 MCM 原生 UUID 系统组/系统容器。Settings 两次以 SIGABRT 退出，原因均为 LinkServices 的 `NSInternalInconsistencyException: Invalid parameter not satisfying: actionData`；不是旧 20G81 的同一处图标缩放异常。不能据此声称所有 XPC、日志和图形问题都已解决。

新增 Preferences 专用 `settings_compat18.c`，通过匹配 22H374 的实际类、方法编码、image UUID 和入口字节安装修复：

- MobileIcons 的实际缩放函数改为 CoreGraphics CPU 缩放，读取原 CGImage、保留 alpha/色彩；限制有限比例和输出尺寸，不使用 20G81 地址，也不返回空白占位图伪装完成。用户报告的设置图标显示问题随本轮部署，效果待运行确认。
- 关于本机沿用 stock 的无无线硬件分支，省略未移植的 SIM/Wi-Fi/Bluetooth 查询；系统版本取原生本地 fallback，不改全局设备身份。
- 缺少 actionData 的可选 AppIntents 历史记录按初始化失败返回 nil，保留有实际 archive 时的原生处理；没有编造 action、序列化数据或吞掉任意 Foundation 异常。Preferences 的真实未捕获异常另有界保存在 Data 的 `Pong-Preferences18.log`。

将既有 Pong A730 实现按 22H374 分档接入 Darwin24：使用实际原厂 IOGPUFamily 的 IOAcceleratorES（136 字节、168 个虚表项）及匹配的固定预链接地址；相关原厂依赖仅作 library-only 预链接，避免启动 Apple GPU/IOSurface 硬件 provider。保留 SM8475 固件、MMIO、2 MiB GMEM、SMMU SID、4 KiB GPU 页表和真实 GMU/HFI/CP/fence 前置条件，不搬入 Sheng 的 SM8550 常量。

Metal bridge 与 discovery bundle 重新编译为 arm64 iOS18 部署目标。原厂 `_MTLDevice` 实际为 600 字节，取代 20G81 的 528 字节布局；使用 `PongMetal22H374Enabled` 属性。metal2vulkan 新增独立 IOS18_7_10 策略，读取实际 22H374 `MTLPixelFormatGetInfo` 七字/56 字节 ABI，并保留原生 Vulkan 功能限制；未套用 Apple heap/compression 能力。桥接补丁为 `patches/metal2vulkan/0010-pong-ipados18-22H374-policy.patch`。复用相同 Pong native wire ABI 的 Turnip 库，不引入其它板级 GPU 驱动。

backboardd 保留 22H374 BGRA 输出适配，恢复原厂 renderer 选择：实际取得设备时使用 Metal，设备不可用时走原厂 CPU fallback。真实首帧后清除启动控制台的逻辑保留。GPU 仅在硬件和 ABI 前置条件满足后发布，不伪造设备、提交完成或 fence；编译完成本身不代表硬件加速已运行。

内核编译/链接成功（`build-20261025T040044Z.log`），Settings UI 库、Rust bridge 和 Metal plugin 均编译完成。services35 镜像保存于外置 `boot-services35/pong-xnu-initial.img`，项目 out 路径为符号链接。OS 增量基于 services33 安装回执，共 11 个 band、92,274,688 字节，写入已收到完成回执；SystemOS/Data 未重写，services34 已知可进界面的启动镜像保留。没有运行测试、镜像哈希或设备镜像读回。

已从 REC 切到 Fastboot 并临时启动 services35：Sending 100,112 KiB（2.324 秒）、Booting（10.126 秒）均 OKAY，总用时 12.516 秒；未 flash。实际回执保存在 `out/ipados18/boot-services35/temporary-boot.json`。尚无本轮 UI、Settings 图标/关于本机或真实 Metal 提交结果，不能以临时启动命令成功宣称功能已修复。

### services36：GPU BO 池缓存维护的抢占超时

services35 的本轮 REC 保存了 16,168 字节完整 XNU console 和 517 字节 pmsg，原始文件位于外置 `audit-services35-after-boot/rec-pstore`。panic 为 `preemption disable timeout exceeded: 437921 >= 120000 mt ticks`，CPU 1、kernel_task、tid 193；按实际 19.2 MHz timer 换算，连续禁止抢占约 22.8 ms，阈值为 6.25 ms。CPU 2–7 的采样 PC 在 machine_idle，CPU 0 在中断控制路径，未从单个 PC 推断其它核心的完整状态。此轮已完成父电源投票、CX 时钟和 GMU 4 KiB SMMU 配置，尚未到 GMU firmware/HFI、Metal provider 发布、用户态或 Settings，因此不能将本次 panic 归因于 Settings hook、图标或用户态 Metal 对象。

用该镜像的实际内核符号还原了调用栈：`NativeAdrenoMaintainOwnedPages +0xd8` → `NativeAdrenoMemory::writeCombinePool +0x58` → `PongAdrenoCpPrepare +0xa48` → GPU worker。具体错误是将 256 MiB 连续 BO 池的整段缓存 clean/invalidate 放在一次 disable_preemption 内，Darwin24 在最后恢复抢占时检测到超时。没有通过减小 Data、取消多核、跳过缓存同步、关闭 GPU 或调大看门狗阈值处理。

将 owned-page 缓存维护改为最多 64 KiB 一批：每批固定当前 CPU、读取其 CTR、发出实际 cache 指令、执行完成 DSB 后恢复抢占，再开始下一批。这样异构 CPU 调度后会重新读取实际 cache line，前一核心的操作已完成，整个维护函数仍同步完成全部页面后才返回。输入对齐、范围溢出及操作类型检查保留。另检查本驱动所有显式禁抢占点：CPU 状态读取范围很短，物理页缓存维护本来已分成最多 64 页的有界批次；没有其它覆盖整个 BO 池的显式禁抢占循环。

本轮仅修改 GPU 内核源码，保留 services35 已安装的 Settings 修复库、匹配 22H374 的 Metal bundle 和现有 Data；无需再传输 OS/SystemOS/Data。新内核构建和临时启动结果在后续记录，实际越过本故障及 Metal 硬件执行仍需新运行日志确认。

services36 内核已编译/链接完成，日志为 `build-20261025T042356Z.log`；匹配的 22H374 预链接镜像已生成于 `out/ipados18/boot-services36/pong-xnu-initial.img`（实际位于外置磁盘）。随后从 REC 切到 Fastboot，临时启动 Sending 100,112 KiB（2.323 秒）和 Booting（10.174 秒）均 OKAY，总用时 12.588 秒，实际命令/回执保存在同目录 `temporary-boot.json`。没有 flash、OS/Data 重写、镜像哈希、设备镜像读回或测试/性能测量。尚无本轮屏幕或 REC 结果，不能据接收镜像成功宣称 panic 已在运行中消失或 Metal 已加速。

### services37：Darwin24 命令缓冲区与设置/壁纸渲染路径

services36 返回 REC 后，保存了 2,097,140 字节 console 和 11,557 字节 pmsg，并有界读取约 8.6 MiB Data APFS 日志与缓存目录元数据。原始文件在外置 `audit-services36-after-boot`。pmsg 明确显示 BO 池、GPU/GMU 4 KiB context、GMU/HFI、PAS13 ZAP、CP_ME_INIT、非安全切换及真实 timestamp fence 均完成；随后原生 IOAcceleratorES、Vulkan device 和 scanout worker 已创建。这证明已越过 services35 的内核抢占超时，仍不等于已完成界面合成。

console 记录 13 次 `NBCommandBuffer setResponsibleTaskIDs:count:` 未识别异常，backboardd 反复 SIGABRT。最后的设备观测为 c0/0、p0，Data 的 pipeline journal 也只有功能参考行，没有实际 pipeline 创建记录。将调用栈结合本轮 cache slide 0x1efb0000 还原到 22H374 `CA::OGL::MetalContext::start_command_buffer`，确认在 enqueue 前调用此方法。原厂 `_MTLCommandBuffer` 与 `MTLIOAccelCommandBuffer` 均声明 `v28@0:8r^I16I24`，即 const uint32_t 数组和 uint32_t count。

services37 按实际签名保存 command buffer 的任务归属元数据，检查提交状态、空指针/count 和容量；不修改内核 task/BO 所有权，不伪造 GPU 完成。检查同一原厂函数其余调用，protectionOptions、setProtectionOptions、commandBuffer、setLabel、enqueue、addCompletedHandler 在当前前端已有实现。

另修复 native user-client 配额：原实现用历史 opens 总数拒绝后续连接，导致崩溃后或多次正常重建设备时出现 Busy，已关闭的 ABI 查询连接也消耗永久额度。现按 opens-closes 的实际存活数判断；全局固定八个 VM slot 和失败 teardown 保留租约的规则仍有效，不重用仍可能被 DMA 使用的资源。

读取原厂 22H374 Info.plist 后，将 Preferences、LegacyPoster、MercuryPosterExtension、Unity2025Poster、PridePosterExtension 加入本 guest 的插件客户端文件和相同的内核 user-client 授权名单。没有使用任意进程通配符，也没有以 Sheng 的进程、A740 固件或地址替换 Pong 实现。

Settings 的本轮独立日志只有 AppIntents 初始化修补，没有 MobileIcons CPU scaler 或 About radio 代码安装成功记录。重新核对本机 22H374 MobileIcons/GeneralSettingsUI 的 UUID、实际 header/入口和原始指令，均与目标档案相符。Settings 代码改用已在 backboardd 生效的 COW 保护路径，移除额外 vm_region_64 前置调用；UUID、可执行 segment、原指令匹配和 RX 恢复继续保留。Preferences 主队列加载真实 MobileIcons，再对这两个已加载 image 做一次有界、幂等安装重试，避免将 loader 初始化期间未安装的补丁当作成功。COW 失败保留实际错误，尚不能宣称图标已显示。

`default.metallib` 的 InvalidFile 记录来自可选 binary archive 探测。当前前端保留对不兼容 archive 的真实拒绝和原生 AIR/library 编译路径，未将 Apple GPU 二进制 archive 伪装为 A730 可执行程序；本轮 backboardd 的明确退出原因仍是上述未识别 selector。下一轮需要看到实际 pipeline/command 完成和壁纸呈现，再判断后续 shader/IOSurface 故障。

新 native kernel、Settings/UI runtime、Metal bridge 与 plugin 均已编译链接完成；最终内核日志 `build-20261025T044756Z.log`。保留 services36 的 64 KiB 缓存维护批次与 CPU fallback，使用已更新的 DarwinBoot24；services37 启动镜像已打包，OS 增量安装和临时启动结果在后续记录。本轮没有运行测试、图像散列或设备镜像读回。


services37 的 OS 增量安装已完成（11 bands / 92,274,688 字节），SystemOS/Data 未重写。临时启动 Sending 100,112 KiB（2.323 秒）、Booting（10.173 秒）均 OKAY，总用时 15.111 秒，命令回执见 boot-services37/temporary-boot.json。

### services38：iPhone 16 Pro 身份、壁纸资源与图标位图

services37 返回 REC 后，本轮读取 1,035,759 字节 pstore 和约 10 MiB Data 的有界 APFS 日志，保存在外置 audit-services37-after-boot。真实 CP fence、Metal 设备、GPU scanout 拷贝及首三个显示帧均完成。最后记录为 c408/0、d408、p68；backboardd journal 有 98 个 R、78 个 S，未见 ok=0。setResponsibleTaskIDs 未识别退出不再出现。仍有 6 项 private device query 缺少答案与可选 Apple binary archive 的 InvalidFile，没有伪造这些属性。用户确认壁纸和图标主要为空白/内容缺失，尚不能据此归因于同一阶段。

Settings 日志确认 MobileIcons scaler 和 About radio 修补已安装。此次 SIGABRT 是 predictionsData 为空；检查实际 22H374 LNTranscriptActionRecord 初始化体，三项明确必需 archive 为 actionData（x23）、resolvedActionData（x24）、predictionsData（x26）。现在缺少任一项时仅使这项可选历史记录初始化失败，完整记录保留原生实现和其它真实异常处理。

ADT 根 model / hw.product 改为 iPhone17,1，product 名称和说明改为 iPhone 16 Pro，artwork idiom 改为 phone，补回无 Home 键 buttons。实际 SM8475 资源、1080×2412 framebuffer、scale 3，以及 22H374 J171 board/jetsam/arm64 策略保留。phone artwork 使用 2436 family、iPhone10,3 资源 fallback，不以 Apple 面板或 GPU 参数替代 Pong 硬件。静态 init 匹配新 hw.product 后、launchd 前，一次性删除两个原生 MobileGestalt cache 文件，由原生 helper 重建。About _modelNameString 使用所选机型，Preferences 的 UIKit/本地文字采用 iOS 品牌，实际 18.7.10/22H374 版本不变。按实际 22H374 方法编码适配 status bar factory，仅在真实 Pong 主屏尺寸匹配且原厂 provider 回退时，选用原厂 1080 phone 布局。

恢复镜像只有 ~ipad 壁纸清单，默认 iPadOS17 LayeredAnimation 仅有 iPad screen-class 变体。构建脚本在本机 OS 副本生成 phone 清单和 Lake/iPadOS17 的 ~iphone collection 包装，引用原有动画/本地化文件，包装 logicalScreenClass 为 any，由原厂渲染器处理实际面板。新增手机清单不宣告不存在的 Stills/iPad 路径。仓库只保存脚本，不保存 Apple artwork。

Preferences 与实际 iconservicesagent 两个进程的位图生成使用原生 CIContext 公开的 kCIContextUseSoftwareRenderer；方法编码均取自 22H374，并保留原方法的返回/所有权。它们不再依赖尚缺部分 private GPU 查询的 CoreImage 档案；backboardd、Poster 的 Metal 合成、真实 fence 和 scanout 保留。用户态已重新编译链接；启动包复用 services37 已运行的 GPU 内核和相同 22H374 原厂 kext，本轮改变为 ADT 与 runtime/resources。未运行测试、镜像散列或设备镜像读回。部署结果在后续记录。


services38 最终用户态编译成功，输出 runtime-services38/init 与 libpong-ui.dylib。准备 OS 副本时新增 phone artwork 资源链接；增量计划有 4 个 8 MiB bands。通过 REC 写入 OS 33,554,432 字节，4 个区段均收到写完成回执。SystemOS 和 Data 没有重写。Fastboot 临时启动 services38 最终镜像：Sending 100,112 KiB（2.328 秒）、Booting（3.830 秒）均 OKAY，总用时 8.645 秒；未 flash。命令、完整回执和安装记录在 boot-services38/temporary-boot.json 与 install-services38/installation.json。没有镜像校验、镜像散列或设备镜像读回。手机显示、机型字符串和壁纸/图标效果待本次运行反馈，构建/临时启动成功不代表 UI 已修复。

### services39：全局 Metal 发现、原生图标渲染和状态栏 factory

services38 返回 REC 后，本轮读取 1,341,274 字节 pstore，并有界读取约 12 MiB Data
日志，保存在外置 audit-services38-after-boot。CP、Vulkan device、scanout 拷贝和
首三个显示帧均有真实完成记录，最后为 c446/0、d446、p57。Preferences 和
iconservicesagent 的强制 CPU CIContext 已确实安装，用户照片仍显示空图标；
不能把“安装了 hook”当作“位图已生成”。Preferences 本轮还有 SIGSEGV 11，尚无
可用的新 crash stack，未把它误判为历史 predictionsData 断言。

按 UniXNU GPU 文档，22H374 的 Metal 客户端文件改为 `*`，插件保留 root-owned
输入、真实 CP/fence 和 guest ABI 前置条件。内核仅在该实际 GPU service 同时
具备 CPReady、Metal22H374Enabled 和显式 pong_metal_global=1 时允许原生设备
的全局 user-client 发现；每个客户端的 BO、VM、任务所有权继续分离。连接池由
8 扩至 16，旧八个窗口不移动，在已有 4 GiB 页表范围内追加八个互不重叠的
128 MiB IOVA 窗口；物理 BO 池仍为 256 MiB，不增加八份物理内存预留，失败
teardown 仍保留可能在 DMA 中的租约。

撤销 services38 对 CIContext 的强制 CPU 选择和 iconservicesagent UI 注入。
MobileIcons 已定位的缩放入口改为真实 MTLCreateSystemDefaultDevice + 原生
CIContext/contextWithMTLDevice，再用 CIImage transform 和 createCGImage 生成
图标位图。没有伪造 bitmap/GPU completion 或未确定的 private Metal 能力值。
全局发现也覆盖原生图标和 Poster 进程；是否实际生成图标/壁纸需本轮运行反馈。

22H374 可直接调用 Phone/Split/DynamicSplit 的 factory，绕过 iOS factory。
现对实际声明同签名方法的五个 factory 分别保留 original IMP，在实际主屏
1080×2412、nativeScale 有效时选择原厂 Split1080 的 Pong 子类；其它屏幕保持
各原方法。不通过隐藏 rdar 标签来伪装正确布局。

native kernel、用户态、Metal bridge/plugin 均编译完成，内核日志
build-20261025T052427Z.log；OS 增量 11 bands / 92,274,688 字节，5 次写完成回执。
未改 SystemOS/Data，未执行测试、镜像散列或镜像读回。临时启动结果另记。

services39 已执行 Fastboot 临时启动：Sending 100,112 KiB（2.325 秒）、Booting
（10.131 秒）均 OKAY，总用时 12.572 秒，未 flash。完整命令和输出在
boot-services39/temporary-boot.json。UI、壁纸/图标及状态栏效果待手机反馈。

### Pong Wi-Fi：QCA6490 输入及 PCIe handoff 适配

按用户要求，Wi-Fi 不使用 HackiOS。实际 REC 端点为 17cb:1103，子系统
17cb:0108，BAR0=0x60200000/2 MiB；DT 标识 QCA6490，采用 PCIe0/MHI/QRTR，
Linux 对应 ath11k。UniXNU Ath12kWifi 的 WCN7850 后端不能仅替换 PCI ID 使用。

已只读提取 modem_a/image/qca6490 下的 200 个文件，共 24,758,861 字节，
保留 amss/amss20、m3、regdb 和 board-data 候选；未猜测实际固件变体或 NV MAC。
新增 prepare-pong-wifi.py，将实际 DT 的 PHY 表、供电资源、GPIO、时钟/复位、
stream 0x1c01、IOVA 0xa0000000/256 MiB 和 MSI map 写入可重现的板级档案。
实际 MSI 使用 GIC ITS；DT 的旧 SPI MSI 节点为 disabled，未沿用 Sheng 路由。

新增 PongWifiPci 的有界 handoff 配置/BAR 接口：供电、时钟、链路未就绪前拒绝
访问 controller；配置子字写使用真实 byte enable，避免 W1C 状态被 dword RMW
清除。Darwin24 独立归档 libPongWifiPlatform.a 已编译。该归档未链接为可用无线
驱动，services39 没有新增 Wi-Fi 探针或伪造接口；还需实际板级上电、ath11k/MHI、
独立 DMA/MSI 和 22H374 网络 ABI。尚无固件启动、扫描、认证或联网成功观测。


## services40（待本轮真机观察）

- services39 日志显示实际 Vulkan device、CP/GMU 和 scanout 完成，仍无设置图标 bitmap 成功记录；状态栏工厂 hook 未命中实际 provider 选择。
- 改为在 22H374 `_UIStatusBar -_prepareVisualProviderIfNeeded` 的实际实例初始化前选择 Pong 的 phone provider；保留原初始化流程。
- Preferences 原生图片查询返回 nil 时，仅对原系统 bundle 重试原 iPad restore 的真实资源 rendition，保留 iPhone 身份与 GPU 渲染。
- 按用户明确要求，由原始空白 APFS 模板重新生成 8 GiB Data；本机文件完成后再仅写入既有 iOS Data window，保留 SHNGROOT 几何。
- 接入实际 QCA6490 ath11k PCI/MHI/QMI 和 Darwin24 网络层；恢复 wifid，保留 Apple 专属 firmware loader 的排除。编译接入不等于已联网。
- 无线使用 Pong DT 的供电/PHY/SID、独立 DMA arena/native4K 页表、TCS4 和 GIC message-SPI；扩展原有 4 个 SPI owner 槽到 64 个。固件仅保留在本地忽略产物。补齐 QCA6490 GPIO204 XO latch、Pong PCIe PHY ldoc10=1.2V / ldob5=0.88V，CX/MX 保留实际最高 AUX corner。
- OS 更新按 sparsebundle 写入记录生成 delta（当前 3 个 band、25,165,824 bytes），不对完整 OS/Data 做镜像哈希或回读校验。


services40 构建记录：`build-20261025T063108Z.log`，Darwin24 原生内核 43,644,896 bytes；
匹配 22H374 预链接后约 74 MiB，Fastboot 镜像保持 102,514,688 bytes。
Data 写入记录位于 `out/ipados18/install-services40-data/installation.json`；
OS 增量记录位于 `out/ipados18/install-services40/installation.json`。
固件/扫描/GUI 修复是否生效仍以本轮手机观察为准。

services40 的 8 GiB Data 重建写入已完成，16 个设备写入收据；未做镜像哈希或回读。

services40 已安装并临时启动：Data 8,589,934,592 bytes（16 个设备写入收据），
OS delta 25,165,824 bytes（3 个设备写入收据）。Fastboot Sending=OKAY 2.331s，
Booting=OKAY 10.185s；镜像为 boot-services40/pong-xnu-initial.img，未写入 boot 分区。
本轮启用 `pong_wifi=1`，实际固件/扫描/联网和 GUI 结果待手机反馈，未宣称联网成功。


### services41：services40 日志修复与 Pong ADSP 初始接入

services40 REC 保存于外部 `audit-services40`：无 XNU panic；QCA6490 在 PCIe0 link training 超时，wifid 因 `_LimitLoadToDeviceTree` 被 launchd 拒绝（138），Setup[208] 被 SpringBoard 拉起。未获取到 Preferences 自身的诊断，不能把本轮日志当作图标问题已解决的证据。

本版补齐 reset 后 x1 DesignWare lane、AUX 20 MHz、vendor EQ/core preset，使用 ELBI XMLH 与 capability DLL active 联合判断。wifid 移除 Apple WLAN DT gate 并纳入启动服务缓存，保留原 daemon/XPC 接口；未虚构接口或扫描结果。

加入遗漏的 `pong_skip_setup=1`。状态栏 factory 接受 Darwin24 的 nil 默认屏幕和真实屏幕 wrapper，不再要求对象指针等于 mainScreen；撤销预设 class ivar 的捷径，保留原 `_prepareVisualProviderIfNeeded` 对 scale/reference/overflow flags 的初始化。选择仍限定于实际 Pong 1080×2412 屏幕。

新增 `drivers/PongPower` 与 force-load recipe：Pong PAS1 固件启动、实际 SMEM/IPCC/SMP2P/GLINK、电池属性解码/IOPMPowerSource、音频 APM 通道。固件来源为手机自己的 modem_a，未进入仓库。与 Wi-Fi 共享 TCS4 的请求使用同一个睡眠锁，不触碰 GPU 的 TCS3，也不把 SM8550 PAS36/CS35L43 当作 Pong 设备。具体边界见驱动 README；PCM/TFA 路由及 CPU/系统休眠策略仍需后续实现。

未运行测试、镜像散列或设备镜像读回。Data 保留 services40 的新建 8 GiB 卷，仅更新已跟踪 OS bands。


## services42：services41 实际日志后的联合修补

- services41：ADSP APM ready、电池服务发布；PCIe0 `ELBI=1 link=10110008 PHY=86` 后超时，尚未到 WLAN 固件加载。物理音量键随后已被 HID 打开，key1/2 down/up `sent=1`，因此本轮补接输出音量链路。
- 状态栏：22H374 工厂枚举比较父子 IMP，之前给多个工厂安装同一 IMP 导致候选被过滤。恢复 iOS16 单工厂方式，只替换原选择结果为 nil/Fallback 的 Pong 主屏。
- 壁纸：原厂文件并未被删空；恢复原厂目录和默认 manifest，删除重复 UUID 的实体别名目录，使用本版本 WallpaperKit 的产品/屏幕兼容入口。给 PosterBoard 保留真实 fatal signal 和有界持久堆栈，不吞掉异常。
- Wi-Fi：按 Pong DT 使用 WLAN_EN pull-up/16mA，恢复 100MHz rate-change 时钟，CLKREQ 保持有效并先建立 Gen1 链路。仍需新日志确认链路和 QCA6490 固件阶段，不能把编译成功当作热点扫描成功。
- 音频：从 Nothing 原厂容器生成双 TFA9873 的 speaker 配置；GENI QUP1 SE5 @ a94000、GPIO1/89 复位、tertiary MI2S 48kHz/双声道/32位、SMMU SID1801。只读工厂 MTP，不运行校准或测试音。
- UEFI 为 DSP 保留真实的低地址 64KiB DMA lease，经 chosen/pong-audio-dma 交给 XNU。独立页表和共享 SMMU 租约锁，不重置 GPU/UFS 上下文。
- 新 PongAudio 内核用户客户端 + 原生 AudioServerPlugIn。只提供实际就绪的立体声输出，不伪造输入；有真实 scalar/dB 音量控制。PCM StartIO 等待实际 running，失败时不报告成功。
- 内核、UEFI 和用户态编译完成；新硬件 PCM/WLAN 与界面恢复结果等待本轮设备反馈。未运行镜像哈希、镜像审计、回读或测试。

## services43：回收 services42 的界面回退

本轮先从 REC 保存 services42 的 856,562 字节 console/pmsg，并有界读取
7,864,320 字节 Data 日志，位于外置 `audit-services42`。与 services41 对比：
两版 GPU CP fence、Vulkan device 和 scanout worker 都初始化成功；services41
有提交/完成及画面拷贝，services42 三次状态均为 c0/d0/p0。Data 的 backboardd
pipeline 日志没有新增 render 项（两次均为 169），只追加能力查询。SpringBoard
和 backboardd 没有退出或 kernel panic，不能误判成 GPU 固件未启动。

新音频端点在 TFA0 第一次 I2C 读取失败后停止；但新 HAL 仍被音频服务器枚举，
audiomxd 从上版一次 SIGKILL 变成 16 次 SIGSEGV，且都在启动后一秒内。
现把 HAL 保留在 `/pong/staged-audio18`，默认恢复空 board HAL view；原生
audiomxd/CoreMedia 作业保留。PCM 改为显式可选，默认 pong_audio=0，ADSP 和
真实电池服务继续启用。音频插件内部崩溃 PC 尚未获取，不宣称已定位其 ABI 原因。

收回 services42 对 Poster/WallpaperKit 进程增加的注入和全局资源兼容开关，
恢复已被现有 Data 引用的 phone catalog 路径、原厂动画资源和原生产品过滤。
状态栏在实际 Pong 屏幕上先选原厂 phone provider，再对其它屏幕调用原实现，
恢复 services41 的调用顺序；referenceScreenScale 返回 ADT 的实际 scale 3，
避免从 provider 枚举中重入 UIScreen mainScreen。其是否是本次 UI 阻塞的直接
原因仍需真机观察，不能仅凭代码推断确证。

本轮重建用户态并复用 services42 的已构建 Darwin24/GPU/UFS 内核；打包仍为
8 核、4 GiB XNU window、原生 Metal 和 UFS Data。修正打包脚本的重复 boot arg
处理，使显式参数替换默认值。未清空 Data，未运行测试、散列或镜像回读。

OS 增量安装完成：92,274,688 字节，5 个写入收据。services43 镜像临时启动
Sending（2.330s）/Booting（10.126s）均 OKAY，总计 12.545s；回执在
`out/ipados18/install-services43/temporary-boot.json`。是否恢复界面仍待本轮反馈。

## services44：PCIe 供电/时钟、Pong 屏幕资源和音频引脚

已读取 services43 REC 的 1,045,454 字节 pstore，并有界读取 8,257,536 字节
Data 诊断。该版已恢复真实显示提交：MiniIOMobileFramebuffer 记录面板、显示
和每帧 IOSurface 均为 1080×2412 BGRA、stride 4320，首帧已释放 console。
GPU 计数推进到 c434/d434/p54。audiomxd 不再发生 services42 的 SIGSEGV
循环；一次 52 秒后的自身 SIGKILL 不能当作 PCM 已可用。PosterBoard 仍反复
SIGTRAP，目前没有本次有效堆栈，不能声称已证明是分辨率导致。

PCIe 仍是 ELBI=1、PHY=0x86、DLL inactive。对照本机 DT 和 Nothing 源码：
补齐 `/soc/syscon@162128` bit0 的 GDSC collapse vote、RPMH_CXO_CLK 的
`xo.lvl=3`，按原厂顺序选择 PHY PIPE parent 并在 PHY 表之后启用 PIPE 分支。
保留 Gen1 的真实 XMLH+DLL 成功条件，加入原厂 lane-skew/PARF 训练配置。
没有放宽 link 判断或伪造端点。

音频 DT 明确 `i2c@a94000` pinctrl-0 是 qupv3_se13_i2c_active，GPIO48/49
及 cape qup13 mux1；services42 误用了 GPIO52/53 mux2。现已修正，同时保留
真实 TFA 版本、只读 MTP 和 graph/周期完成要求。HAL 工厂在真实设备未就绪时
直接不创建实例，避免未完成硬件初始化就进入 iOS HAL；本轮显式启用 PCM 与
HAL 观察修正后的实际链路，不提前宣称已出声。

屏幕实际像素无需改成 Apple 面板；ADT display/artwork scale 均为 3。
壁纸旧包装仅改外层 ~iphone，内部 variant 仍是 810w-1080h@2x~ipad。
现为两套原厂动画生成 360w-804h@3x~iphone variant，保留原资源及旧 URL 的
兼容链接。2436 仍是软件 artwork family，不作为物理面板高度。
状态栏给 iOS/Phone/Split/DynamicSplit 各自独立 IMP，保留 Darwin24 父子
IMP 比较；此前只替换 iOS 工厂没有命中真实选择，给所有工厂相同 IMP 又会
使候选被原生枚举过滤。没有隐藏 rdar 标签来冒充正确布局。

PosterBoard 仅接入保持原 fatal signal 的持久堆栈，不对其运行全局容器或
WallpaperKit 方法替换。其它启动路径、GPU 和 Data 保留。内核与 runtime
已编译，内核日志 `build-20261025T082807Z.log`；镜像安装/临时启动另记。
未执行测试、镜像散列或镜像回读，新增板级代码没有使用 HackiOS。

services44 安装完成：OS 增量 100,663,296 字节，5 个写入收据，Data 保留。
临时启动 Sending（2.356s）/Booting（10.174s）均 OKAY，总计 12.604s，未
flash boot 分区。回执：`out/ipados18/install-services44/temporary-boot.json`。
本轮界面、link、PCM 播放和 PosterBoard 崩溃状态仍待手机反馈。

## services45：修复 services44 的界面回退与功放复位

已保存 services44 的 783,314 字节 pstore，解码 console 780,552 字节；有界读取
9,306,112 字节 Data 诊断，保存在外置 `audit-services44`。未见内核 panic，
SpringBoard/backboardd 仍存活，但 Metal 三次统计均 c0/d0/p0，10 秒 main queue
wake 未完成。audiomxd 连续 15 次 SIGSEGV，重现 services42 的回退模式；
services43 隔离板级 HAL 后实际有 c434/d434/p54 和首帧显示。缺少本轮有效
音频崩溃 PC，不能将相关性当作已经定位的 HAL ABI 崩溃原因。

本轮将插件从可枚举 HAL 目录移回 staged-audio18，保留原厂 audiomxd 与
CoreMedia 服务。收回 services44 的 Phone/Split/DynamicSplit 工厂覆盖，
默认恢复 services43 的单一 iOS 工厂适配；更广覆盖仅允许明确开发参数
`pong_statusbar_factories=1`。当前没有隐藏 rdar 字样来冒充已修复。

发现实际板级错误：Pong 两个 TFA9873 节点都没有 reset-polarity，Nothing
源码 tfa98xx_parse_dt 默认 HIGH，ext_reset 要求 HIGH 10ms -> LOW 10ms。
此前序列反向、最后保持 HIGH，使功放停在复位中。现按实际驱动修正，继续
显式启用 PCM，HAL 维持隔离。读寄存器失败保留 GENI 原因和 IRQ，仍以真实
revision/graph/DMA 完成为就绪条件，不伪造音频设备。用户客户端清除未实现
的 capture 占位映射，拒绝输入 selector；QueryInterface 按 C ABI 清空失败
返回的 out 指针。

源码/链接审计：PongAudio / PongAudioUserClient / SHENG_AUDIO_CLASS 命名一致，
PongPower、PongWifi archive 均被 Darwin24 构建 force-load，并非漏编。实际
22H374 CoreAudio CreatePlugIn 存在标准 factory UUID 和 NULL 返回分支，
未据此宣称整个 HAL 运行 ABI 已适配。Wi-Fi 的供电请求已被确认，但链路
仍在 DetectQuiet：ELBI=1、PHY=86、DLL inactive。音频停点在第一次 TFA
读取；GPU 初始化已到 Vulkan/scanout worker，但 UI 未送帧。

屏幕物理参数仍是实际 1080×2412、stride 4320、scale 3；2436 是资源
family，不是将 Pong 面板改成 Apple 面板。保留 services44 的板级 PCIe
修正和现有 UFS Data，不清空 Data、不导入 HackiOS、不执行测试或镜像校验。
构建及临时启动回执在完成后追加。

services45 已构建并临时启动。OS 增量 50,331,648 字节，4 个写入收据；
Data 保留。Fastboot Sending / Booting 均 OKAY，精确镜像路径与完整输出记录
于 `out/ipados18/install-services45/temporary-boot.json`，源文件快照保留在
外置 boot-services45/source-runtime。没有 flash boot，未执行镜像散列或
回读。此回执不代表已观测到界面恢复或声音/Wi-Fi/壁纸可用，等待手机反馈。

## services46：按官方接口补齐 Wi-Fi、PosterBoard 与音频传输

用户确认 services45 已恢复 iOS 界面。已从 REC 保存 console 1,983,208
字节、pmsg 3,090 字节，并有界读取 9,043,968 字节 Data 诊断，保存于
外置 audit-services45。GPU 计数推进到 c984/d983，主队列 wake 完成；
实际屏幕 native 1080×2412、logical 360×804、scale/nativeScale=3。
两个 TFA9873 均读出 0xb73、SID1801 映射完成，但 graph 未完成；Wi-Fi
仍 ELBI=1、DLL inactive、DetectQuiet，不能声明扫描已通。

阅读 UniXNU 仓库 docs/zh/porting/network.md、power.md、Sheng 的
WifiProbe/WifiNet、nw_abi.h，并获取 Sheng main：仍为 0e1f6e5，未冒充
有新的 main 改动。按文档要求把 station 事件回调提前到 start/step 前；
硬件 MAC 和信道表就绪前不发布 ifnet。原 PCIe rate-change 设置依赖
GPLL0/6，但没有像 CCF 一样 enable parent。本轮只追加 GPLL0 vote 与
lock 等待，不改共享 PLL 频率；补齐 slave Q2A 分支 halt 条件。实际
DT 为 qcom,cape-gcc，Nothing 的同一 gcc-waipio.c 含对应 Cape fixup，
此前仅从名称判断两个控制器实现不能共用的推测已收回。

services45 PosterBoard 首次有可用 fatal 记录。按其 libobjc 基址减去
真实 cache slide 0x0b2ec000，fatal PC 为 PosterBoard+0x16a8e8；调用
链为 scene -> PBFModalRootViewController initWithScene:dataStore:
behaviorAssertionProvider:，进入断言前 dataStore 为 nil。没有把这个
崩溃误当成 GPU 固件未加载，也不跳过原生断言。保留 PBF 的数据库、
迁移和 renderer，只为 _baseDataStoreURL: 返回 nil/不可写时补接 mobile
拥有的实际 Data 目录 /private/var/mobile/Library/Pong/PosterBoard；
该进程使用与现有端口一致的单用户 MCM 路径。原生错误继续记录。
参考 iOS16 的实际资源过滤策略，phone collection order 仅列出本机
真正存在的 22H374 collection IDs，保留原资源、ID、variant 和已有选择；
不列出 IPSW 缺少、目前无法从 MobileAsset 下载的壁纸。

对状态栏实际 ABI 做源码/二进制元数据核对：22H374 的
_prepareVisualProviderIfNeeded 直接调用 _UIStatusBarGetVisualProviderClassForScreen，
并未通过旧 iOS factory。只在匹配 UIKit UUID 150B91D9-F37E-359F-AD9D-9526A43B81AA
和原入口指令时适配这个选择点，原生支持的结果全部保留，仅在 Pong 的
1080×2412/scale3 返回 Fallback 时选择原厂 1080 phone provider。保留
原函数与 UIKit 后续 flag/scale 初始化，不用删除诊断标签代替布局修正。

音频合并官方 power.md 固定引用的 Sheng 552b674 的 QcomGlink.c
实际差异：remote intent 表满时，用新到的大 intent 替换最小空闲
intent，保留所有正在使用的 intent。原代码只能保留初始小 intent，
APM readiness 成功后更大的 graph 请求可能永远排队。本轮另外修正
MEM_MAP 等待真实 handle；拒绝/超时记录真实状态，不以 ACK 伪造完成。
音频 HAL 保持隔离以保留已恢复的界面，系统声音仍需继续接通。

新增代码未使用 HackiOS，未清空 Data，未运行测试、镜像散列或回读。

services46 内核第二次构建成功，日志 build-20261025T093022Z.log。首次
编译只在 strip 输出时因宿主磁盘满而失败；清理可重建 FAT/ramdisk 中间
文件后重新构建，保留所有完整启动镜像与证据。用户态加入原生 PosterBoard
shared/legacy 两个独立目录，保留迁移时对旧、新路径的区分。镜像安装
和临时启动另记。

services46 OS 增量 92,274,688 字节已安装，5 个实际写入收据；仅更新 OS，
保留 SystemOS 和现有 Data。精确镜像 boot-services46/pong-xnu-initial.img
已通过 Fastboot 临时启动，Sending 2.333s / Booting 5.029s 均 OKAY。
回执：out/ipados18/install-services46/installation.json、temporary-boot.json。
未 flash boot，未执行镜像散列或回读；界面、热点扫描、壁纸及状态栏
效果待手机反馈，系统音频 HAL 仍处于隔离状态。

## services47：根据 services46 REC 日志修正 PCIe 和 Darwin24 接入遗漏

保存 console 841,451 字节和 pmsg 2,876 字节于外置 audit-services46。
PCIe 仍为 ELBI=1、DLL inactive、PHY=86、DetectQuiet；没有把前版
父 PLL vote 当作链路成功。对照实际 Pong DT、Nothing sm8475/t 的
gcc-waipio.c 与其 dt-binding，补启用 DT 的 GCC_AGGRE_NOC_PCIE_1_AXI_CLK
（ID9，vote62000 bit11），保留 ID8/26/52 和独立 CLKREF_EN。恢复
pci-msm/Sheng 的 active CLKREQ 控制和 PARF PM 位，删除强制输入覆盖；
追加电源稳定等待、训练前清除继承的 Link Disable，真实链路建立后
设置 Pong 的 type1 0x60200000..0x63ffffff 窗口。SID、PHY 表、复位、
GPIO 均核对 Pong；未套用 SM8550 两路 PHY GDSC/寄存器。ELBI/DLL/PCI ID
成功条件保持实际硬件要求，后续 MSI/DMA/MHI/扫描仍需实际结果。

PosterBoard fatal 与上轮相同，原生 datastore 修补未执行。元数据确认
_dataStoreWithError: 参数是 o^@，旧 method_abi 把方向限定符 o 当成类型，
因此安装逻辑提前返回。现在剥离 Objective-C 类型限定符后仍检查实际
参数数目/类型；保持原生数据库、错误和 fatal 信号，使用 UID501 的
Shared/Legacy 路径。REC 仅有界读取目录元数据，确认父目录属501，
两条 fallback 目录尚未创建，支持补丁确实未安装的判断。

状态栏 C 选择入口已安装，但无替换选择日志。补接 UIKit 默认 nil
屏幕，避免重入 mainScreen；若未构造 provider 而已有 Fallback class，
通过 Darwin24 原生 setter 清空该选择，再执行原生 prepare 来初始化
scale/overflow flags。使用本版本 runtime ivar/方法签名，未复制旧偏移，
未强行替换活动 provider，保留两条有界选择记录以确认实际调用。

首次编译发现 GPU 固件仍硬编码到已移除的旧 iOS16 构建目录。已从
上一版保留的 PongAdrenoFirmware.generated.c 恢复同一批实际固件，
迁到 out/ipados18/firmware/adreno730；构建器按 guest 选择分支本地输入，
并在编译前报告缺失文件，不再依赖旧 v92 目录。未下载替代固件。
重建日志 build-20261025T100858Z.log 已成功导出内核；运行时亦已编译。
本轮保留现有 Data；没有导入 HackiOS，没有运行测试/镜像散列/镜像回读。
安装及临时启动结果另记。

services47 已安装 25,165,824 字节 OS 增量，3 个写入收据，保留现有
SystemOS/Data。Fastboot 临时启动 Sending 2.332s、Booting 10.171s 均
OKAY；精确镜像 boot-services47/pong-xnu-initial.img，回执保存于
out/ipados18/install-services47/temporary-boot.json。未 flash boot。
该回执仅证明镜像已提交启动，不代表 PCIe 链路或壁纸/状态栏已实机通过。

## services48：恢复 Darwin24 组件并修复空 Poster 数据库初始化

services47 的 REC console 为 1,134,683 字节，保存在外置 audit-services47。
GPU 有真实成功流水线记录；没有本轮内核 panic 证据。PosterBoard 的
路径适配已安装，但原生 datastore 仍返回无法解释的创建错误。
有界读取实际 Data 目录确认 UID501 的 PRBPosterExtensionDataStore/59
已有 Extensions、GalleryCache，均为空，未发现数据库。Shared/Legacy
fallback 目录亦为空；没有用扩大权限或跳过场景断言代替数据库修复。

根据匹配 22H374 的 PBF SQLite 初始化实现，option1 仅验证/打开，
option8 会走原生 SQLite CREATE 和同一 schema setup。只对 Poster
路径下文件不存在、父目录真实且属于当前 mobile 的情况加 option8，
保留所有现存数据库及真实错误。PosterBoard 另保留原生统一日志输出。

状态栏 C selector 修补的安装日志存在，但没有调用决定记录。
为覆盖注入前已经创建的 fallback 对象，主队列有界遍历 UIKit 窗口，
仅对实际 fallback provider 使用原生 region teardown、运行时发现的
strong ivar 和原生 prepare 重建，重新计算 flags；未删除 rdar 字符串。
修复效果须以手机界面和新日志为准。

恢复本源 22H374 服务定义及各自 RunAtLoad/KeepAlive/LaunchEvents 等
触发条件，不再删除无 MachServices 的事件型服务，也不再使用旧板级
软件黑名单。实际选中 573 项，8 项仅因可执行文件缺失或 DarwinOS
变体排除。接入已经下载的同一 IPSW AppOS 组件 094-32850-038.dmg，
将已知 dangling restore App alias 替换为 OS 内真实 App 目录，恢复
8 个 App cryptex 服务。保留原生设备/功能 gate，不伪造硬件就绪。

Metal 默认关闭成功流水线的逐条文件日志、栈回溯和计时；保留失败。
PONG_METAL_PIPELINE_TRACE=1 可显式启用详细记录。桥接器和插件重新
编译为 22H374、600 字节 MTLDevice ABI，实际渲染能力声明未扩大。

Wi-Fi 仍处于 PCIe DetectQuiet / DLL inactive，尚无固件或扫描成功。
本轮复查 Nothing SM8475 的 GCC、PCIe、CNSS 和 pinctrl 源码以及实际
Pong DT，未发现可据此确认修复的新硬件差异，保留 services47 的真实
失败路径；没有套用 Sheng SM8550 的时钟、双路 PHY 或 radio 固件。
本轮复用 services47 已完成内核，仅打包新 revision 和用户态更新。

所有更改保留现有 Data 和 SystemOS。未导入新的 HackiOS 内容，未运行
测试、镜像散列或回读；OS 增量为 134,217,728 字节。安装/临时启动回执
在完成后追加。

services48 的 OS 增量 134,217,728 字节已提交，4 个写入收据；
精确镜像 boot-services48/pong-xnu-initial.img 已成功临时启动，
Fastboot Sending/Booting 均 OKAY，完整输出见 install-services48/temporary-boot.json。
Data/SystemOS 保留；实际壁纸、状态栏和恢复服务结果待手机反馈，
当前不宣称 Wi-Fi 或这两项 UI 问题已经解决。


## services49：Metal 接口与用户映射回收、Poster 保护等级、手机界面和 PCIe 冷启动

services48 的完整 REC console（703756 字节）和 Data 有界诊断保存在
外置 audit-services48。Settings/cameracaptured 实际抛出 NBLibrary
newFunctionWithName:constantValues:pipelineLibrary:error: 不存在的异常。
已增加 22H374 的 pipelineLibrary/functionCache 入口：编译真实函数和
常量特化，保留 pipeline library/cache 生命周期、实际编译错误和回调。
不把原生 Apple binary archive 格式谎称为桥接器可读的缓存格式。
成功常量特化的格式化开销也移到显式详细日志选项内。

随后 cameracaptured 的 clientClose/freeBo 在 IOBufferMemoryDescriptor
释放时触发 pmap_assert_free。匹配内核符号表确认用户 PTE 仍引用 BO。
此构建的 OSPtr 是原始保留指针；removeMappingForDescriptor 的旧 boolean
循环既没有 unmap，也没有 release 返回的引用。现在显式 unmap/release，
失败保留 backing，关闭开始后禁止新 BO 操作，不抑制 pmap 断言。

22H374 PosterFoundation 的 PFPosterPathURLResourceValues 实际要求
NSURLIsReadableKey=YES、NSURLFileProtectionKey=NSURLFileProtectionNone。
对 UID501、已存在且属于 PRBPosterExtensionDataStore 或 Pong fallback
的根目录和 59/Extensions/GalleryCache 设置真实资源保护等级；不设置
虚假校验结果，不清空数据库，不放宽任意 Data 权限。保留原生迁移、
SQLite schema、有效性检查及错误。此变更待新日志确认实际设置结果。

用户要求灵动岛和手机功能。本轮 ADT 的机型保持 iPhone17,1，素材
subtype 从旧 iPhone X 的 2436 改为 iPhone 16 Pro 的 2622，保留
1080x2412 实际面板与 scale3、全面屏手势。匹配本地 22H374 MG UUID
和入口，针对实际 deviceSupportsDynamicIsland getter 的 bool 键
YlEtTtHlNesRBMal1CqRaA 启用 UI 答案，其他 MG 答案保留原逻辑。
没有伪造 Face ID、相机、蜂窝或 120Hz 面板能力。身份缓存 marker 更新，
由 helper 清理既有两条 MG 缓存，不清空 Data。状态栏选择真实的
DynamicSplitMedium 派生布局；在实际状态栏 view 的 layout 时修复
已经存在的 Fallback，覆盖不属于 UIApplication.windows 的服务器窗口。

PCIe 未确认建链。根据 Nothing SM8475 pci-msm 的 suspend/resume 顺序，
冷启动先关闭 PIPE branch 并 park TCXO，核心/PHY reset 后切回外部
PHY；同时清除 PARF PHY_CTRL 的 SW_PWRDN 和 REFCLK_DRV_DSBL 两位，
解决只清 bit0 可能保留休眠 REFCLK 禁用的问题。保留真实链路、DLL、
端点 ID、DMA/MSI/MHI 前提和失败返回；日志记录继承的实际 PHY_CTRL。
REC 当前 CNSS 为 PCI PROBE DONE 但 WLAN 已关电，未冒险读取未时钟
控制器。未将其 sysfs PCI 设备当作建链成功，也未套用 Sheng SM8550 值。

Darwin24 内核、22H374 Metal bridge/plugin 及运行时已重建；保留 Data、
SystemOS 和恢复的原生服务。未导入 HackiOS，未运行测试/镜像散列/回读。
安装与临时启动的精确收据在执行后追加，不能以 Sending/Booting 成功
代替实际界面、灵动岛或无线扫描确认。

services49 已安装 92,274,688 字节 OS 增量，5 个区间写入回执。
Fastboot 对 boot-services49/pong-xnu-initial.img 的 Sending/Booting
均为 OKAY；精确输出见 install-services49/temporary-boot.json。
保留现有 Data/SystemOS，未 flash boot。实际界面、灵动岛、壁纸和
Wi-Fi 结果仍需本轮手机反馈或新 REC 日志，当前不宣称已通过。

## services50：Metal 资源生命周期、状态栏适配和壁纸容器排查

services49 的 XNU console、Data 日志和容器元数据保存在外置
`audit-services49`。新增异常是 `NBTexture makeAliasable` 未实现；
之前的 NBLibrary selector 异常及 pmap_assert_free 没有在本轮再次出现。
已接通 runtime 的真实 texture/buffer make_aliasable/is_aliasable，
保留 heap 分配回收语义与 view 所属 heap；补齐 hazardTrackingMode、
parentRelativeLevel/Slice 的真实 runtime 查询，不虚构资源或 GPU 成功。

本轮末尾 panic 是 watchdogd 连续 SIGTRAP 后的原生 panic-on-crash。
Pong 没有本机 IOWatchdog provider，stock opt-in Mach service 却会启动
该进程。因此保留原始 job/binary/崩溃策略，通过原生设备树导入条件要求
真实 watchdog/wdt 节点；未压制 kernel panic 或删除 watchdogd。

状态栏适配未执行的代码原因已定位：22H374 DynamicSplitMedium 基类
不声明 optional referenceScreenScale，旧前置条件使整段安装直接退出。
改为给 Pong 的真实派生类添加该查询，保持原生父类行为和实际面板尺寸。

用户要求专项检查壁纸相关容器。直接从 REC 有界读取 Data 元数据，
应用容器 204 项、扩展容器 772 项已检查；PosterBoard、CollectionsPoster、
LegacyPoster、照片/天气/Emoji 等壁纸扩展目录均存在，属于501:501，
普通目录为0755。第一批350项扩展扫描未包含 CollectionsPoster，
补查剩余422项后已找到，不能把截断列表当作丢失容器。
原生 PRBPosterExtensionDataStore/59/Extensions/GalleryCache 仍为空，
Pong Legacy/Shared fallback 也为空；暂无普通 POSIX 权限根因证据，
不重建 UUID、不伪造 MCM metadata、不清空 Data。

修正 PosterBoard 的 MCM late-load 时序：原先 PBF 路径钩子装好后，
后续镜像回调提前返回，可能漏掉稍后出现的 MCMUserIdentityCache。
现在每次相关镜像到达仍应用真实单用户策略。文件保护资源访问兼容
22H374 的 BOOL 编码 B/c，保留原生数据库/迁移/保护检查和错误。
仅 PosterBoard 开启35/90秒两次有界进程堆栈，以确认仍未初始化时的
实际等待链，不改变线程或服务返回值。

补回 PreinstalledAssetsV2 共94项、592476字节（SoundScapesPicker、
CoreAS、CarExperience），原始 APFS 元数据提供数值所有者/权限，
init 只以 O_EXCL 创建缺少文件。此批不含壁纸图片，不能据此解释壁纸。
现有原厂壁纸资源、手机 catalog、容器和 Data 保留。

PCIe 在本版仍未建链。此前把 qcom,bw-scale 的 Gen1/Gen2 19.2MHz
误解为初始训练时钟；services51 对照完整 Nothing pci-msm 流程后纠正：
19.2MHz 是链路建立后的缩频策略，初始 refgen 为100MHz。
REC 原生 CNSS 已成功建立链路，但因 REC 缺少 qca6490/amss20.bin
在 MHI 阶段触发 Linux panic；原 XNU 日志提前保存，手机已回 REC，
没有重复执行该缺失固件的流程，也没有把原生 REC 建链等同于 XNU 成功。

Darwin24 内核、22H374 Metal bridge/plugin、init/UI 已重建。
未导入新 HackiOS 内容，未执行测试、镜像散列、镜像校验或设备镜像回读。
部署及临时启动回执完成后追加，保留原有 SystemOS/Data。

services50 已安装159,383,552字节 OS 增量，共9个区间；完成回执见
`out/ipados18/install-services50/installation.json`。实际临时启动镜像为
`boot-services50-fix/pong-xnu-initial.img`（包含120014Z新内核，
不是之前保留的 boot-services50 候选），Sending2.331秒、Booting10.174秒
均 OKAY。精确命令和输出见 `temporary-boot.json`。Data/SystemOS和
23个已找到的壁纸相关容器保留，实际界面/壁纸/状态栏/Wi-Fi仍待反馈。

## services51：壁纸目录保护层级、默认资源与 PCIe 初始时钟

用户确认 services50 灵动岛已生效，壁纸、rdar 字样及 Wi-Fi 未解决。
REC console 为1,166,429字节，连同 PosterBoard 原生 crash、Data
目录元数据保存在外置 `audit-services50`。PosterBoard 在打开壁纸时
因真实 DataStore 为 nil 于 scene 构造阶段 SIGTRAP；这比默认动画加载
更早。23个壁纸相关容器已找到，普通 owner/mode 没有缺失证据。

文件保护设置未执行的实际代码问题是目录层级不符：
`_baseDataStoreURL:` 返回 Application Support，而原 helper 只接受
PRBPosterExtensionDataStore 根。现在先在允许且归当前 UID 所有的
base 下派生真实 store，再为已存在的 store/59/Extensions/GalleryCache
设置 NSURLFileProtectionNone。保留真实 NSURL 错误、原生版本59校验、
SQLite 创建/迁移和 schema；不伪造资源属性或数据库成功。

默认配置仍使用原始22H374的 collection
62879BFA-F700-4384-83E1-7AD1CC0D081E、wallpaper7570。
手机 wrapper 为360w-804h@3x~iphone，两份原始 Background/Floating
动画目录均存在且0755，引用保持实际相对文件名，不改成不存在的
iPhone artwork 名称。新增仅解析 plist/路径元数据的 default_configuration
记录；没有散列、解码整幅图像或运行镜像校验。保留原有 poster 引用。

状态栏原生 `_prepareVisualProviderIfNeeded` 新增22H374 UUID/指令匹配
的入口覆盖，处理可能绕过方法表的调用，也保留方法表入口。
只在真实 Fallback 上执行原生 reset、strong ivar 清理及重新选择；
provider 的 scale/overflow 初始化仍由 UIKit 完成。未用删除 UILabel
或替换 rdar 字符串的方式掩盖布局错误。

PCIe 对照 Nothing SM8475 pci-msm.c：MSM_PCIE_MAX_PIPE_CLK 为2，
Pong max-clock-frequency-hz 的 refgen 在 index11，初始100MHz。
恢复 GPLL0 600MHz父时钟及除6配置；只在真实 DLL/XMLH up 且协商
Gen1/2 后切19.2MHz。Pong DT 未设 target-link-speed，移除强制Gen1，
保留 core reset 的 capability/target。端点、DMA/MSI/MHI/固件前提
和真实失败返回均保留，未声称建链或扫描已成功。

Darwin24 编译完成，内核日志为 build-20261025T123836Z.log。
OS 仅更新41,943,040字节、4个区间，保留现有 Data/SystemOS。
`boot-services51/pong-xnu-initial.img` 临时启动：Sending2.334秒、
Booting1.489秒均OKAY；精确命令/输出在 install-services51/
temporary-boot.json。未 flash boot，未运行测试或镜像散列/回读。
本轮实际壁纸、状态栏及无线结果待手机反馈或新REC日志。

## services52：移除 rdar 诊断布局并连接原生状态栏

用户确认 services51 三项仍未修复，要求删除 rdar 后仍显示状态栏。
已保存本轮946,818字节 console、Data日志及实际目录元数据至外置
`audit-services51`。该版的 prepare COW入口已安装，但仍未记录原生
selector/prepare 执行；不能把“入口已安装”当作布局已经接管。

现在针对实际 `_UIStatusBarVisualProvider_Fallback`：分配并保留一个
独立的 Pong DynamicSplitMedium 派生 provider，把 setup/placements/
style/clock 以及可用的可选消息交给它，类级 scale/overflow/size 查询
同样采用原生手机布局。未把24字节 Fallback 原地换成240字节对象。
Fallback 的弱 statusBar 引用可定位不在 UIApplication.windows 中的
服务器状态栏，在当前更新完成后对真实 bar 排队执行一次原生重建。
正常时间、电池和状态数据仍由 UIKit 的原生 item/provider 提供。
专用 `_UIStatusBarFallbackItem` 的 canEnableDisplayItem 返回NO，
不再启用 rdar 诊断条目；未隐藏整个状态栏或用固定数值伪造图标。
原生 provider 的 regions 替代红色 diagnostic region。灵动岛保留。

壁纸继续在 nil DataStore 处失败，尚未进入默认动画渲染。本轮发现
版本59仅是22H374的 minimumDataStoreVersion；当前 dataStoreVersion
实际返回61。保护 helper 改为动态读取 minimum/current，遍历已有
版本目录，不预建61、不改名或清空原生迁移输入。

REC Data mount 日志明确为 unencrypted APFS，所查六个 poster 目录
的 inode protection_class 均为0。之前 NSURL setResourceValue 成功
并未改变该字段，不能当成已得到持久 class-D 保护。现在仅在实际
UID-owned poster 路径、APFS、挂载点/private/var且真实 statfs 未声明
MNT_CPROTECT 时，调整 PBF 预期资源字典，去掉该卷无法持久支持的
文件加密等级要求。保留原始可达性、可读性、普通权限、目录类型及
SQLite schema/migration 检查；不改写全局 NSURL 属性，不伪造数据库
成功，也不改变加密卷策略。实际是否进入此分支仍需本轮运行日志。

Wi-Fi 仍在初始 PCIe LTSSM Detect Quiet；没有端点ID、MHI或扫描成功
证据。services52 保留services51内核的板级时钟修正，不把此轮状态栏
及 Poster 变更描述为 Wi-Fi 已修复。GPU实现保留services50真实资源
生命周期修补；壁纸先解决存储阶段，再以实际渲染请求继续定位。

services52 已安装41,943,040字节 OS 增量，4个区间。实际临时启动
`boot-services52/pong-xnu-initial.img`：Sending2.331秒、Booting10.165秒
均OKAY，命令输出见 install-services52/temporary-boot.json。
保留Data/SystemOS；未flash boot、未运行测试、镜像散列或镜像回读。
状态栏真实可见结果、Poster数据库迁移和壁纸渲染仍待手机反馈/新日志。


## services53：同步上游、Darwin24 状态栏与 GPU 地址空间压力

按用户要求 fetch 并 fast-forward 同步 UniXNU 与 UniXNU-sheng。
两者 origin/main 与本机 HEAD 已一致，分别为16e592a、0e1f6e5；
Adreno740/AdrenoMetal/GopFramebuffer/ArmSmmu4K/AdrenoFirmware/AdrenoPM4
也没有新增提交。本次没有可合并的新上游代码，保留本机修改和未跟踪的
旧兼容目录。同步记录见 upstream-20261025.json。
已阅读官方 GPU porting 文档的上游 Markdown；保留 Pong SM8475/A730
固件、寄存器、真实 SMMU/BO/fence，以及22H374设备协议和格式策略。

services52 REC console 为1,156,662字节，保存至外置 audit-services52。
CP、SMMU、ZAP、fence及实际 GPU scanout copy 已运行，不能再把所有
显示缺失描述为 GPU 未启动。后续出现 no-copy IOSurface 的
OutOfMemory(Device)，也不能直接等同于物理 RAM 或256MiB BO池不足。

实际 Metal allocator 的 pooled block 固定64MiB，而 Pong 全局客户端中
有128MiB地址窗口；buffer与image各保留一个大块就可耗尽该窗口。
现在按实际 Vulkan heap 大小将块限制为heap/16（最多64MiB），并在
预留整块失败时尝试精确大小的 dedicated allocation。IOSurface import
遇到真实设备内存不足后，回收没有任何活跃 placement 的空闲暖块，
再试一次导入。保留用户页面 pin、IOSurface 生命周期及所有真实错误；
不释放仍被 command/fence 持有的资源，不复用忙碌 IOVA，不扩大物理池。
实现保存为0011-pong-scoped-memory-pressure.patch，构建脚本可复现。
这修正了分配策略，不表示本轮实机已证明壁纸正确渲染。

PosterBoard 已越过先前的 DataStore protection/SIGTRAP，但在启动时
被 ActiveHard 6MiB 终止。当时仍有706468个可用页，未发生全局 RAM
耗尽。原厂 J171/J172 的 Angel override 对该服务设置350MiB软限额，
实际却套用了 SystemXPCService 的6MiB全局规则。原因与本机把
LaunchAngels合并到可信LaunchDaemons cache的服务分类有关。
现在仅在 RunningBoard 为真实 com.apple.PosterBoard 查询内存策略时，
使用一个同步的 Angel identity view；PID、原始identity、bundle、
entitlements和注册保持不变。原生 RunningBoard 从其真实 Angel 数据库
构建策略，不伪造 memorystatus 成功，不全局关闭 jetsam，也不手写
350MiB硬限额。其他服务保持原始分类。

状态栏此前只适配 UIKitCore 的 _UIStatusBar，而本次22H374 SpringBoard
直接依赖 SystemStatusUI.framework，其中 STUIStatusBar 有独立 selector、
FallbackItem及368字节DynamicSplit provider。此差异解释了旧钩子安装后
没有实际调用记录。新适配匹配 SystemStatusUI UUID
0e5c36cc8846361c858bba510ff8dc0d 和实际入口指令，为1080x2412、scale3
屏幕选择原生 STUI DynamicSplitMedium 派生类，并由原生 prepare 初始化
scale/overflow和布局。仅重建实际 fallback；移除专用诊断 item，不删除
整条状态栏，不用固定时间/电量图像替代真实数据，不混用 UIKit/STUI对象。
灵动岛和原生状态数据链路保留，真实可见结果仍待本轮观察。

UI/helper、22H374 Metal bridge/plugin 已重新编译。内核及 SM8475 板级
固件无本轮变更，复用services51/52的123836Z内核并重新打包带
pong_revision=services53的临时启动镜像。Data/SystemOS保留。
Wi-Fi仍无新建链证据，本轮未把任何 GPU/UI 改动报告为无线已修复。
未运行测试、benchmark、镜像散列、镜像校验或设备镜像回读。

services53 已安装100,663,296字节 OS 增量，共5个区间。
实际临时启动镜像为 boot-services53/pong-xnu-initial.img，精确命令及
Sending/Booting 结果见 install-services53/temporary-boot.json；boot未flash。
界面状态栏、壁纸和内存压力修补的实际表现待用户反馈或新REC日志。

用户确认 services53：状态栏和壁纸均正常。该结果是本轮可见界面的
实际反馈，不代表Wi-Fi或所有GPU API已验证。后续保留本版UI、Poster
和Metal修补，继续处理原生PCIe/MSI/MHI与Wi-Fi接入。


## services54：桌面滑动冻结、CP 错误判定与上下文切换

services53 的状态栏和壁纸已由用户确认正常；本版保留其 OS/runtime。
随后用户在桌面滑动时冻结。REC console 851570字节显示 RBBM
CP_HW_ERROR 汇总为0x200，CP_HW_FAULT=0；SMMU FSR=0x400是FORMAT，
没有本轮翻译错误证据。之后 Metal 返回DeviceLost，scanout因GPU拷贝
未安全退役而保留上一帧和DMA页面。不能把这段日志解释为物理内存耗尽，
也不能仅凭CP_HW_FAULT=0断言整个CP汇总是误报；旧版未记录CP子状态。

代码中轮询使用固定fatal掩码，把CP汇总及PM4完成直接判为致命；IRQ
解码器却按CP_INTERRUPT_STATUS判断。现统一两条路径的分类，先保留
真实CP错误/保护状态再清RBBM，实际故障仍终止设备并保留所有忙碌BO。
只在真实故障时输出一次原因，便于区分BR/BV保护错误与硬件故障。

共享ring先前仅在client open时请求BR/BV状态重置，实际在backboardd、
SpringBoard、Poster和Widget等Vulkan客户端之间切换时没有重新进入上下文。
现按队列所属客户端，在实际切换时发出已有A7XX上下文序列，等待前一
真实fence后再清同步时间戳、资源表和BR/BV计数；同一客户端的队列不重置。
已对照Nothing SM8475 gen7_rb_context_switch及Linux a6xx_set_pagetable；
保留A730 APRIV/保护寄存器，不因怀疑保护错误而关闭整套保护机制。

缓存交接先前每次都维护全局VM的所有进程BO/IOSurface。现将客户端完整
IOVA租约随job保存，提交前的WC属性修正/缓存发布和完成后的导入缓存获取
只覆盖该租约；仍覆盖全部该客户端映射，而不是仅覆盖IB。内核无范围的
调用保留原行为，实际fence、对象引用、SMMU/TLB和失败隔离均保留。
此修改减少不相关表面的同步开销；尚无新实机帧率或滑动结果可报告。

本版仅修改内核，复用services53已安装的OS、SystemOS、Data与Metal插件。
未运行测试、benchmark、镜像散列、镜像校验或设备镜像回读。编译和
临时启动的结果另记out/ipados18/services54-audit.json。

services54 内核及22H374预链接镜像已构建完成。实际临时启动的唯一镜像为
`/Volumes/Untitled/Pong-iPadOS18/boot-services54/pong-xnu-initial.img`，
Sending2.330秒、Booting10.175秒均OKAY。精确命令见
`out/ipados18/install-services54/temporary-boot.json`。未重写OS/SystemOS/Data，
未flash boot。桌面滑动和性能变化仍待用户反馈；不能将命令成功等同于
已证明冻结消失。

用户确认services54桌面滑动不再冻结，但壁纸界面仍会卡死。继续根据本轮日志定位，不能将桌面恢复等同于所有壁纸渲染已完成。


## services55：壁纸 BV opcode、128 IB 和 Data A Live 优化

用户确认services54桌面滑动恢复，打开壁纸界面仍卡住。REC console为
1003707字节，pmsg为3621字节。新增真实错误子状态记录显示
CP_INTERRUPT_STATUS=0x2000（Nothing gen7的CP_INT_OPCODEERRORBV），
不是SMMU页错误或CP寄存器保护错误。之后scanout保留上一帧和DMA页面，
Metal返回DeviceLost；backboardd后续SIGABRT位于QuartzCore renderer不可用
断言。不能通过隐藏该错误、释放未退役页面或强制CPU盖写来宣称修复。

已阅读用户提供的data-a-live-cpu-gpu说明、CPU/GPU源码、相关补丁。
该项目也遇到16 IB拆批导致快速操作卡住，并改为128 IB提交。Pong的
Turnip同样将一个Vulkan提交任意拆为每16 IB一批，每批插入独立BR/BV
prologue/fence。现增加协商flag2与method17，固定3104字节、最多128 IB；
原method5的416字节/16 IB ABI不变。所有IB仍按本客户端的真实BO映射
验证并持有至实际fence；大scratch列表移入序列化的堆对象，不占内核栈。
Turnip根据实际Query能力选128或旧16。此为针对已确认命令路径缺口的
修补，尚不能证明所有BV opcode错误都由拆批引起，实机结果仍待反馈。

同时移植Data A Live生产源码中的七项翻译层改动：final render pass缓存、
viewport/push字段原地存储、保留draw command与使用列表、资源跟踪map、
不使用的no-copy rebind日志关闭、完全覆盖的双平面shadow不预先整图清零。
保留本分支22H374策略、内存压力修补、IOSurface生命周期和完整shadow
拷入/写回。未启用padding Alias（原项目也记录其可能覆盖行尾）。
未导入测试、自动滑屏或采样探针。补丁来源/许可见patches/metal2vulkan。

Turnip等待改为传递剩余超时（单次上限100ms），内核仍由真实GPU fence
中断立即唤醒，兜底从100us放宽为1ms，减少Mach往返和调度唤醒。
未照搬SM8850的SCMI/Oryon、Adreno840/G8寄存器、固件或温控阈值。
REC确认Pong是qcom-cpufreq-hw的4+3+1三组，最高分别1804.8、2496、
2995.2MHz，与SM8850两组SCMI接口不同。本版CPU优化来自减少逐draw分配、
缓存构建和等待开销，不宣称已启用SM8475动态调频。

屏幕方面，REC DRM实际发布1080x2412x120cmd/90cmd/60cmd；chosen
选择NT37705 Visionox 120Hz，timing@0为1080x2412、120Hz。旧Pong
MiniIOMobileFramebuffer默认60Hz，且未发布IOMFBDisplayRefresh。
现默认120并发布displayMinRefreshInterval（32.32）和固定step0，将
CoreAnimation心跳与模式/vsync节拍接通；不声明尚未实现的APT/VRR，
不复制SM8850 DPU地址。保留UEFI的物理面板时序；实际物理呈现率与
渲染帧率仍需后续运行日志，配置120不代表已经达到120fps。

services53 UI/状态栏/默认壁纸配置及现有Data/SystemOS保留。Wi-Fi本轮
仍为Detect Quiet，未把渲染优化报告为无线已工作。构建/安装/临时启动
结果另记services55-audit.json；未运行测试、benchmark、镜像散列、
镜像校验或设备镜像回读。

services55 内核、Turnip 和 Metal 插件已构建完成，已安装109,051,904字节
OS增量；Data/SystemOS保留。实际临时启动镜像为
`/Volumes/Untitled/Pong-iPadOS18/boot-services55/pong-xnu-initial.img`，
Sending 2.221秒、Booting 10.173秒均OKAY，返回码0，未flash boot。
精确命令和结果见`out/ipados18/install-services55/temporary-boot.json`。
启动参数包含`fb_hz=120`；壁纸操作是否恢复、实际渲染帧率与物理呈现率
尚待运行反馈，不能将临时启动命令成功等同于这些结果已确认。
