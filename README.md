# UniXNU-pong

**简体中文** | [English](README_EN.md)

Nothing Phone (2)（代号 **Pong**，高通 **SM8475**）的 UniXNU 设备支持仓库。这里包含设备配置、XNU 补丁、板级驱动、用户空间适配和构建脚本，目标系统为 **iOS 16.6.1（20G81）**。

当前代码以已达到 iOS 主界面的设备状态为基准。屏幕显示和触摸可用；Adreno 730 的硬件加速与 Metal 接入仍未确认完成。

- 固件：[uefi-pong](https://github.com/Chengdong-Yu/uefi-pong)
- UniXNU 核心：[UniXNU](https://github.com/UniXNU/UniXNU)
- 文档：[UniXNU 文档](https://unixnu.com)

## 构建

将 UniXNU 核心、设备仓库和 UEFI 仓库放在同一目录下。克隆本仓库需要 `--recurse-submodules`，以取出 NativeMetal 子模块：

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone --recurse-submodules https://github.com/Chengdong-Yu/UniXNU-pong
git clone --recurse-submodules https://github.com/Chengdong-Yu/uefi-pong
```

以下命令从 UniXNU 核心仓库目录运行：

```sh
./unixnu defconfig pong_defconfig -D ../UniXNU-pong
./unixnu fetch
python3 ../UniXNU-pong/scripts/fetch-ios16-assets.py
./unixnu build kernel
./unixnu build package
./unixnu build firmware --uefi ../uefi-pong
```

组件获取脚本按 `board.toml` 中的 iOS 16.6.1 / 20G81 配置读取 IPSW 的 BuildManifest，并通过 HTTP Range 只提取内核、设备树、信任缓存、OS 和 SystemOS（Cryptex1）等构建输入。每个成员在写出时对照 IPSW 中央目录中的 ZIP CRC-32 校验。完整 IPSW 不会保存到仓库或作为必需的本地归档；提取的组件保存在 UniXNU 的 `out/ipsw/20G81` 目录。

Pong 的 userdata 根镜像可在获取组件后单独生成：

```sh
python3 ../UniXNU-pong/scripts/build-userdata.py
```

镜像写入本机的 `UniXNU-pong/out/`。构建器在单次拷贝中生成 SHNGROOT 格式要求的窗口和头部摘要字段，不执行额外的镜像校验或回读。Apple restore 文件、生成的 userdata 镜像、系统卷认证数据和 GPU 固件均不纳入仓库；需要的系统组件由脚本从本地获取的 restore 输入中提取，GPU 固件作为本机输入提供。

生成的 JSON 含报告字段 `write_target`（`/dev/block/sda13`）。它只标明 Pong userdata 分区的参考路径，**不能按该路径直接刷写**。

设备身份配置选择 iPad7,1 / J120 / 20G81 restore 组件；板级驱动和启动配置对应 Nothing Phone (2) 的 Pong / SM8475 硬件。

## 设备支持

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| iOS 16.6.1 启动 | ✅ | 已进入 iOS 主界面 |
| 屏幕显示 | ✅ | 系统界面可显示 |
| 触摸 | ✅ | Pong 触摸路径可用 |
| 实体按键 | ✅ | 保留 Pong 电源键和音量键路径 |
| UFS / APFS | ⚠️ | 原生 UFS 和 APFS 启动路径已接通，部分挂载能力仍在完善 |
| 可写 Data | ⚠️ | 提供本地 userdata 根镜像构建器；镜像需在本机生成 |
| Adreno 730 / Metal | ⚠️ | 驱动和 Metal 适配源码已纳入，硬件加速尚未确认 |
| Wi-Fi、蓝牙、相机、音频、传感器 | ❌ | 尚未完成设备适配 |

✅ 可用，⚠️ 部分可用或尚未确认，❌ 尚不可用。

## 许可证

设备级代码采用 BSD-2-Clause-Patent。导入组件保留各自的 SPDX 标记与原始许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
