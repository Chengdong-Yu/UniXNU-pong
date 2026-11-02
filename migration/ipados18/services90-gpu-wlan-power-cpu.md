# Pong：桌面冻结、WLAN 供电、充电通知与八核调度

## 本轮设备证据

读取 REC `logs-services89/pstore`，没有以手机手动回 REC 推断系统自行重启。

- CPU0–CPU7 均有 `first boot completed`。EPSS 三个域分别请求 1.8048、2.496、2.9952 GHz；
  这些是请求频率，不代表硬件温控下的即时频率。单个 malloc cluster 不等于只启动一核。
- 桌面已经产生 GPU scanout 完成记录，随后 CP BV opcode fault：cause `0x2000`、
  packet `0x70970001`，后续 Metal 返回 DeviceLost。FSR `0x400` 是格式位，不当作 SMMU 页面错误。
  显示冻结有 GPU 故障证据，不能只归因于 CPU 慢或内存不足。
- WLAN 完成前五项物理电源读取，在后续采样失败时退出，尚未到固件加载。
  services83/85/86/87 同一路径曾完成六项检查和真实扫描；本轮不是固件被替换的证据。
- XNU 首次发布电池为 100%、4421mV、external=0；回 REC 后原厂驱动报告 CDP online=1、
  约 5V、Full。它们不是同步采样，不能将 REC 值直接写成 XNU 固定状态。

## 修改

1. 按 Nothing `gen7_ringbuffer_submitcmd` 和 Linux `a7xx_submit` 补齐 IB1LIST start `0x00d` /
   end `0x00e` 标记，使用户 IB 列表在内核 BR/BV fence 协议前明确结束。同步、真实时间戳、
   缓存维护和故障后 BO 保留继续存在。该缺项是否消除本次 BV 错误仍需新设备结果确认。
2. WLAN 物理电源在已有 100ms 稳定期限内重读失败的完整样本，不因单次 SPMI observer 读取
   失败立即退出，也不把部分旧数据当成完整样本。仍要求真实 enable/in-regulation/电压下限；
   未成功时不会访问未供电的 PCIe 主机。继续使用 Pong 活动 B 槽 QCA6490 固件。
3. 对照 Nothing `qti_battery_charger.c`，通知订阅 opcode 0x04 使用 TYPE_NOTIFY，而属性请求
   仍使用 TYPE_REQUEST。精确匹配订阅响应；之后由真实通知/周期读取刷新 IOPMPowerSource。
   满电与实际充电分开，保留固件充电控制，不伪造外接电源、充电电流或 charging=true。
4. 通过独立 `0002-pong-idle-cpu-preference.patch` 为非绑定、非实时线程增加板级 idle 排序：
   Cortex-X2 CPU7 → A710 CPU4–6 → A510 CPU0–3。候选必须已在 native idle/recommended map；
   X2 忙时其余核心继续接收任务，八核并行不受限制。实时线程、绑定、原调度锁与忙核选择保留。
   SM8475 编号和排序放在 ArmCpu，通用 XNU 不硬编码板级 CPU 编号；不伪装成 Apple AMP。

本轮只需内核更新，保留 services88 OS、services87 Metal、现有 UFS Data 及原生外设接口。
没有更换用户维护脚本，没有重置 Data，没有添加自己的音量 HUD 或隐藏不可用的系统控件。

## 构建范围

新增调度 hunk 首次触发既有补丁脚本对已应用主补丁的反向模糊回放，导致生成目录丢失 PONG
配置并编译失败。失败目录差异与旧 index 已保留在本机忽略目录；恢复生成源并严格核对补丁
状态后，将调度变化拆成独立补丁，主补丁与本轮前一致。没有 reset/clean 用户仓库或修改脚本。

Darwin24 全部编译和原生驱动链接通过；新 scheduler 调用有真实 ArmCpu 实现，无未解析符号。
预链接打包完成，CPU 数为 8，启动镜像 100112 KiB。Fastboot Sending 2.326 秒、Booting 6.828 秒
均为 OKAY，手机保持运行，未主动回 REC。OS、Data 与永久 boot 分区均未写入。
实际桌面稳定性、Wi-Fi 扫描/连接及充电图标尚待此次设备反馈，不能以编译或 Fastboot 成功代替。
未运行性能基准、镜像哈希或设备镜像回读比较。
