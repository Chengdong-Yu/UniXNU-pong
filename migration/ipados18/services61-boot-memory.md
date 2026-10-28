# services61：UEFI 交接与分区内存审计

## 本轮依据

services60 用户报告只保留 UniXNU 菜单，没有 XNU 日志。REC 抓取保存在 `/Volumes/Untitled/Pong-iPadOS18/audit-services60`：console 776008 字节、pmsg 43220 字节，均为 Linux 内容，未包含 XNU early-console。不能将其判断成 SpringBoard、Wi-Fi 或亮度服务启动后卡住，也不能据此声称已定位某条 XNU 指令。

## 容量支持核对

| 层 | 当前实现 |
| --- | --- |
| UEFI 板级内存 | `uefi-pong/Silicon/Qualcomm/QcomPkg/Library/RamManagerLib/RamManager.c` 读取高通 RAM partition 的 64 位 Base/AvailableLength；`RamManagerDxe` 注册实际范围，没有固定 8 GiB 开关 |
| UEFI 保留区 | Pong `MemoryMapLib` 将 DXE heap 放在 0xf8000000、保留 FD/栈/日志/显示区域；额外交接不能耗尽其尚在使用的堆 |
| ADT 物理容量 | `dram-size=0x300000000`（12 GiB）；真实不连续范围来自 Pong REC FDT，不能用 base+12 GiB 合成连续地址区间 |
| 启动窗口 | 4 GiB 连续 AllocatePages；解析器允许到 8 GiB 是单窗口边界，不是总 RAM 上限，不直接改成 12 GiB |
| 加载器到 XNU | 独立 bank 契约记录成功分配的区间；boot_args.memSize 保持主窗口，memSizeActual 为交接总量 |
| 物理地址与页 | 通用 XNU 40 位 PA，可覆盖 Pong 0xa80000000 以下的真实 RAM；kernel/user/SMMU 维持 4 KiB |
| VM/PV 元数据 | 沿用 services60 的紧凑 bank 索引、额外物理 aperture 映射和反向查询；最终内核包含早期 DT 对齐复制修补 |

## 实际更改

- 额外分配前检查完整允许列表、溢出、重叠及 map stride；无效可选列表保留原主窗口。
- 不再申请低于 4 GiB 的额外空闲页，保留低地址 DXE 运行堆；额外内存仅接管真实 WB conventional 描述符。
- 高地址另留 256 MiB 固件池；每次 AllocatePages 最多 1 GiB，相邻成功分配合并为一个 bank。固件保留/显示/设备区不作为 RAM 使用。
- 屏幕显示实际 loader 阶段，失败显示阶段和 EFI status，避免失败返回菜单却没有可见原因。

本轮重新编译 DarwinBoot24 并以 services60-final 内核重新打包 services61，保留已写入的 OS 和原有 SystemOS/Data；不重新传 OS、不修改 boot 分区。不执行镜像哈希、镜像审计、回读、测试或基准。

此为对交接风险的修正，尚不能证明它是 services60 无日志的唯一原因。实际进入 XNU、bank 数量与可用容量仍需本轮启动结果；没有把设备报告的 12 GiB 等同于 OS 可分配 12 GiB。

## 临时启动

最终镜像 `/Volumes/Untitled/Pong-iPadOS18/boot-services61/pong-xnu-initial.img` 已执行 fastboot boot，Sending/Booting 均为 OKAY。回执 `out/ipados18/temporary-boot-services61.json`；该回执仅证明命令完成，进入 XNU/UI 和容量仍待观察。
