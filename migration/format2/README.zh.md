# Pong iPadOS 18 仓库格式 2 迁移

按照 UniXNU `0fc0fc1f9ddd0fbd4787738afef38667df5e0a95` 的迁移说明，将现有项目原地改为格式 2。main 发布 Pong 的 iPadOS18 适配；主机构建和实机验证等级分别记录。

## 配置归属

| 层 | 文件与用途 |
| --- | --- |
| ARM64 | 主仓库 `profiles/arch/arm64`；4 KiB 公共配置 |
| SM8475 | 相邻 `UniXNU-sm8475-common`；CPU/GIC/时钟与控制器 DTSI |
| OS | 本仓库 `migration/format2/profiles/os/ipados-18.7.10-22H374`，安装到主仓库同名 profile；Darwin 24、Apple 源码版本及系统身份 |
| Pong | `board.toml`、`dts/pong.dts`、configs、固件选择、驱动和用户态适配 |

`feeds.toml` 和 `unixnu.lock` 固定 SM8475 与社区组件的公开源提交。SM8475 源位于本仓库独立的 `migration/format2/soc-seed` 子目录，通过格式2 feed 的 `path` 选择；没有填入不存在的官方 SM8475 仓库。迁移脚本可根据种子及提交元数据重建相邻的本地 SoC 开发仓库，再用忽略的 `local.toml` 记录覆盖。公开源固定版本与本地种子提交分别记录，二者源码相同但 Git 根目录不同。

`PongPlatform24` 是社区 Pong 实现的组件标识，保留 A730、QCA6490/ath11k、TFA9873 和 Darwin24 绑定。它没有冒用 Adreno740、Ath12kWifi 或 ShengUfs 的官方源身份。组件目录扩展保存在本仓库，迁移脚本安装到选定主仓库。现阶段将相关平台实现作为一个组件聚合，源码目录不移动，避免破坏相对 include 与现有补丁。

## 本机准备与构建

```sh
python3.12 scripts/migrate-pong-format2.py --unixnu ../UniXNU-upstream
python3.12 scripts/build-pong-format2.py --unixnu ../UniXNU-upstream --out /path/to/build-output --jobs 4
```

入口依次调用官方 defconfig、status、模块解析、树组装及 DTS 编译，再调用本项目已适配的 Darwin24 内核构建器。输出归入 `out/pong/ipados-18.7.10-22H374/pong_ipados18/`，其中包含 `resolved-profile.json`、`device-tree/hardware.dtb`、`kernel/` 和 `PONG_FORMAT2_RESULT.json`。

Apple 源码本地覆盖与本机路径写入忽略的 `local.toml`。`[pong_build]` 可填写 `tree`、`developer_dir`、`volume_layout`；编译缓存可在外部磁盘，最终产物仍使用上述目标目录。现有四项 Apple 源码版本保持不变。

已编译的硬件 DTB 被内核构建记录引用。打包时交给 Pong ADT 生成器，用其 CPU affinity、timer、GIC 和 UFS 资源生成原有 Darwin24 交接结构。Pong 的 PMIC/Goodix 自定义 ADT 属性仍由板级生成器编码。

```sh
python3.12 scripts/build-ipados18-prelinked-boot.py \
  --artifact-dir /path/to/build-output/pong/ipados-18.7.10-22H374/pong_ipados18/kernel \
  --output-dir /path/to/new-boot-output --cpus 8 \
  --kernel-arg 'pong_audio=2 pong_wifi=1'
```

Darwin24 的完整适配补丁保留在设备 patch series，并只应用一次。Darwin23 的 architecture patch series 没有直接叠加到 XNU11417；其适配版本已经包含于现有 Darwin24 聚合补丁。后续可按功能进一步拆分补丁和平台组件。

本项目使用自己的 Darwin24 构建、预链接和打包入口；主仓库默认的 Darwin23 `unixnu kernel/payload` 入口尚不能直接代替它。22H374 ABI 导入记录、固件、解密系统及 Data 均保留在本机忽略目录，镜像不做哈希或回读。

## 本轮硬件修补

- PCIe0：依据 Pong Android DT 的 PHY status、power-down、EP latency、PERST delay 与 AUX clock 生成输入；训练前关闭 LTSSM、保持 PERST、重置 PCS START/软复位/模拟电源状态，再运行完整 82 项厂商 PHY 序列并启动 PIPE。
- 音频：旧配置把 `MI2S-LPAIF-RX-TERTIARY` 的 RX 当作 LPAIF 类型，选择了 2。改为实际 LPAIF 0、tertiary 2、SD0 1；配置失败时关闭已创建的图。HAL 工厂始终返回匹配的现代 .driver 接口，仅在实际端点 Ready 时枚举输出设备。
- 音量键：硬件 press/release 已在 REC 日志中出现；改用 22H374 `SpringBoard.volumeControl`，替代不存在的 `SBVolumeControl.sharedInstance`。
- 关于本机：`softwareVersionString` 是左侧标题，返回“iOS版本”；右侧版本读取维持原系统方法。

最新修补见 [services73](../ipados18/services73-smmu-audio.md)：PCIe 已连通、DMA 建表成功，修复 TCR2/TTBR 写序及 audiomxd 本地路由策略。本轮只构建，未临时启动。编译成功不等同于 Wi-Fi 扫描、PCM 声音或按键动作成功。

## 发布记录与本地输入

公开仓库保留 `PongPlatform24` 的完整源码和固定提交，SM8475 作为同一公开仓库的独立子目录源；这不表示通用 SoC 仓库拆分已完成。`local.toml.example` 只给示例路径，`local.toml` 不提交。

```sh
# 结构化记录检查，不读取或校验镜像。
python3.12 ../UniXNU-upstream/scripts/porting-record.py check --device .
```

`.github/workflows/porting-record.yml` 固定官方检查工具版本。仓库带 `unixnu` topic。公开内容仅包含源码和配置；构建产物、实际日志、Apple 私有二进制和认证数据留在本机。
