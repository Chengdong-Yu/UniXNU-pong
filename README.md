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
2. 点 **Run workflow**，选要编的分支。
3. 想编出货配置就把固件压缩包的下载地址填进 `firmware_url`（见下一节）；留空就只编 CI 配置。
4. 想连 userdata 镜像一起生成，就把 Apple 的 iPadOS 18.7.10（22H374）restore 下载地址填进 `ipsw_url`（见「userdata 镜像」一节）；留空就跳过，什么都不下载。
5. 等名字里带 `CI kernel` 的那个 job 跑完。旁边的 `disk probe` job 名字里就写着 not a kernel build，它只量磁盘。
6. 在 run 页面底部 **Artifacts** 下载 `pong-macos-products`，有 userdata 镜像时再下 `pong-userdata-part0` 到 `part3`。

推送到任意分支也会自动触发（`on: push`），但 push 事件没有输入框，所以只会走不含 Wi-Fi、背光和 GPU 的 CI 配置，也不会做 userdata。要完整配置就手动触发一次。

整轮的上限是 360 分钟：这是 GitHub 托管 runner 对单个 job 的硬上限，填更大不会给更多时间，只会在六小时处失败。userdata 两个阶段各自另有 120 / 150 分钟的上限，超时只让该阶段失败，后面的 `always()` 步骤照常把内核传上去。

并发组是 `pong-macos-${{ github.ref }}` 且 `cancel-in-progress: true`，同一个分支上新推送会**取消**还在跑的那一轮：想保住某次结果，就别在它跑完前再推同一个分支。

### 固件压缩包

Wi-Fi、背光和 Adreno 730 这三块需要一个只在这台手机上的固件，公开仓库里不带。做法是你自己把固件打成一个压缩包放到 CI 能下载的地方（对象存储、私有 release、任何能给出 https 链接的位置），再把链接填进 `firmware_url`。

压缩包可以是 `.zip`、`.tar.gz`、`.tgz` 或 `.tar`，解压后至少要有下面四组文件（多一层外壳目录没关系，比如从文件夹压缩出来的 zip）：

```
wifi/                 # 也可以叫 qca6490/，两者等价
  amss.bin
  amss20.bin
  m3.bin
  regdb.bin
  bdwlan.e01          # 至少一个 bdwlan* 板级数据
  ...
audio/
  tfa98xx.cnt
adreno730/
  gmu_gen70000.bin
  a730_sqe.fw
  a730_zap.mdt
  a730_zap.b01
  a730_zap.b02
power/
  adsp.mbn
```

`wifi/` 建议整目录放进去：构建器会把目录里每个文件都嵌进内核的固件包，`Data.msc` 这类文件也在其中。`adreno730/` 和 `audio/` 只认上面列的名字，多放的文件会被忽略。

填了 `firmware_sha256` 就先核对整个压缩包的 sha256，对不上直接失败，一个字节都不解压。链接必须是 https。

压缩包放在公开可读的位置（或对象存储的带签名临时链接）就不用配任何东西。如果它挂在私有 GitHub release 上，工作流会用 `Authorization: Bearer` 去取，所以要加一个 secret `PONG_FIRMWARE_ARCHIVE_TOKEN`，只给那个仓库的 `Contents: Read`；`firmware_url` 填 API 形式，比如
`https://api.github.com/repos/<owner>/<repo>/releases/assets/<asset id>`，请求头带上 `Accept: application/octet-stream` 由 curl 自动跟随重定向完成。

本地打包可以用仓库里的脚本，它按上面的结构生成 zip 并打印 sha256：

```sh
python3 scripts/pack-pong-firmware.py \
  --wifi ../private-inputs/pong-wifi-firmware \
  --audio ../private-inputs/pong-audio-inputs \
  --adreno730 out/ipados18/firmware/adreno730 \
  --adsp ../private-inputs/pong-power \
  --output ../private-inputs/pong-firmware.zip
```

固件不要提交进仓库。工作流只在校验通过后才用这些文件，并把压缩包的 sha256 和解压出来的路径写进 `ci-manifest.json`。

### userdata 镜像

CI 能把 userdata 布局和整张镜像做出来，**输入只有一个**：Apple 的 iPadOS 18.7.10 / 22H374 restore（IPSW）下载地址，填进 `ipsw_url`。压缩、解密、挂载、组装都在 runner 上完成，不需要事先在别的机器上准备好任何东西。

