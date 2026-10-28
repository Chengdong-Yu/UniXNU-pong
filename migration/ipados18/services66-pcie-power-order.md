# services66：PCIe 电源域访问顺序修复

## services65 的实际日志

日志保存于 `/Volumes/Untitled/Pong-iPadOS18/audit-services65`。XNU 已注册全部八个 CPU，报告四个可管理内存段、9728 MiB，GPU CP 和用户客户端已发布，OS/SystemOS/Data 均挂载成功，8 GiB Data 保留。ADSP/APM 和 TFA9873 服务已发布。日志在原生 launchd 加载共享缓存时突然结束，没有保存 XNU panic 或 Wi-Fi 冷启动输出。

因此日志不能单独确定 900E 的硬件故障来源。源码审计发现新增的首个 PARF LTSSM 读写发生在 PCIe GDSC、AHB/AXI 时钟和复位就绪前。这与 Pong Android pci-msm 的启用顺序不一致，属于明确的未满足硬件访问前置条件。

## 实际修补

- 冷启动首先仅访问 GCC/TLMM，并保持端点 PERST 为低。
- 完成 RPMh 电源请求、GDSC 开启、实际总线分支时钟确认、core/PHY reset 后，才映射并访问 PARF/DBI/PHY/ATU。
- 按 Nothing pci-msm 的 `msm_pcie_clk_init`，在 GDSC 开启后选择外部 PHY PIPE 父时钟；PIPE 分支仍在完整 DT PHY 序列和稳定延迟后才开启。
- 失败清理使用 `hostMmioReady` 前置条件。仅有已映射地址不足以允许对未上电域执行中断关闭等清理写入。
- 保留实际 QCA6490 标识、Pong PHY 表、音频 LPAIF0、音量控制及“iOS版本”修补。没有禁用 Wi-Fi 以绕过故障。

## 构建和设备观察

使用已迁移的 format2 源码/硬件配置构建 Darwin24，不改变 Data，也不重写 OS。镜像哈希、镜像校验及设备回读按用户要求跳过。临时启动结果另存 `out/ipados18/temporary-boot-services66.json`；Fastboot 成功不能代表已经进入桌面，仍须观察实际启动结果。
