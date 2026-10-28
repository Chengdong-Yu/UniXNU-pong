# Pong → iPadOS 18 迁移

工作分支：`ipados18`。源码目录：`/Volumes/PongDev/source/UniXNU-pong-ipados18`。

目标是第七代 iPad Wi-Fi 型号 iPad7,11 / J171 的 **iPadOS 18.7.10（22H374）**，物理设备仍是 Nothing Phone (2) / SM8475。使用该 restore 的 arm64 内核组件，公开内核源码为 `xnu-11417.140.69` / Darwin 24.6.0。原厂内核的完整版本字符串是 `11417.140.69.706.66~1/RELEASE_ARM64_T8010`；公开源码不包含其全部闭源维护修改。

2026-10-24 的上游拉取、文档理解和合并范围见 [上游同步记录](upstream-20261024.md)。当前仍使用 Pong 的 SM8475 / 22H374 输入，组件来源与本机原始迁移基线分别记录。

最新运行记录：services34 已由用户确认进入界面；services36 的 REC 证明 A730 CP/ZAP/fence 和原生 Metal/Vulkan 设备创建完成，但 backboardd 在 22H374 新增命令缓冲区接口处崩溃。services37 已补齐该元数据接口、连接额度回收、壁纸客户端与 Settings 图标安装路径，实际呈现结果待本轮确认。详见 [服务与图形修复记录](services.md#services37darwin24-命令缓冲区与设置壁纸渲染路径)。

## 已落地的迁移

- `board.toml`、`feeds.toml` 和独立的 `pong_ipados18_defconfig` 已切到 22H374 / Darwin 24；iOS 16 构建树继续保留。
- `patches/xnu-ipados18/` 保存重定位到 Darwin 24 的 Pong 4 KiB 页、PAN、DT/CPU、控制台与 pstore、APFS 挂载、静态 init、开发签名策略、软件 keybag、输入及基础显示修补。
- 新增 `DarwinBoot24` 并完成编译。Darwin 24 ARM64 的 `CommandLine` 为 1024 字节，`BootFlags` 位于 `0x470`，`MemorySizeActual` 位于 `0x478`，`boot_args` 大小为 `0x480`。不能用 DarwinBoot22 的 608 字节布局直接交接。
- Pong 的 DT 平台、PSCI/GICv3 CPU、UFS 驱动和匹配 libdispatch 的 firehose 库已用新内核 SDK 重新编译。
- UFS 使用 22H374 的 IOStorageFamily 接口重新生成绑定，不复用 20G81 地址。该版本的抽象基类构造/析构被 LTO 内联；适配使用公开 IOBlockStorageDevice 的转发定义，并绑定真实原厂 vtable/metaclass。
- UFS OS 窗口同时识别旧 GPT 和新的裸 APFS 容器；数据目录与板级 UFS/SMMU 参数继续采用 Pong 的实现。
- AEA OS/SystemOS 已解包，实际卷 UUID 从挂载结果读取。新建独立的 8 GiB Data 磁盘，未使用 iOS 16 的可写 Data。
- `userland/build-ipados18.py` 已生成并修改新的 OS 与 Data 副本，保留原版 launchd/dyld、61 个匹配的拆分缓存及原始应用资源；核心启动任务没有缺失。语言预设为简体中文。
- 新用户态适配库使用运行时 HID 接口和 Pong 输入队列；不使用 20G81 的 QuartzCore、设置或图标函数地址。

完整 XNU 编译、链接与 22H374 原厂组件预链接已完成，首个临时启动镜像为 `out/ipados18/boot-first/pong-xnu-initial.img`（102,514,688 字节）。首版保留单核运行和 4 GiB 内存窗口；PSCI 多核代码仍保留。真机启动仍在推进，镜像构建成功不能当作设备已启动的证据。Adreno/Metal 的 iOS 18 私有 ABI 尚未接入，初始配置不链接旧 20G81 GPU 运行时绑定。

## 本机输入与输出

- restore 与组件：`apple/ipados18-22H374`，链接到 `/Volumes/Untitled/Pong-iPadOS18/22H374`。
- 解包镜像：上述目录下的 `decrypted/`。
- 用户态副本：上述目录下的 `prepared/`。
- XNU 构建树：`/Volumes/PongDev/build/Pong-iPadOS18-native/tree`。
- 卷元数据：`/Volumes/PongDev/build/Pong-iPadOS18-inputs/volume-layout.json`。构建器从此读取实际 SystemOS/Data UUID，防止使用 iOS 16 UUID。
- 源码导出的内核、loader 与布局：本分支忽略目录 `out/ipados18/` 与 `mac-build/`。

系统、Cryptex、Data 和固件仅作为本机输入，不提交 Git。构建不进行镜像散列、全盘校验或设备回读；SHNGROOT 目录保留其协议要求的 4 KiB 头部校验值。

## 构建入口

```sh
/opt/local/bin/python3.12 scripts/build-darwinboot24-macos.py \
  --package /Volumes/PongDev/upstream/UniXNU/package
/opt/local/bin/python3.12 scripts/build-ipados18-pong-macos.py \
  --skip-image-verification
```

修改 Darwin 24 工作树后，使用 `scripts/save-ipados18-patch.py` 导出补丁。内核链接完成后执行：

```sh
/opt/local/bin/python3.12 scripts/build-ipados18-prelinked-boot.py \
  --output-dir out/ipados18/boot-first
/opt/local/bin/python3.12 scripts/prepare-ipados18-ufs-layout.py
```

当前 SystemOS 使用普通 APFS 文件副本，先按下面的 SystemOS 小节准备，再生成 UFS 布局。三个卷的 UUID 均由 `volume-layout.json` 提供，defconfig 不提供固定卷 UUID。

UEFI 基础固件与 Qualcomm 固件沿用 Pong；新 loader 替换协议仍在内存中完成。先完成内核及组件链接，再写入新系统窗口并进行 fastboot 临时启动。

## 首版安装

`install-ipados18-ufs.py` 在 REC 中检查 Pong 的 userdata 几何布局，并从镜像直接流式写入三个窗口。每个已完成的区块记录在 `out/ipados18/install/installation.json`，中断后可用 `--resume` 继续；所有窗口完成后才提交 SHNGROOT 目录。新的 Data 起点在 OS 与 SystemOS 之后，不再沿用 iOS 16 的 8 GiB 起点。

```sh
/opt/local/bin/python3.12 scripts/install-ipados18-ufs.py \
  --serial dd72e429 \
  --boot-image out/ipados18/boot-first/pong-xnu-initial.img
```

## 首次 panic 的修复

首次真机已进入 Darwin 24 的 IOKit 初始化，随后在 CPU 注册过程中触发 `ArmCpu::initCPU(bool)+0x3c` 的空控制器访问。Darwin 24 在注册阶段提前启动 CPU，与旧驱动的控制器创建顺序冲突；现已把 Pong 的首次 CPU boot 延后到全部中断向量安装后，并接入新的调度器完成流程。详见 [Darwin 22 → 24 差异](darwin22-to24.md)。

修正版镜像：`out/ipados18/boot-cpu2/pong-xnu-initial.img`。完整内核编译和预链接通过，Fastboot Sending / Booting 均返回 OKAY；后续 REC 日志确认已越过原 CPU panic。该轮没有重写 OS、SystemOS、Data 或 boot 分区。

`cpu2` 真机已越过 CPU panic，确认 GICv3/CNTV、UFS 三窗口与 APFS 根卷挂载成功。后续因 kern_exec 残留 iOS 16 根卷 UUID 而跳过 SystemOS/Data，导致 helper ENOENT；现已将三个卷的 UUID 统一配置化。`apfs3` 已临时启动，其后续 REC 日志见下面的 SystemOS 问题。

## SystemOS 普通 APFS 副本

`apfs3` 日志确认 SystemOS 与可写 Data 挂载成功，helper 已 exec 原版 `launchd.ios`；随后 dyld cache `.09` 触发 APFS 的 16 KiB 文件数据校验错误，导致缓存不可读、initproc 退出。当前 4 KiB 内核与这条校验路径的粒度兼容性仍需进一步定位。

本轮通过正常文件复制，把原版 22H374 SystemOS 文件放到新建普通 APFS 卷，不继承原卷封印元数据、不修改 dyld 缓存二进制，也不将 I/O 错误改为成功。原厂 SystemOS 需事先只读挂载。示例：

```sh
hdiutil create -size 5242880k -type SPARSE -layout NONE -fs APFS \
  -volname Pong-iPadOS18-SystemOS /path/PongSystemOS18.sparseimage
sudo /opt/local/bin/python3.12 userland/build-systemos18.py \
  --source /path/original-systemos-mount \
  --image /path/PongSystemOS18.sparseimage \
  --mountpoint /path/new-systemos-mount \
  --output /path/pong-ipados18-systemos.cdr
```

将输出旁 JSON 中的卷 UUID、名称和容量更新到 `volume-layout.json` 的 `systemos` 项，再构建内核、打包并生成布局。容量从原始约 3.69 GiB 增为 5 GiB；已有安装必须先将实际 Data 向后搬移，再替换 SystemOS，最后提交目录：

```sh
/opt/local/bin/python3.12 scripts/replace-ipados18-systemos.py \
  --serial dd72e429 \
  --boot-image out/ipados18/boot-systemos4/pong-xnu-initial.img
```

此操作保留实际 8 GiB Data 的内容，完成记录位于 `out/ipados18/install-systemos4/replacement.json`。中断后使用相同参数加 `--resume`；不应重新运行首次安装脚本覆盖已使用的 Data。新布局占用约 19.39 GiB userdata，保留 Pong 实际分区边界检查。

本机 `systemos4` 已完成上述替换与 Data 搬移，随后从 REC 进入 Fastboot，临时启动返回 Sending / Booting OKAY。后续 REC 日志确认前次 APFS 数据校验错误未再出现，并暴露下面的文件压缩解码故障。未写入 boot 分区。

## SystemOS 文件压缩处理

`systemos4` 后续日志已无前次 APFS 数据校验错误，但 dyld cache `.35.dyldlinkedit` 在压缩算法 `0x702` 解码时失败，导致 `check_signature` 返回 EIO、PID 1 退出。首次 `ditto` 使用了默认的文件压缩保留策略。构建器现明确取消 `preserveHFSCompression`，不再把压缩数据和解压依赖带入新的 SystemOS。

对已有普通 APFS 输出，可原地展开其中压缩文件，无需重新创建卷或导出整张 sparseimage：

```sh
sudo /opt/local/bin/python3.12 userland/build-systemos18.py \
  --source /path/original-systemos-mount \
  --image /path/pong-ipados18-systemos.cdr \
  --output /path/pong-ipados18-systemos.cdr \
  --mountpoint /path/new-systemos-mount --materialize-existing
```

本机已展开 1,806 个文件，仍使用 D96FCEC3-6DE9-474F-8F38-E40FA084948B / 5 GiB。更新时通过 `--old-installation` 指定上次已完成的替换记录；相同布局会保留 Data 原位，仅写 SystemOS。`--stream-compression gzip` 可减少 USB 传输量，设备端在写入前解压，不会使目标文件恢复为压缩文件。

```sh
/opt/local/bin/python3.12 scripts/replace-ipados18-systemos.py \
  --serial dd72e429 \
  --old-installation out/ipados18/install-systemos4/replacement.json \
  --boot-image out/ipados18/boot-systemos5/pong-xnu-initial.img \
  --output-dir out/ipados18/install-systemos5 --stream-compression gzip
```

`systemos5` 的主机脚本报告传输完成，并成功执行 Fastboot 临时启动（Sending / Booting OKAY）。后续日志仍读取旧 SystemOS checkpoint xid 18，不能据主机返回成功认定文件系统更新已生效。本轮未搬移或重写 Data，也未写 boot 分区。

## 安装传输状态处理

故障文件在本机修复卷的 xid 37 中已是普通数据流，无压缩属性；systemos5 运行时仍报告旧压缩文件。旧 `exec-in` 流式传输没有 shell-v2 的远端退出状态，也没有设备端完成回执，无法可靠报告管道内部错误。

两个安装器现改为 `adb shell -T` 的无 PTY 二进制传输，要求每个写入区块在远端命令成功后返回特定回执，收到回执后才累计完成字节。gzip 路径保留管道错误处理。该回执报告写操作完成，不是镜像散列或设备镜像回读。修正版打包为 `systemos6`，使用原 SystemOS UUID、容量与 Data 位置。

本机 systemos6 的 5 GiB SystemOS 已收到全部 10 个区块的设备端完成回执；随后 Fastboot Sending / Booting 均返回 OKAY。Data 未重写、boot 分区未写入，运行时是否选择新的事务与越过压缩故障待下轮日志确认。

## SystemOS 原厂所有权

systemos6 的 REC 日志确认已选择 xid 37，之前的压缩解码错误消失。当前拒绝点为共享缓存 `owned by uid=99 instead of 0`；Darwin 24 的 root-owned cache 要求应当保留。

原厂源卷曾以 noowners 挂载，旧 SystemOS 构建器也没有指定目标卷 ownership。构建器现先启用源卷真实所有权，目标始终使用 `hdiutil ... -owners on`；复制或展开后按原厂条目逐项恢复 UID/GID/mode，不统一猜测组号。原厂共享缓存实际为 uid=0/gid=80。本机已修复 2,891 个条目，卷 UUID、5 GiB 容量和 Data 位置保持不变。

对已有普通 SystemOS 继续使用 `build-systemos18.py --materialize-existing` 即可同时执行所有权修复；已展开的文件不重复复制内容。修正版打包为 `owners7`，沿用原本 Darwin 24 内核的共享缓存归属及限定挂载检查。

本机 owners7 的 SystemOS 更新已收到全部 10 个区块的设备端完成回执，并成功执行 Fastboot 临时启动（Sending / Booting OKAY）。Data 保留原位，boot 分区未写入；是否越过 uid=99 拒绝点仍需后续屏幕或 REC 日志确认。

owners7 后续日志确认已越过共享缓存映射与 PID 1 退出点，原版 launchd 开始服务 bootstrap，未出现 kernel panic。目前处理 backboardd 的任务导入和系统代理 persona，详见 [服务与 SpringBoard 启动](services.md)。


### 重新生成本机 Data 副本

需要全新 Data 时，使用 `userland/build-ipados18.py rebuild_data --out <prepared 目录>`（macOS 管理员运行）。该命令从 build-plan.json 记录的原始空 APFS 模板转换完整磁盘，填充当前 22H374 seed 并恢复数值 owner/mode，正常卸载后替换本机 Data cdr。它不会读取或写入手机。正常的内核/运行时增量更新应保留设备 Data；只有需要重置时才由安装器选择 `--windows Data`。
