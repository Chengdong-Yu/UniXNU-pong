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

## 用 GitHub Actions 编译内核

不想在本地准备 Xcode、Apple 开源依赖和私有固件时，可以让 GitHub 托管的 macOS runner 编好内核。工作流在 `.github/workflows/macos-build.yml`（名称 **Pong macOS build**），跑在 `macos-15-intel` 上，单次约 25–30 分钟。

### 手动触发

1. 打开仓库的 **Actions** 页面，左侧选 **Pong macOS build**。
2. 点 **Run workflow**，在下拉里选要编的分支，确认。
3. 等名字里带 `CI kernel` 的那个 job 跑完。旁边的 `disk probe` job 名字里就写着 not a kernel build，它只量磁盘。
4. 在 run 页面底部 **Artifacts** 下载 `pong-macos-products`。

推送到任意分支也会自动触发（`on: push`）。并发组是 `pong-macos-${{ github.ref }}` 且 `cancel-in-progress: true`，同一个分支上新推送会**取消**还在跑的那一轮：想保住某次结果，就别在它跑完前再推同一个分支。

### 产物

`pong-macos-products`（保留 14 天）：

| 文件 | 内容 |
| --- | --- |
| `pong-xnu-initial.img` | Android boot v4 容器；ramdisk 是 `PONGFAT1` 包，内含 `EFI/UniXNU/kernel.macho`、`kernel.macho.adt`、`DarwinBoot24.efi`、`unixnu.toml` 和 `pong-boot-manifest.json` |
| `mach.development.pong` | 本次编出的 XNU 内核（Mach-O） |
| `DarwinBoot24.efi` | runner 上现编的 UEFI 引导驱动 |
| `ci-manifest.json` | 本次的路径、磁盘余量、引导决策和产物字节数 |
| `boot-decision.json` | 走了 `prelink` 还是 `initial` 分支，以及容器校验结果 |

`kernel-ci-log`（保留 1 天）是内核构建日志，失败时先看这里。

### 固件：Path A 与 Path B

工作流按 secrets 里有没有 `PONG_FIRMWARE_TOKEN` 分两条路：

- **有 token（Path B）**：用 `configs/pong_ipados18_defconfig`，Wi-Fi、背光、Adreno 730 全开。token 需要能读私有固件仓库 `Chengdong-Yu/UniXNU-pong-firmware`，工作流按固定 commit `c1695a31` 检出 `wifi/`、`audio/`、`adreno730/`、`power/` 四个目录。
- **没有 token（Path A）**：退回 `configs/pong_ipados18_ci_defconfig`，Wi-Fi、背光、Adreno 730 全部关闭。这条路能编出内核，但不是出货配置。

所以 fork 之后直接跑就是 Path A，不需要配任何 secret。要用完整配置，在 **Settings → Secrets and variables → Actions** 加一个 fine-grained token，只给固件仓库的 `Contents: Read` 权限。固件本身不要提交进仓库，工作流只读取。

### 它不做的事

- 不刷机、不碰设备，一次运行只产出文件。
- 不生成 userdata 镜像：那一步需要一份 prepared userland（`result.json`、`apply-complete.json` 以及 OS / SystemOS / Data 三个镜像），CI 里没有这些输入，所以 `userdata-decision.json` 记为 `skipped`，不是失败。
- 默认跳过整镜像哈希校验和单核 ADT 审计（prelink 路径传 `--skip-image-verification`），容器结构校验仍然执行。
- 产物没有上过真机。CI 只校验容器结构和打包内容，不做任何启动验证。

## 源码边界与许可证

仓库包含板级源码、补丁、配置和脚本，不包含 IPSW、Apple 私有二进制、固件、认证数据或构建好的 userdata。它们由脚本在本机获取或生成，并存放于 Git 忽略目录。本机路径只写入 `local.toml` 等本地配置。

设备级代码采用 BSD-2-Clause-Patent；导入组件保留原始许可证及 SPDX 标记，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