链接必须是 https 且指向 `.ipsw`。填了 `ipsw_sha256` 就在下载完之后核对整个 IPSW 的 sha256，对不上直接失败，一个字节都不解压。

runner 上依次做这些事（`scripts/prepare-pong-userland-from-ipsw.py` 串起来，每步都是仓库里已有的脚本）：

1. 下载 IPSW（`scripts/download-ipados18-restore.py`，分段并发、可续传），核对大小，按需核对 sha256。
2. 用 `scripts/prepare-ipados18-components.py --ipsw` 从 IPSW 里取组件，并核对它确实是 18.7.10 / 22H374、机型是 iPad7,11 或 7,12。
3. 用 `scripts/decrypt-ipados18-aea.py` 解 OS 和 SystemOS 两个 `.dmg.aea`（AEA profile 1，公钥 FCS 解封装后交给 `/usr/bin/aea`）。
4. OS 解出来的 UDIF 用 `hdiutil convert -format UDTO` 展开成裸盘，挂载后作为 `--os-root`，同一张裸盘作为 `--os-image`。
5. 新建 5 GiB 的普通 APFS 卷，用 `userland/build-systemos18.py` 把原版 SystemOS 文件复制进去，产出 `pong-ipados18-systemos.cdr`。
6. 新建 8 GiB 的空 APFS 卷并转成裸盘，作为 Data 的起点（不沿用 iOS 16 的 Data）。
7. 跑 `userland/build-ipados18.py prepare` / `apply` / `finish`，产出 `result.json`、`apply-complete.json` 和 OS / Data 两个镜像。
8. 用 `scripts/stage-pong-userdata.py` 自己的校验函数检查这五个文件：名字齐、`result.json` 的 build 是 22H374、每个镜像是 4 KiB 对齐、开头有 APFS 容器（或含容器的 GPT）、三个卷 UUID 都能读出来且互不相同。

完成后写 `ci-userland-path.txt` 和 `ci-volume-layout.json`，这两个文件的下游格式跟以前完全一样，所以布局和镜像那两步不用改：生成 SHNGROOT 目录（`scripts/prepare-ipados18-ufs-layout.py`，要读完约 19 GiB 的三个镜像，这一段最慢），再按目录拼出整张镜像（`scripts/build-pong-userdata-image.py`）。

三个卷的 UUID 由工作流从镜像的 APFS 卷头里读出来，写进 `ci-volume-layout.json`，再交给内核的 `XNU_OWNER_*_VOLUME_UUID` 和引导清单的 root-match 值。**同一次 run 里编出的内核和这份 userland 是配套的**：UUID 是编进内核的常量，换了 userland 就要重编内核。

这一步要 macOS 并且要能免密 `sudo`：`apply` 和 `build-systemos18.py` 必须用 root 才能把原版文件的 uid/gid 写对。`sudo -n true` 不通过就直接失败，不会跳过一个 root 步骤假装成功——跳过之后产出的 userland 看着完整，但装到设备上会因为共享缓存归属不对而起不来。

一次完整链路的输入、每步耗时和产物字节数记在 `ci-userland-from-ipsw.json`（也在 `kernel-ci-log` 里），`ci-manifest.json` 的 `userland_from_ipsw` 一节引着它。IPSW 地址里的查询串（带签名的临时链接）在报告里会被换成 `?<redacted>`。

`userdata_image` 默认开着，产物按 4 份上传成 `pong-userdata-part0` … `part3`（单份约 5 GiB，保留 30 天）。单件 artifact 上限 10 GiB，整张镜像约 19.4 GiB，所以必须切开；下载后按顺序拼接即可：

```sh
cat pong-userdata-part0 pong-userdata-part1 pong-userdata-part2 pong-userdata-part3 > pong-ipados18-userdata.img
```

镜像本身只写到最后一个窗口的结尾，不是整个 454 GiB 分区；目录里没有描述的地方是零。写入手机仍然用 `scripts/install-ipados18-ufs.py`，一次一个窗口并最后提交 SHNGROOT 目录；这张整盘镜像只是同一批字节的另一种封装，方便检查、对比和整体留档，CI 不会把它推给任何设备。

