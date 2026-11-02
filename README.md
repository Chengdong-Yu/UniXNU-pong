# UniXNU-pong

**简体中文** | [English](README_EN.md)

Nothing Phone (2)（Pong / Qualcomm SM8475）的 UniXNU 格式 2 设备仓库。当前 main 面向第七代 iPad 的 **iPadOS 18.7.10（22H374）**，使用 Darwin 24.6.0 / XNU 11417.140.69；系统输入为 iPad7,11 / J171AP。

- 核心构建工具：[UniXNU](https://github.com/UniXNU/UniXNU)
- 独立固件：[uefi-pong](https://github.com/Chengdong-Yu/uefi-pong)
- 官方说明：[仓库迁移](https://unixnu.com/zh/porting/migration)、[设备仓库](https://unixnu.com/zh/porting/device-repository)、[驱动适配](https://unixnu.com/zh/porting/drivers)
- 本设备：[格式 2 配置与构建](migration/format2/README.zh.md)、[系统迁移](migration/ipados18/README.zh.md)、[结构化移植记录](porting.toml)

## 设备与进度

| 部分 | Pong 配置及实际进度 |
| --- | --- |
| CPU / 内存 | SM8475，Cortex-X2 + 3×A710 + 4×A510，PSCI/GICv3，19.2 MHz timer，原生 4 KiB 页；物理 12 GiB，受管容量受保留区限制 |
| 屏幕 | 1080×2412，stride 4320，120 Hz 请求；桌面、触摸与壁纸已有实机记录，亮度调节已获实机确认，实际刷新率继续完善 |
| GPU / Metal | Adreno 730，Pong 固件、Turnip 与 Metal 前端接入；渲染已用于桌面，已合并上游调度、共享缓存和 Metal 更新；慢帧及部分恢复路径仍在修复 |
| 存储 | 原生 UFS/APFS 启动，独立 OS / SystemOS / 可写 UFS Data（实机已扩至 128 GB；CI 初始卷仍为 8 GiB），Data 挂载至 `/private/var` |
| Wi-Fi | QCA6490 / ath11k；PCIe0 link、BAR 和 DMA 分配已有日志，SMMU 编程顺序已修补；扫描和连接已获实机确认，固件来自 Pong |
| 声音 | ADSP、AudioReach、TFA9873 双功放与输出 HAL 已接通；已修补 CoreMedia 路由服务选择，原生 IOAudio2 发声和音量已获实机确认 |
| 按键 / 电源 | PMIC 物理事件已送达 HID / 可以正常工作 |

主机编译、服务发布和设备功能分开记录。本次汇总包含此前的 Wi-Fi、亮度、输入、音频、电池、USB、系统组件和 GPU 修复，见 [完整运行时修复及构建接入](migration/ipados18/runtime-fixes-and-build-overlay.md)。

## 驱动仓库

`drivers/` 下每个板级驱动都是独立仓库的 submodule（写法与 [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) 相同）。路径名保持不变，构建脚本与驱动之间的相对 `#include` 不必改。

| 路径 | 仓库 | 适配硬件 |
| --- | --- | --- |
| `drivers/ArmCpu` | [pong-arm-cpu](https://github.com/Chengdong-Yu/pong-arm-cpu) | SM8475 GICv3 + ARM generic timer，PSCI 从核启动；上游 [ArmCpu](https://github.com/UniXNU/ArmCpu) 的 Pong 变体 |
| `drivers/ArmDTPlatform` | [pong-arm-dt-platform](https://github.com/Chengdong-Yu/pong-arm-dt-platform) | Apple 格式设备树平台专家（`/chosen`/`/cpus`/`/arm-io`）；上游 [ArmDTPlatform](https://github.com/UniXNU/ArmDTPlatform) 的 Pong 变体 |
| `drivers/NativeRamBlock` | [pong-ram-block](https://github.com/Chengdong-Yu/pong-ram-block) | RAM 后端 Data 卷（sparse seed + 持久 overlay）；上游 [RamBlock](https://github.com/UniXNU/RamBlock) 的 Pong 变体 |
| `drivers/DarwinBoot24` | [pong-darwin-boot24](https://github.com/Chengdong-Yu/pong-darwin-boot24) | Darwin 24 UEFI XNU 加载器 + PONGFAT1；上游 [DarwinBoot](https://github.com/UniXNU/DarwinBoot) 的 Darwin 24 变体 |
| `drivers/AdrenoMetal` | [pong-adreno-metal](https://github.com/Chengdong-Yu/pong-adreno-metal) | Adreno Metal 前端（pin `c6d29ca`）；上游 [AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) |
| `drivers/NativeUfs` | [pong-ufs](https://github.com/Chengdong-Yu/pong-ufs) | SM8475 UFS host（`0x01d84000`）、SID `0xe0`、SPI 265；SHNGROOT v1 三窗口 |
| `drivers/PongAdreno` | [pong-adreno](https://github.com/Chengdong-Yu/pong-adreno) | Adreno 730 GMU/CP/HFI/ZAP/SMMU/scanout（Cape GPUCC） |
| `drivers/PongMetal` | [pong-metal](https://github.com/Chengdong-Yu/pong-metal) | 22H374 的 A730 Metal 前端与 GOP 扫描输出 |
| `drivers/PongPower` | [pong-power](https://github.com/Chengdong-Yu/pong-power) | ADSP PAS1、TFA9873 双功放、WCD938x、NT37705 DCS 背光、电池 |
| `drivers/PongUsb` | [pong-usb](https://github.com/Chengdong-Yu/pong-usb) | SM8475 DWC3/eUSB2 + NXP repeater，32 KiB DMA / SID0 |
| `drivers/PongWifi` | [pong-wifi](https://github.com/Chengdong-Yu/pong-wifi) | QCA6490：PCIe0 + MHI + ath11k，SID `0x1c01` |
| `drivers/PongBootCapture` | [pong-boot-capture](https://github.com/Chengdong-Yu/pong-boot-capture) | PMK8350 SID0 SDAM7100 有界诊断重启 |

跨驱动头文件 `drivers/PongProbe.h` 仍留在本仓库。克隆时请加 `--recurse-submodules`，或之后执行 `git submodule update --init --recursive`。

## 构建

需要完整 Xcode（含 iPhoneOS SDK）、Python 3.12、dtc、对应 Apple 开源依赖、匹配的 22H374 restore/ABI 输入和从 Pong 获取的私有固件。源版本由 `feeds.toml`、`unixnu.lock` 与迁移记录固定。本仓库用 `git clone --recurse-submodules` 拉齐 `drivers/` 下的 submodule。

```sh
git clone https://github.com/UniXNU/UniXNU ../UniXNU-upstream
git -C ../UniXNU-upstream checkout 0fc0fc1f9ddd0fbd4787738afef38667df5e0a95
cp local.toml.example local.toml
# 编辑 local.toml，填写本机构建目录、Xcode 和 volume-layout 路径。
python3.12 scripts/migrate-pong-format2.py --unixnu ../UniXNU-upstream
python3.12 scripts/build-pong-format2.py --unixnu ../UniXNU-upstream --out ../pong-build --jobs 4
```

该入口依次解析 ARM64、SM8475、22H374、Pong 四层配置，编译板级 DTS，再调用已适配的 Darwin24 内核构建器。输出为 `../pong-build/pong/ipados-18.7.10-22H374/pong_ipados18/`。原始镜像下载、用户态处理和 userdata 构建入口见 [系统迁移说明](migration/ipados18/README.zh.md)；配置命令不能代替匹配的本地二进制输入。

## 用 GitHub Actions 打包

不想在本地准备 Xcode、Apple 开源依赖和私有固件时，让 GitHub 托管的 macOS runner 打包即可。工作流是 `.github/workflows/package.yml`（名称 **package**），跑在 `macos-15-intel` 上，按 UniXNU 的打包文档（`docs/porting/packaging.md`）调 `scripts/package.sh`，另加这一步文档没有覆盖的 userdata 镜像。

### 两个 job

| job | 做什么 | 产物 |
| --- | --- | --- |
| **userdata image** | 从 `ipsw_url` 指定的 IPSW 现场做出 prepared userland、SHNGROOT 目录和整张 userdata 镜像 | `pong-userdata`（30 天）、`volume-layout`（1 天） |
| **boot image, payload and kernel** | `scripts/package.sh --os …` 走 deps / preflight / patches / build / pack / verify，再补上本地那两件规范没写的事 | `pong-package`（14 天） |

两个 job 分开是因为它们抢的是磁盘而不是 CPU：userdata 链峰值约 37 GiB，打包 job 的 XNU 对象和 EDK II 输出约 25 GiB，而一台托管 runner 只有 14 GiB 内存和 80–180 GiB 磁盘。打包 job 只从另一个 job 拿一个 2 KB 的 `ci-volume-layout.json`，所以 `userdata` 失败或跳过都不阻止打包。

### 手动触发

1. 打开仓库的 **Actions** 页面，左侧选 **package**。
2. 点 **Run workflow**，选要跑的分支。
3. OS profile 固定为 `ipados-18.7.10-22H374`（下拉只有这一项）。
4. 想连 userdata 镜像一起生成，就把 Apple 的 iPadOS 18.7.10（22H374）restore 下载地址填进 `ipsw_url`；留空跳过，打包 job 用 uuid5 占位 UUID 编内核，那一次的产物不能上机。
5. 想用 22H374 原厂 kernelcache 预链接，把 `prelink` 打勾；默认关闭，直接用本次编出的原生内核。
6. 跑完在 run 页面底部 **Artifacts** 下载 `pong-package`，有 userdata 时再下 `pong-userdata`。

`package` job 需要仓库 secret `UNIXNU_UNIQUE_CHIP_ID`，值是这个设备的 8 位十六进制平台种子；没有它 `payload` 步骤会直接拒绝运行，因为种子是编进镜像的每芯片取值。

并发组是 `package-${{ github.ref }}` 且 `cancel-in-progress: true`，同一个分支上再手动触发会取消还在跑的那一轮。工作流只由 `workflow_dispatch` 触发，推送不会自动跑。

### 打包 job 做的事

`scripts/package.sh` 与 UniXNU 的 `templates/package.sh` 逐字节相同，它按 `board.toml [package] unixnu` 找到 UniXNU 检出，跑 `unixnu release`。产物在 `UniXNU/out/pong/<os>/<config>/` 下：`boot/boot.img`、`payload/payload.img`、`kernel/kernel.macho`、`package.manifest.json`。

runner 上有三处与仓库默认不同，都不是随意的：

- **defconfig**：`release` 总是用 `[package.defconfig]` 的映射，命令行改不了，所以 runner 上把该映射改成 `pong_ipados18_ci_defconfig`。出货配置的 Wi-Fi、背光和 Adreno 阶段需要公开仓库不带的私有固件，CI 编不了。仓库里仍保留出货映射。
- **编译树**：共享 kernel 步骤的树里 `drivers/`、`kits/`、`patches/` 已是真实目录，设备内核构建器另用一棵树，编完把 `mach.development.pong` 回填到共享树的 `[kernel.prelink] kernel` 路径。
- **卷 UUID**：内核把 OS / SystemOS / Data 三个 UUID 编成常量。这次跑出了 userdata 就用那份布局；没跑出来就用固定命名空间的 uuid5 占位值，保证同一份对象树可复用，但产物只能编译验证。

### 与本地产物对齐的两步

`docs/porting/packaging.md` 只写到 `boot/boot.img`，本地那套流程还多做两件事。CI 用 `scripts/finalize-pong-adt.py` 按同样的参数补上，所以 CI 出的镜像与本地出的形态一致：

- **ADT 换成本地生成器**：共享的 `devicetree.generate()` 只写 root、chosen 和 `[adt]` 里配置的节点，`/arm-io` 上内核要读的 `pong,ufs-*`、触摸和按键资源、PMIC 元数据都不在里面。这一步调 `scripts/make-pong-adt-ipados18.py`，参数与本地一致：`--dram-size 0x300000000`（12 GiB）、`--window-size 0x100000000`（4 GiB）、`--cpus 1`、`--chip-id 0x8010`、`--development-debug`、`--skip-image-verification`，并把 `pong-boot-manifest.json` 的 SHA-384 写进 `/chosen/boot-manifest-hash`。生成结果写回 `payload.img` 里的 `EFI/UniXNU/Payloads/kernel.macho.adt`，同目录也放一份 `pong-boot-manifest.json`。
- **启动镜像换成可刷形态**：固件（`PongBootMemoryDxe`）只认带 `PONGFAT1` 信封的 initrd，而 `release` 把 `payload.img` 原样塞进 ramdisk 槽。这一步按本地镜像的容器重打一张 `boot/pong-xnu-initial.img`：Android boot v4 头、kernel 槽为空（这台机器从自己的分区启动固件）、ramdisk 是信封加 `payload.img`、末尾 4096 字节空签名区。`boot/boot.img` 仍是 `release` 写的那张规范产物。

`PONGFAT1` 信封里 `payload.img` 必须是 96 MiB，因为 `PongBootMemoryDxe` 拒绝超过 128 MiB 的 initrd，本地 `--fat-mib` 也是 96。`board.toml [payload] size_mib` 因此是 96。

### userdata 镜像

输入只有一个：Apple 的 iPadOS 18.7.10 / 22H374 restore（IPSW）下载地址，填进 `ipsw_url`。压缩、解密、挂载、组装都在 runner 上完成。

链接必须是 https 且指向 `.ipsw`。填了 `ipsw_sha256` 就在下载完之后核对整个 IPSW 的 sha256，对不上直接失败，一个字节都不解压。

runner 上依次做（`scripts/prepare-pong-userland-from-ipsw.py` 串起来）：

1. 下载 IPSW（`scripts/download-ipados18-restore.py`，分段并发、可续传），核对大小，按需核对 sha256。
2. 用 `scripts/prepare-ipados18-components.py --ipsw` 取组件，核对确实是 18.7.10 / 22H374、机型是 iPad7,11 或 7,12。
3. 用 `scripts/decrypt-ipados18-aea.py` 解 OS 和 SystemOS 两个 `.dmg.aea`（AEA profile 1）。
4. OS 展开成裸盘，挂载作为 `--os-root`，同一张裸盘作为 `--os-image`。
5. 新建 5 GiB 普通 APFS 卷，用 `userland/build-systemos18.py` 复制原版 SystemOS 文件，产出 `pong-ipados18-systemos.cdr`。
6. 新建 8 GiB 空 APFS 卷并转成裸盘，作为 Data 起点。
7. 跑 `userland/build-ipados18.py prepare` / `apply` / `finish`。
8. 用 `scripts/stage-pong-userdata.py` 的校验函数检查五个文件、`result.json` 的 build、镜像对齐和三个卷 UUID。

然后写 `ci-volume-layout.json`，再生成 SHNGROOT 目录（`scripts/prepare-ipados18-ufs-layout.py`，要读约 19 GiB，这一段最慢），最后按目录拼出整张镜像（`scripts/build-pong-userdata-image.py`）。

三个卷 UUID 从镜像的 APFS 卷头读出来，编进内核的 `XNU_OWNER_*_VOLUME_UUID`，也写进 ADT 的 root-match 值。**同一次 run 编出的内核和这份 userland 是配套的**：UUID 是内核常量，换了 userland 就要重编。

这一步要 macOS 且要能免密 `sudo`：`apply` 和 `build-systemos18.py` 要 root 才能把原版文件的 uid/gid 写对。`sudo -n true` 不通过就直接失败，不跳过一个 root 步骤假装成功。

产物按 4 份上传成 `pong-userdata`：镜像文件名 `<name>.part00` … `.part03`（两位编号，单份至多 8 GiB，镜像约 19.4 GiB，保留 30 天）。下载后按顺序拼接：

```sh
cat pong-ipados18-userdata.img.part00 pong-ipados18-userdata.img.part01 \
    pong-ipados18-userdata.img.part02 pong-ipados18-userdata.img.part03 > pong-ipados18-userdata.img
```

镜像只写到最后一个窗口的结尾，不是整个 454 GiB 分区。写入手机仍然用 `scripts/install-ipados18-ufs.py`，一次一个窗口并最后提交 SHNGROOT 目录；CI 不把它推给任何设备。

没填 `ipsw_url` 时 userdata job 只把 `staged=false` 写进输出就退出，不是失败。

### 产物

`pong-package`（保留 14 天）：

| 文件 | 内容 |
| --- | --- |
| `pong-xnu-initial.img` | 与本地同形态的启动镜像：Android boot v4、kernel 槽为空、ramdisk 是 `PONGFAT1` 信封加 `payload.img`、空签名区；`pong-boot.json` 记着参数、ADT 与 manifest 的摘要 |
| `boot.img` / `payload.img` | `release` 写的规范产物：`boot.img` 的 ramdisk 是未包装的 `payload.img` |
| `kernel/kernel.macho` | 本次编出（或预链接）的内核 |
| `package.manifest.json` | `release` 的清单，摘要已按重打后的文件更新 |
| `pong-boot-manifest.json` | 本地镜像里也带的那份 unsigned 开发引导说明 |
| `ci-volume-layout.json` / `local.toml` | 这次用的卷布局与 runner 上写的板级本地输入 |

`pong-userdata`（保留 30 天）是 `.part00` … `.part03` 四份，另附 `userdata-image.json` 与 `ci-volume-layout.json`。`volume-layout`（保留 1 天）只用于两个 job 之间传布局。

### 它不做的事

- 不刷机、不碰设备，一次运行只产出文件。
- 不做任何降级：链路上任何一步失败（下载、组件校验、AEA 解密、`sudo`、`apply`、五项校验、打包、ADT 与容器检查）都会让这一步失败。
- ADT 与启动镜像按本地参数生成，但 `--skip-image-verification` 与本地一致，所以 `image_sha256` 之外的哈希审计不做；容器结构检查照做。
- 产物没有上过真机。CI 只校验容器结构和打包内容，不做启动验证。
- prelink 的 slide 还没有在本板表达，走 `prelink` 分支用的是本地那套固定基址；这条路径未在 runner 上验证过。

## 源码边界与许可证

仓库包含板级源码、补丁、配置和脚本，不包含 IPSW、Apple 私有二进制、固件、认证数据或构建好的 userdata。它们由脚本在本机获取或生成，并存放于 Git 忽略目录。本机路径只写入 `local.toml` 等本地配置。

设备级代码采用 BSD-2-Clause-Patent；导入组件保留原始许可证及 SPDX 标记，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
