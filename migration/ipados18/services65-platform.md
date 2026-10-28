# services65：Pong Android DT、音量与音频修补

来源为 UniXNU 官方 input/network/power/migration 文档、已保存的 Pong Android DT，以及 NothingOSS SM8475 的 pci-msm、GCC 和 pinctrl 实现。最新 REC 基线是 services64。

## 已确认的基线

- Wi-Fi：PHY `0x214=0x86`，bit6 已清；PCIe ELBI=1、DLL 未激活，链路训练超时。
- 两颗 TFA9873 均读到 revision `0x0b73`；拒绝的 APM 命令为 `0x01001006`（端点配置），不是图创建命令。
- 两个音量键有硬件按下/释放和 HID 投递记录，SpringBoard 适配因错误的单例入口而未启动。
- 关于本机左侧标题方法错误地调用了版本值读取方法。

## 实际修改

PCIe 训练前先关闭 LTSSM 并保持 PERST，清理继承的 PCS 软复位、START 和模拟电源状态，再按完整 82 项厂商序列启动 PHY/PIPE。GPIO94/95/96、WLAN_EN80、XO204 保持 Pong 值。设备树中的 PHY 参数、AUX 频率、EP 时序与 GPIO 引用现由编译后的 format2 DTB 接入 Wi-Fi 构建；不匹配的 GPIO 配置会要求重新适配。

音频使用 MI2S LPAIF0、tertiary index2、SD0=1。端点配置失败时清理已经创建的图；用户态 HAL 只有真实硬件服务 Ready 才创建驱动实例。本轮启动参数显式设置 `pong_audio=2`。

音量控制器通过 `SpringBoard.sharedApplication.volumeControl` 获取，并检查方法 ABI。关于本机的标题固定为“iOS版本”，右侧数字仍由系统原生版本方法提供。

## 迁移与构建

`board.toml` 已采用格式2，使用 SM8475 共享源、22H374 OS profile、设备 DTS 和完整配置/patch series。共享源、Darwin24 OS overlay、社区组件目录与可恢复本地 SoC seed 见 `migration/format2/`。本机迁移已解析并编译，远程共享仓库尚未发布。

最终构建使用 `scripts/build-pong-format2.py`，生成的硬件 DTB 同时用于内核构建记录与 Pong ADT。没有直接套用主仓库默认 Darwin23 的预链接入口。构建回执为忽略目录中的 `PONG_FORMAT2_RESULT.json` 和 `out/ipados18/services65-ready.json`。

OS 用户态库和有硬件 Ready 门控的音频 HAL 已更新到现有 OS 窗口，现有 8GiB UFS Data 保留。最终镜像由 format2 目标目录中的内核预链接、打包，临时启动回执另存。没有执行镜像哈希、镜像回读或基准测试。

编译和打包成功不能证明本轮已扫描到热点或已完成扬声器播放；以新的设备日志和实际操作结果为准。
