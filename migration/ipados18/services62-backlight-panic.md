# services62：修复亮度类重复注册，减少启动和 VM 热路径开销

## 真机日志

来源：`/Volumes/Untitled/Pong-iPadOS18/audit-services61/`，console 11884 字节、pmsg 123 字节、dmesg 63 字节。

首行已记录 **4 owned banks, 9728 MiB managed before kernel reservations, 4 KiB pages**。多区间 RAM 已进入 XNU；不能再将本轮故障认定为没有跨过 UEFI，不能将 9728 MiB 写成完整 12 GiB。

实际 panic：AppleARMPlatform 注册 `AppleARMBacklight` 时发现 `__kernel__` 已有同名类，`OSMetaClass.cpp:896`。根因是 services60 中添加的兼容 metaclass 先于真实 AppleARMPlatform 异步类初始化。

## 修复

- 删除 `PongBacklightOSClass` 和同名类构造代码，仅注册唯一的 `PongBacklight`。
- 工作循环异步等待真实 AppleARMBacklight（100 ms 间隔，30 秒期限）；就绪后再执行面板亮度回读和服务发布。
- 原生类缺失时只记录并停用该亮度服务；不替换 panic 处理，不阻止真实 Apple 类初始化，也不阻塞主启动线程。
- 保留 services61 的 UEFI 多区间接管方式和 4 KiB PMAP/VM，现有 OS、SystemOS、Data 不重写。

## 性能修改

- UEFI 只对实际填充的 payload/BSS、boot_args、ADT、可选 initrd/trustcache 和 trampoline 执行 cache clean；此前每次扫描完整 4 GiB 窗口的未使用空间。显示缓冲仍单独清理，boot_args 和内存总量不缩减。
- VM 页数组首个 dense index 在初始化时计算一次，保留初始化前 UINT32_MAX 语义；逐页地址查询不再重复查找该常量。`struct vm_page` ABI 不变。
- bank 范围查询使用有界 unsigned offset 比较，保留物理空洞拒绝和无效 index 的实际 panic。
- 保留 8 核配置、已有 CPU 策略与 GPU/Metal 路径。未运行基准，不宣称实测提升幅度。

本轮重新构建内核与 DarwinBoot24，沿用已安装的 services60 OS 及 8 GiB UFS Data。镜像哈希、镜像审计、回读、自动测试和基准均未执行。临时启动和亮度/界面结果分别记录，命令成功不等同于运行成功。

## 构建和临时启动

- 内核：`out/ipados18/kernel-services62/mach.development.pong`。
- 构建日志：`/Volumes/PongDev/build/Pong-iPadOS18-native/build-20261027T102323Z.log`。
- 加载器构建日志：`/Volumes/PongDev/build/Pong-DarwinBoot24/build-20261027T102323111726Z.log`。
- 最终镜像：`/Volumes/Untitled/Pong-iPadOS18/boot-services62/pong-xnu-initial.img`。
- 已执行临时 fastboot boot，Sending/Booting 均为 OKAY；回执 `out/ipados18/temporary-boot-services62.json`，实际进入桌面和亮度功能待本轮反馈。
