# UniXNU-pong

**简体中文** | [English](README_EN.md)

Nothing Phone (2)（Pong，Qualcomm SM8475）的 UniXNU 设备适配，目标系统为 iOS 16.6.1（20G81）。本仓库包含 Pong 设备树、XNU 补丁、UFS/APFS、显示与触摸驱动、Metal/Adreno 适配，以及构建脚本。

## 当前状态

- XNU 已通过 DarwinBoot 在 Pong 上交接启动，曾进入 iOS 主界面。
- 原生 UFS 启动、APFS 卷读取和可写 Data 窗口已有设备端实现；Data 镜像在本机生成。
- 屏幕、触摸和实体按键已有 Pong 适配。
- Adreno 730 的命令提交、Metal 设备发现和显示扫描输出有真机日志；壁纸选择页仍有预压缩图片兼容问题，最新解码回退修补的画面效果待设备确认。
- 每次启动的具体结果和待处理问题以本地 REC 日志为准；本仓库不包含设备日志、截图或构建产物。

## 目标硬件与系统

| 项目 | 配置 |
| --- | --- |
| 设备 | Nothing Phone (2) |
| 代号 / SoC | Pong / SM8475 |
| GPU | Qualcomm Adreno 730 |
| iOS restore | iPadOS 16.6.1 / 20G81 组件 |
| XNU / Darwin | 8796.141.3 / 22.6.0 |
| 页大小 | 4 KiB |
| Data | UFS 窗口上的 APFS |

## 构建

将 UniXNU 核心、设备树和 UEFI 仓库放在同级目录，并递归克隆 NativeMetal 子模块：

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone --recurse-submodules https://github.com/Chengdong-Yu/UniXNU-pong
git clone --recurse-submodules https://github.com/Chengdong-Yu/uefi-pong
```

在 UniXNU 核心仓库目录执行 defconfig、依赖获取和设备固件构建。iOS 16 restore 输入由 `board.toml` 对应的脚本从 Apple restore 文件读取；构建输出和 Apple 私有组件保留在本机忽略目录，不提交到 Git。

本机 Darwin/macOS 内核构建入口为 `scripts/build-ios16-pong-macos.py`，预链接启动镜像入口为 `scripts/build-pong-prelinked-boot.py`。命令行参数、Xcode 路径和输出目录以脚本 `--help` 及 `board.toml` 为准。可选的 `scripts/build-userdata.py` 将本机已准备的 OS 与 SystemOS 文件打包成 SHNGROOT 窗口；不会额外读取镜像计算校验值或回读输出。

设备树的 DarwinBoot 包路径使用 `drivers/DarwinBoot22`。构建 restore 输入时只提取清单中声明的组件，不下载或提交完整 IPSW。镜像构建跳过额外完整性散列和回读步骤。

## 目录

- `drivers/`：SM8475/Pong 的 UFS、APFS、触摸、按键、CPU、显示、Adreno 与 Metal 适配。
- `patches/xnu/`：面向 iOS 16.6.1 / Darwin 22 的 XNU 补丁。
- `configs/`：Pong 内核配置。
- `scripts/`：组件准备、内核、启动镜像、userdata 构建脚本。
- `docs/`：本地日志读取方案等开发文档。

## 不纳入仓库的输入

Apple IPSW、系统卷和 Cryptex、trust cache、私有固件、userdata 镜像、内核镜像、设备启动日志及截图都只作为本机输入或输出。请通过脚本准备所需组件，不要把这些文件提交到仓库。

## 许可证

设备级代码以仓库许可证和各文件 SPDX 标记为准。导入组件的许可说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
