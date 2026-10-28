# services59 启动与控制台恢复

## 事实

- services58-final 的实际 Fastboot 回执为 Sending/Booting OKAY。
- 用户报告启动后不显示日志。REC 本轮 `console-ramoops-0` 为 2097140 字节、`pmsg-ramoops-0` 为 44601 字节；抓取内容只有 Linux/REC，未检出 XNU、Darwin 或 Pong 标记。
- 此证据无法确定精确停止地址，也不能把用户手动回 REC 认定为系统自动重启。

## 源码修正

- 撤回 DarwinBoot 在交接前接管额外 DRAM 区间的操作，恢复原来的 4 GiB 连续启动窗口。
- 撤回早期 `arm_vm_init` 的多区间契约读取、分段 physmap、紧凑 PV 索引和 VM 页反向换算，恢复原来连续内存的一致计算方式。
- 删除 ADT 的额外 RAM 交接属性，避免加载器和内核使用不同代的内存契约。
- 保留本轮 GPU/SMMU、Metal、Wi-Fi reset 顺序、显示尺寸/刷新策略及亮度驱动；Data 仍为现有 8 GiB UFS 卷。
- 保留 `-v`、pstore 和首帧图形输出后关闭控制台的现有逻辑，不提前隐藏启动日志。

新增内存路径是回归嫌疑，不把没有 XNU 输出解释为已定位到某条指令。撤回的完整源码与源代码差异保存在 `out/ipados18/services59-source-audit/`；其中 `services57a` 子目录是 services58 前未完成的快照重建，不能作为已编译 services57a 的精确源码，实际恢复采用 `stable-memory` 子目录的连续内存实现。

## 构建与运行

重新编译 DarwinBoot24 和 Darwin24 内核，再打包 services59；本轮不重新传输 OS/SystemOS/Data。依照用户“编译完成后临时启动”的指令执行 fastboot boot，不刷写 boot 分区。

未进行镜像哈希、镜像审计、设备镜像回读或基准测试。实际屏幕日志和桌面恢复情况需本轮启动反馈确认。

services59 已完成编译及临时启动：Fastboot Sending/Booting 均 OKAY，命令耗时 4.335 秒。回执：`out/ipados18/temporary-boot-services59.json`。这只证明加载命令成功，屏幕与系统启动结果待用户反馈。
