# UniXNU-pong

**简体中文** | [English](README_EN.md)

Nothing Phone (2)（Pong / Qualcomm SM8475）的 UniXNU 格式 2 设备仓库。当前 main 面向第七代 iPad 的 **iPadOS 18.7.10（22H374）**，使用 Darwin 24.6.0 / XNU 11417.140.69；系统输入为 iPad7,11 / J171AP。iOS16 的实现保留在 Git 历史中。

- 核心构建工具：[UniXNU](https://github.com/UniXNU/UniXNU)
- 独立固件：[uefi-pong](https://github.com/Chengdong-Yu/uefi-pong)
- 官方说明：[仓库迁移](https://unixnu.com/zh/porting/migration)、[设备仓库](https://unixnu.com/zh/porting/device-repository)、[驱动适配](https://unixnu.com/zh/porting/drivers)
- 本设备：[格式 2 配置与构建](migration/format2/README.zh.md)、[系统迁移](migration/ipados18/README.zh.md)、[结构化移植记录](porting.toml)

## 设备与进度

| 部分 | Pong 配置及实际进度 |
| --- | --- |
| CPU / 内存 | SM8475，Cortex-X2 + 3×A710 + 4×A510，PSCI/GICv3，19.2 MHz timer，原生 4 KiB 页；物理 12 GiB，受管容量受保留区限制 |
| 屏幕 | 1080×2412，stride 4320，120 Hz 请求；桌面、触摸与壁纸已有实机记录，实际刷新率和亮度继续完善 |
| GPU / Metal | Adreno 730，Pong 固件、Turnip 与 Metal 前端接入；渲染已用于桌面，部分路径仍需完善 |
| 存储 | 原生 UFS/APFS 启动，独立 OS / SystemOS / 8 GiB 可写 UFS Data，Data 挂载至 `/private/var` |
| Wi-Fi | QCA6490 / ath11k；PCIe0 link、BAR 和 DMA 分配已有日志，SMMU 编程顺序已修补；扫描与联网尚未确认 |
| 声音 | ADSP、AudioReach、TFA9873 双功放与输出 HAL 已接通；已修补 CoreMedia 路由服务选择，实际 PCM 发声尚未确认 |
| 按键 / 电源 | PMIC 物理事件已送达 HID / SpringBoard；电源键与音量键当前反馈仍有问题，继续适配系统动作；完整挂起恢复未完成 |

主机编译、服务发布和设备功能分开记录。最新修补见 [SMMU 与音频路由审计](migration/ipados18/services73-smmu-audio.md)

## 构建

需要完整 Xcode（含 iPhoneOS SDK）、Python 3.12、dtc、对应 Apple 开源依赖、匹配的 22H374 restore/ABI 输入和从 Pong 获取的私有固件。源版本由 `feeds.toml`、`unixnu.lock` 与迁移记录固定。

```sh
git clone https://github.com/UniXNU/UniXNU ../UniXNU-upstream
git -C ../UniXNU-upstream checkout 0fc0fc1f9ddd0fbd4787738afef38667df5e0a95
cp local.toml.example local.toml
# 编辑 local.toml，填写本机构建目录、Xcode 和 volume-layout 路径。
python3.12 scripts/migrate-pong-format2.py --unixnu ../UniXNU-upstream
python3.12 scripts/build-pong-format2.py --unixnu ../UniXNU-upstream --out ../pong-build --jobs 4
```

该入口依次解析 ARM64、SM8475、22H374、Pong 四层配置，编译板级 DTS，再调用已适配的 Darwin24 内核构建器。输出为 `../pong-build/pong/ipados-18.7.10-22H374/pong_ipados18/`。原始镜像下载、用户态处理和 userdata 构建入口见 [系统迁移说明](migration/ipados18/README.zh.md)；配置命令不能代替匹配的本地二进制输入。

## 源码边界与许可证

仓库包含板级源码、补丁、配置和脚本，不包含 IPSW、Apple 私有二进制、固件、认证数据或构建好的 userdata。它们由脚本在本机获取或生成，并存放于 Git 忽略目录。本机路径只写入 `local.toml` 等本地配置。

设备级代码采用 BSD-2-Clause-Patent；导入组件保留原始许可证及 SPDX 标记，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
