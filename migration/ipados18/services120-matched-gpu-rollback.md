# services120：恢复匹配的可启动 GPU 组合

用户确认 services119 仍没有界面，要求回退全部错误部分。REC 日志显示该轮已没有 services117/118 的 CP opcode 故障，但新前端仍停在初始化，缺少此前正常轮次的 SpringBoard 主队列亮屏完成记录。不能将“没有 CP 故障”当作桌面已恢复。

## 回退范围

- 从合并前保存的源码恢复 NativeAdreno 提交、BO、页表、共享地址空间客户端窗口、ABI1 和对应 ArmSmmu4K 实现。
- 恢复 Pong A730 板级有限页表和 services112/113 日志确认的 512 MiB BO 池；保留此前已接入的跨设备 SCM 串行锁。
- 恢复原来的 scanout wire/桥接源码及 Metal frontend。安装保留的 services111 插件、bridge、Vulkan 三件配套库；该组合延续至 services113。不能将旧 ABI1 内核与新 ABI2 用户态混装。
- 改动前的 GPU 源码保存在忽略目录 `out/ipados18/services120-before-rollback`，实验更新未丢失。`configs/pong-gpu-parity.toml` 明确区分归档实验版本与当前活动版本。

Wi-Fi、输入法、亮度、音频接入、rootful 环境、Darwin24 根卷策略、八核调度和已有 Data 不回退。用户维护的构建/CI 脚本不改动。自动回 REC 入口保持移除。

## 部署

仅更新 OS 内 GPU bundle，不替换现有 helper、UI 库、音频插件或 Sileo。由于设备启动后已写过可写 OS 的 APFS 元数据，不能把主机旧增量直接覆盖到设备；按既定完整 OS 写入流程部署匹配副本，Data/SystemOS 保留。不执行镜像哈希、镜像回读或性能测试。

本轮内核构建日志为 `out/ipados18/kernel-services120b.log`，Metal 恢复记录为 `out/ipados18/restore-metal-services120.log`。安装、临时启动与实际 UI/亮度结果分开记录。

## 已完成的部署结果

`install-services120/installation.json` 已记录 OS 9,663,676,416 字节完整写入，选择窗口只有 OS，跟踪状态为 services120、pending 为空。Fastboot Sending 100112 KB / 2.333 秒和 Booting / 10.176 秒均为 OKAY。设备保留运行，没有定时返回 REC。

本轮界面和亮度是否恢复尚无设备反馈；不能以完整安装或交接成功代替实际 UI 结果。
