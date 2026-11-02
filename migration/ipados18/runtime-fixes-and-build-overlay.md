# Pong / 22H374：完整运行时修复汇总

本次提交基于远程 main 的 `aaa2d739522ef98142fb83f6bf2ce7ad70e4a7a8`，包含本机此前所有驱动、内核补丁、用户态适配和本轮 GPU/PAC 准备的源码改动。既有 `scripts/` 文件、`kits/PongPlatform24/build-kernel.py` 和 `.github/workflows/` 保持该远程版本；其最近的 package、IPSW、AppCryptex 和目录迁移修改保留。

## 包含的功能

| 部分 | 提交的实现 | 实机边界 |
| --- | --- | --- |
| Wi-Fi | Pong 活动 modem 槽固件输入、PCIe0、SID1c01/SMMU、MHI/QMI/WMI、ath11k 等待语义、接口命名和 Apple80211 系统桥接 | 用户已确认扫描与连接；固件不使用 Sheng 二进制 |
| 显示与亮度 | Pong 背光表、原生亮度属性、GPU scanout、surface 所有权及熄屏恢复处理 | 亮度已确认；恢复和帧延迟仍有待改进 |
| 按键和触摸 | Pong PMIC/Goodix 输入、IOHID 和系统音量事件；原生键盘和手机布局适配 | 已接入原生系统路径 |
| 音频 | AudioReach/MI2S、真实 TFA9873 配置、IOAudio2 S16→Q31、原生音量与铃声目录、19.2MHz CoreAudio 时钟、格式重建与 StopIO 排空 | 常规发声/音量已有确认，锁屏解锁后静音仍未解决 |
| 电池和电源 | Battmgr 同轮真实数据、容量/充电源更新、RPMh 共享锁及 DSP 生命周期 | 真实注册表快照与 UI 行为分别记录，不把满电不充电标成失败 |
| USB | DWC3、CDC 日志与手动 REC/Fastboot 命令、Apple USB 配置请求、原生 Mux/TCP、lockdownd 适配、配对及 MobileGestalt 板级信息 | Finder 识别和原生配对会话已确认；未伪造 Apple 证书或激活成功 |
| 存储与用户态 | 可写 UFS/APFS Data 扩容、增量系统写入、键盘、电话组件、Sileo/rootful 和实际 package 状态维护 | 私有镜像、已安装包和凭据不在仓库中 |
| GPU/Metal | A730 固件/ABI1、系统 BO 与真实 fence 生命周期、缓存批处理、活跃导入交接、优先级、共享 shader-cache、单采样/resolve、Metal 接口、帧链路诊断 | 真实 GPU scanout 已运行；剩余壁纸 backlog 不能写成全部解决 |
| CPU/arm64e | 八核 PSCI、X2 空闲选择策略、逐核 PAC 特性记录及上游 native-PAC 密钥补丁的 Darwin24 条件路径 | 八核均报告地址和通用 PAC；密钥访问/EL0–EL1 往返未资格确认，当前仍 arm64 |

各轮的原因、源码路径、主机编译与设备结果保存在 `services74` 至 `services167` 的迁移记录及 `porting.toml`。它们是源码和结论，不含原始私有日志/镜像。

## 现有脚本和 workflow 的保留方式

远程打包入口已经迁移，直接覆盖为实验树中的旧版本会丢失这些更新。本提交把配套驱动所需的 15 个构建配方放在 `kits/PongPlatform24/build-support/merged-scripts/`；这些配方以远程基线做三方合并后保留本机运行时对接。新的 kit 入口 `build-with-runtime.py` 在输出目录生成构建用 source overlay：

1. 源码目录仍引用本次检出；不复制 firmware 或 restore。
2. 先复制现有 `scripts/`，保留其 package/IPSW/ADT 入口。
3. 只在输出目录中选用所列配套配方，并写出 `PONG_RECIPE_OVERLAY.json`。
4. 调用原有 `build-kernel.py`，其余参数、源版本检查和产物回填逻辑保留。

`migration/format2/components.toml` 将 kit recipe 指向这个入口。现有 package workflow、CI defconfig 和标准产物名称不变；CI 仍是缺少私有输入时的裁剪构建，不等价于已验证的实机完整镜像。

也可以显式生成 overlay，用于现有的本地构建操作：

```sh
python3 kits/PongPlatform24/build-with-runtime.py \
  --overlay-only --overlay-dir /path/to/build/pong-runtime-source
# 配好源检出的 local.toml 及公开/私有输入后，在上述目录运行相应脚本。
```

当前实机使用八核，原有 hosted 打包的 ADT 流程仍保留其单核 bring-up 配置。本提交没有偷偷改写其 `finalize-pong-adt.py`；八核实机镜像由配套 prelinked-boot 构建入口的 `--cpus 8` 生成。

## 公开输入和架构准备

公开 native 依赖见 `configs/pong-native-sources.toml`。GPU 核心采用 UniXNU 调度分支 `135b0dabedd9eeac1d43b386843a877f4c974972`；AdrenoMetal 为 `b82fa2751d62d3b0a8b9330a58d1506d9cdf0963`，Adreno740 参考源为 `4db2aa75415775667a8d1c03058053102183f7de`。A730 仍使用 Pong 固件、ABI1 和实际单环，不把 A740 双环/抢占或 Darwin26 偏移当成已适配。

`scripts/prepare-pong-metal-parity.py` 和 `patches/metal2vulkan/parity114/` 准备配套转换器；`scripts/prepare-pong-mesa.py` 将公开 Mesa 固定到 `85fda3b36405bcaea8bffc64aa4f2651dc90770d`，应用 `patches/mesa/parity167/pong-darwin22h374.patch` 的完整累积源码。不要将这份完整补丁与历史增量再次叠加。转换器0026还补齐真实运行时中的 stage_trace、iOS QoS、room-keeper 及调度基础实现，避免只带帧链接增量而漏掉其源依赖。两个准备入口均创建新目录，保留已有工作树。随后使用 overlay 中的配套 builder，Turnip 启用真实 shader cache 和 release/O3，22H374 Metal 前端使用 O3。Apple 的 restore、原版系统库、第三方 app bundle、Pong 高通固件及校准输入均从本地合法输入/脚本取得，不提交生成二进制。

## 本轮实际结果

services167 的完整 Darwin24 内核、Turnip、O3 Metal 前端和插件编译完成，仅安装 96MiB 修改系统区域，保留 Data/SystemOS。真实 GPU scanout 完成，前 5120 次提交未捕获 CP/SMMU 错误。30,216 个交接范围变为 1,793 次批量请求；60秒快照中多数完整交接约0.3–0.4ms。它不能代替全场景帧率结果；壁纸进程仍有明显排队，锁屏后可听恢复仍需继续修复。

在保留远程改动的 PR 源码上，Metal 前端、ArmCpu、GPU 内存/VM/用户客户端及完整 helper 用户态编译成功。构建 overlay 和固定版本 Mesa 源码已生成；准备后的全部 25 个变更 Mesa 源文件与实机配套源相同。完整 PR 的 hosted kernel/package 打包流水线尚未运行。没有运行基准、测试套件或镜像哈希校验。

移植记录使用现有 workflow 固定的 schema：状态 `implemented` 表示源码已实现，未提升为 `device-verified`。历史轮次没有完整不可变验证命令/提交元组的观察，以 `source` 引用对应迁移文档，原分类保存在 `reported_kind`，原始结果和未解决项不删改。补齐必填配置与发现后，同版本记录检查器已通过全部 32 个结构化区域；此项仅检查公开记录，不读取镜像或设备。
