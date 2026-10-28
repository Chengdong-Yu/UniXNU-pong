# services63：修复多区间 RAM 页表对象偏移

## services62 真机结果

日志保存在 `/Volumes/Untitled/Pong-iPadOS18/audit-services62/`：console 32025 字节、pmsg 1568 字节、dmesg 63 字节。

- 4 owned banks，9728 MiB managed before kernel reservations，4 KiB pages。
- CPU0 到 CPU7 均记录 first boot completed。
- 原生 AppleARMBacklight class ready，之前的重复类 panic 已消失。随后 DCS 回读报告 invalid response，只停用亮度服务，没有位于 panic 调用栈。
- UFS、APFS、SystemOS/Data、helper 已继续执行到 launchd.ios。
- PCIe DLL 仍未激活并训练超时，Wi-Fi worker 已按可选路径退出。

实际 panic 是 `vm_page_insert_internal` 的对象偏移越界：`obj=0xfffffff0094891c0`、`off=0xffffffffd4f56000`、`size=0x260000000`。使用本轮未剥离内核符号定位到 `pmap_object_store`；调用栈为 `pmap_enqueue_pages` → `pmap_pages_alloc_zeroed` → `pmap_expand` → 用户页缺页路径。

## 根因与修复

`pmap_object` 大小为实际接管的 RAM 字节数。原实现以 `PA - gPhysBase` 作为对象 key，仅适用于一个连续区间。Pong 主窗口可以高于额外区间，物理空洞也不属于对象；该减法会下溢，或得到超过对象大小的 key。

新增统一的 `pmap_page_object_offset()`：Pong 用已验证 bank 的 dense page index 左移 PAGE_SHIFT；非 Pong 保留原计算。同步修改三个路径：

1. 新页表页加入 pmap_object。
2. 普通 pmap 页表页回收查找。
3. PPL 页表页返回 VM 时的查找（当前配置未启用 PPL，也保持语义一致）。

没有扩大对象到物理地址跨度，没有屏蔽对象边界检查或真实 panic。未受管地址仍拒绝；入队/回收使用同一 key，物理空洞不占 metadata 或虚拟对象槽。

## 保留与边界

保留 9728 MiB 多区间接管配置、8 核、4 KiB 页、services62 的 payload 范围 cache clean 及 VM 首索引缓存；没有因 panic 回退成单窗口。保留 GPU/Metal、已安装 services60 OS 和 8 GiB UFS Data，不重传 OS、不刷写 boot 分区。

亮度回读和 PCIe 超时仍需后续各自处理，未将其声称为本次 panic 的根因或已修复。没有镜像哈希、镜像审计、设备镜像回读、自动测试或基准。

## 构建和临时启动

内核 `out/ipados18/kernel-services63/mach.development.pong` 已编译。最终镜像 `/Volumes/Untitled/Pong-iPadOS18/boot-services63/pong-xnu-initial.img` 已执行临时 fastboot boot，Sending/Booting 均为 OKAY；实际界面和 panic 是否消失待本轮结果。回执 `out/ipados18/temporary-boot-services63.json`。
