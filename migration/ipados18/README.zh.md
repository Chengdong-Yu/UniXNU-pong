# Pong → iPadOS 18 迁移准备

本地分支：`ipados18`。工作目录：`/Volumes/PongDev/source/UniXNU-pong-ipados18`；Downloads 下的同名路径是指向该目录的快捷链接。

本分支保存 Pong iOS 16.6.1 / 20G81 适配基线，供后续迁移使用。当前状态为**准备阶段**，尚未生成 Pong 的 iPadOS 18 内核或启动镜像。

## 基线

- 内核：XNU 8796.141.3 / Darwin 22.6.0，原生 4 KiB 页。
- 板级：Nothing Phone (2)，Pong / SM8475 / Adreno 730，保留当前 CPU、屏幕、触摸、按键及 UFS 实现。
- Data：当前可写 UFS 路径，8 GiB APFS Data；迁移时应使用独立 Data，旧系统卷 UUID 和安装布局不能直接作为新固件配置。
- GPU：保存当前客户端、VA 分配、Metal 桥接和本地 Mesa / metal2vulkan 改动。最新一次临时启动命令成功；壁纸与天气修复的画面效果尚待确认。
- 来源及外部依赖提交见 [baseline/sources.json](baseline/sources.json)。外部 GPU 仓库尚未提交的源代码改动保存为同目录下的补丁；它们是基线记录，不属于 XNU 自动应用的补丁序列。

源目录未纳入 Git，因此在本分支中同步其设备驱动、XNU 补丁、配置、构建脚本和当前用户态源码。`userland/build.py` 按本仓库的 `userland/` 布局调整了源文件路径。原有 `main` 与正在使用的 iOS 16 开发目录继续保留。

## 目标固件

目标是**第七代 iPad（2019）**。iPadOS 18.7.10（22H374）同一份 restore 支持 Wi-Fi 型号 `iPad7,11`（`j171ap`）及蜂窝型号 `iPad7,12`（`j172ap`）；芯片 ID 为 `0x8010`，板号分别为 `0x1c` 和 `0x1e`。固件、BuildManifest 和设备组件清单保存在忽略目录 `apple/ipados18-22H374` 对应的本机输入目录。

原始 `kernelcache.release.ipad7c` 已从 BuildManifest 对应的目标 restore identity 提取。内核字符串为 `Darwin Kernel Version 24.6.0 ... xnu-11417.140.69.706.66~1/RELEASE_ARM64_T8010`，因此公开 XNU 源码基准选择 `xnu-11417.140.69`，不是当前下载的 GitHub `main`。该匹配源码位于忽略目录 `refer/ipados18/xnu-11417.140.69`；用户已下载的 xnu、libplatform、libdispatch 和 AvailabilityVersions 源码也保存在该目录供比对。restore identity 组件路径和大小见 [Wi‑Fi 型号清单](baseline/22H374-j171ap-components.json) 与 [蜂窝型号清单](baseline/22H374-j172ap-components.json)。

旧 `board.toml` 的 `iPad7,1 / j120ap` 是 iOS 16 基线的组件来源，不代表第七代 iPad。新目标元数据单独放在 [target.toml](target.toml)。现有 `board.toml`、`feeds.toml` 和构建脚本仍对应 iOS 16，不能据此生成 iPadOS 18 镜像。

## 后续适配顺序

1. 已选定 iPad7,11 / iPad7,12 的 18.7.10（22H374）restore；内核及 DeviceTree、Cryptex、TrustCache、Multitouch 和启动固件等组件路径记录在本机 BuildManifest 清单中。
2. 建立独立的 Darwin 24 构建配置、缓存和输出目录，再逐项移植现有 XNU 补丁。检查 arm64 非 arm64e、4 KiB / Apple 子页 ABI、线程状态、启动参数和驱动链接接口。
3. 适配 APFS 根卷、Cryptex / dyld 缓存、可写 Data、launchd 及启动服务。重新选择卷 UUID、设备身份和用户态挂载路径。
4. 重做用户态修补中绑定 20G81 的偏移、符号、对象大小及服务策略；尤其检查 `ui_compat.c`、Settings、SpringBoard、keybag 和 sandbox 相关逻辑。
5. 适配新的 IOGPU / IOSurface / Metal ABI 与运行时策略，复用 Pong 的 SM8475 / A730 硬件参数；不把 Sheng 的地址、固件或 GPU 型号直接用于 Pong。
6. 分阶段构建、临时启动并读取实际日志。未适配的功能保留真实错误，不伪造硬件或完成状态。

## 输入与输出

完整 IPSW 和 Apple 私有组件只保存在本机忽略目录，绝不提交到 Git。当前只建立本地迁移输入；源码记录本身不证明新系统已兼容或可启动。
