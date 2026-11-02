# services113：rootful 包管理、ADSP 装载与 GPU 客户端内存

## 实机证据

用户确认 services112 的 Wi-Fi 连接及输入法已正常，继续保留其实现。
REC console 显示 ADSP 在 stage9 以 `-12 PAS carveout CPU mapping` 退出；
随后 AudioServerPlugIn 为 connected=0，没有可用的物理输出。
壁纸相关进程的 `IOServiceOpen(NativeAdrenoGpuPublished, 0x4147)` 返回
`e00002d5`，Vulkan device 创建失败。不能把进程崩溃解释成壁纸资源不存在。

用户照片还显示 Sileo 依赖处理缺少
`apt.procurs.us_dists_1900_main_binary-iphoneos-arm64_Packages`。
对照 Sileo 2.5.1 源码，DependencyResolverAccelerator 将依赖条目写到
`/var/lib/apt/sileolists`（rootless 则加 `/var/jb` 前缀），该目录由 root
命令创建、交给 mobile 写入；只有普通 lists 目录并不足以完成安装。

## 内核修改

- ADSP 保留 Pong PAS1、原始固件、保留区地址及 MDT 边界检查。分段文件和
  BSS 使用至多 256 KiB 的临时物理映射写入，按 4 KiB 对齐；完成屏障后立即
  解除映射，不再要求一次为整个 40 MiB 保留区分配连续内核虚拟地址。
- 当前实际编译的是 `submit/NativeAdrenoPublish.cpp`，此前修改的
  `PongAdrenoClient.cpp` 不在该编译链。实际客户端改为按需分配 128 项的对象块，
  保持 16384 项上限、稳定 BO 地址、原 token 和索引语义；空预检连接不再先分配
  一整块数 MiB 的对象表。关闭仍必须完成真实 fence、PTE 撤销和 DMA 引用退休。
- GPU 打开失败保留原始 IOReturn 和阶段，不再把内存不足、scope 拒绝、slot 耗尽
  都转换成 Busy。16 个隔离窗口、队列及固件保持。

## rootful 环境

新增 `userland/rootful18.py` 和真实 launchd 首次配置脚本，使用官方 Procursus
`iphoneos-arm64/1700/bootstrap-iphoneos-arm.tar.zst` 与 Sileo 2.5.1
`iphoneos-arm` 包。这是 rootful 仓库的真实架构和路径，不是把 rootless 包
换个架构名称。390 个 Mach-O 生成 4 KiB 签名，保留原应用 entitlement。
Apple 的 launchd、launchctl、dyld、系统库及现有 DNS 配置不被 bootstrap 替换。
替换的命令保留本机 OS 内备份。Apple/第三方二进制仍仅留在忽略目录。

OS 初始仍通过原来的只读 UUID 绑定及根卷授权路径启动。首次配置通过真实
`mount -u -w /` 更新 APFS，并实际写入/删除一个根目录文件；失败即报错退出，
不宣称 rootful 已可用。`pong_rootful=1` 下，共享缓存挂载校验允许最初已绑定的
同一 OS 设备变为可写，SystemOS 和 dyld cache 继续要求只读、原设备及原目录。
没有放宽初始根卷身份或给任意卷提供共享缓存例外。

根卷空间不足在本机安装时实际触发 ENOSPC，因此扩大本机普通 APFS OS 至9 GiB，
在 userdata 未占用的24 GiB偏移处安装，保留旧 OS 窗口和原 SystemOS/Data。
`install-pong-relocated-os.py` 检查真实设备几何和不重叠范围，OS 写入成功后才提交
4 KiB SHNGROOT 目录。目录校验码属于协议生成；不计算或比较任何镜像哈希，
不执行镜像回读。此后 OS 差量以新安装回执为基准。

原 `/var/jb` 环境保留备份，只迁移其识别标记，避免新版仍自动选择 rootless。
Data 只补缺失的 rootful 状态目录及真实索引，不替换当前 Data 镜像。
APT lists 为root管理、sileolists为mobile管理；保留正常仓库签名规则，联网刷新
失败时保留真实种子索引。继续通过限于已安装 Sileo 可执行文件的 setuid helper
运行 root 包命令，不注册虚假的 persona，也不全局放宽非root提权规则。

## 结果边界

内核和运行时已完成主机编译。功能结果须看随后实机的 ADSP/GLINK、物理 PCM
输出、壁纸设备创建、根卷可写记录及一次真实包安装；不能由编译或 Fastboot
成功推断。未运行测试、性能基准、镜像校验或镜像回读。自动重启关闭。
Sileo/APT/dpkg bootstrap 不等于自动提供所有 tweak 注入器及插件兼容性。

来源：[Sileo 2.5.1](https://github.com/Sileo/Sileo/releases/tag/2.5.1)、
[Procursus rootful 仓库](https://apt.procurs.us/dists/iphoneos-arm64/1700/)、
[依赖解析实现](https://github.com/Sileo/Sileo/blob/2.5.1/Sileo/Backend/Dependency%20Resolver/DependencyResolverAccelerator.swift)。

## 安装和临时启动回执

9 GiB OS 已写入新窗口，SHNGROOT 在完整写入后提交；旧 OS、SystemOS、Data
均保留。Fastboot Sending 2.330 s、Booting 10.179 s，均为 OKAY。
设备留在本轮系统中，未执行返回 REC 的重启或定时抓取。
Sileo 首次根卷写入、软件包安装、壁纸、电池和音频结果仍未实机确认。
后续 refresh_runtime 会识别真实 rootful 标记并保留匹配任务；若误传旧 rootless
应用暂存目录则明确退出，避免再次悄悄降回旧版启动路径。