本机已经有一份 prepared userland 时，也可以直接打包留档，用 `scripts/pack-pong-userland.py`：它核对五个文件、`result.json` 的 build 和镜像对齐，再把 `pong-ipados18-systemos.cdr` 收进同一个目录并打印 sha256。这条路径现在只用于本地留档和比对；CI 不再读它，`stage-pong-userdata.py` 的校验规则仍然用在这条路径和 runner 链路的最后一步。

```sh
python3 scripts/pack-pong-userland.py \
  --prepared ../private-inputs/22H374/prepared \
  --systemos out/ipados18/systemos/pong-ipados18-systemos.cdr \
  --output ../private-inputs/pong-userland.zip
```

镜像和 IPSW 都不要提交进仓库。工作流只在链路的每一步都成功后才用这些文件，并把 IPSW 地址、解出的组件字节数、读到的三个卷 UUID 和产出的路径写进 `ci-userland-from-ipsw.json`。

没填 `ipsw_url`（包括所有 push 触发的 run）时，这一步只写 `userdata-decision.json` 记 `skipped`，退出码 0，不是失败。

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

填了 `ipsw_url` 时另加 `pong-userdata-part0` … `part3`（各保留 30 天），是整张 userdata 镜像按顺序切成的四份，`cat` 起来就是 `pong-ipados18-userdata.img`。同一次 run 的 `ci-manifest.json` 里 `userdata` 一节记着布局、镜像字节数和每份的大小，`userland_from_ipsw` 一节记着 userland 是怎么从 IPSW 做出来的。

### 本地构建仍然从手机提取

本机构建不需要压缩包，还是从手机取固件。Wi-Fi 部分有现成脚本，在 REC 里跑：

```sh
python3 scripts/prepare-pong-wifi.py --serial <serial> --out ../private-inputs
```

它只读挂载 `modem_a`、拉取 `image/qca6490` 整个目录到 `<out>/qca6490`，再核对 `amss.bin`、`amss20.bin`、`m3.bin`、`regdb.bin` 和至少一个 `bdwlan*` 都在。`adsp.mbn`、`tfa98xx.cnt` 和 Adreno 730 的五个文件同样从这台手机的 modem / vendor 分区取出。构建器认的位置是：Wi-Fi 用 `PONG_WIFI_FIRMWARE`（默认 `$PONG_INPUTS/pong-wifi-firmware`），背光用 `PONG_AUDIO_INPUTS`（默认 `$PONG_INPUTS/pong-audio-inputs`，里面放 `tfa98xx.cnt`），Adreno 用 `PONG_ADRENO_FIRMWARE`（缺省是本次 guest 的 `out/ipados18/firmware/adreno730`）。路径解析都在 `scripts/pong_paths.py`。

两份输入可以共用：本地把文件摆好后，用 `scripts/pack-pong-firmware.py` 打成 zip 就能直接喂给 Actions，省得再单独准备一遍。

### 它不做的事

- 不刷机、不碰设备，一次运行只产出文件。
- 不自己造 userdata 的输入：布局和镜像只在填了 `ipsw_url` 时生成，userland 由 runner 从那个 IPSW 现场做出来。没填就 `userdata-decision.json` 记 `skipped`，退出码 0，不是失败。
- 不做任何降级：链路上任何一步失败（下载、组件校验、AEA 解密、`sudo`、`apply`、最后的五项校验）都会让这一步失败，不会产出"看起来完整"的 userland。
- 不把 userdata 镜像推给任何设备，也不做设备回读。整盘镜像只是把同一批窗口字节装进一个文件。
- 走 prelink 路径时跳过整镜像哈希校验和 ADT 审计（`build-ipados18-prelinked-boot.py` 传给 `build-ipados18-bootimg-local.py` 的 `--skip-image-verification`），换来的代价是 `image_sha256` 等字段为 `null`；容器结构校验两条路都做。
- 产物没有上过真机。CI 只校验容器结构和打包内容，不做任何启动验证。

## 源码边界与许可证

仓库包含板级源码、补丁、配置和脚本，不包含 IPSW、Apple 私有二进制、固件、认证数据或构建好的 userdata。它们由脚本在本机获取或生成，并存放于 Git 忽略目录。本机路径只写入 `local.toml` 等本地配置。

设备级代码采用 BSD-2-Clause-Patent；导入组件保留原始许可证及 SPDX 标记，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
