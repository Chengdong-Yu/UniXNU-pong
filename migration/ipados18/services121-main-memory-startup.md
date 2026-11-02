# services121：main 优化与 dyld/GPU 分配失败

## services120 设备日志

REC pstore 保存了实际 panic：initproc/launchd.ios 因 dyld shared cache 未加载而退出，最终报 `/usr/lib/libSystem.B.dylib` 不存在。SystemOS 挂载和缓存目录探测此前均成功；随后 `check_signature[pid: 1]: error = 12`。这是分配失败，不能通过复制一份 libSystem 或忽略签名失败掩盖。

同轮 GPU 在保留 512 MiB BO 池后，native4K/SQE 准备返回 `kIOReturnNoMemory`。boot 中 KMA_DATA 范围只有约 620 MiB，最大大对象区约 542 MiB，而物理受管内存约 9728 MiB；不能将物理 RAM 充裕等同于内核虚拟地址足够。

## 修补

- kernel tables/firmware 在 BO 池之前准备；池上限/本轮启动值降为 128 MiB。池是优化，普通 BO 仍能以散页回退，不再预先占用 512 MiB 的大对象地址空间。
- 普通非连续 BO 从 IOKit pageable VA 区取得内核映射，再由 `prepare()` wire 后取得物理 spans；GPU 使用期间保持 prepared/wired，真实完成及映射撤销后才 `complete()`。这保留旧的内核映射和 WC 修正，不恢复此前未适配的 task-less system BO 路径。固件、ring、页表仍使用明确的连续受限分配。
- 合并 UniXNU main 的 native4K 签名缓存 I/O 优化：1 MiB 分块读取、大文件 IO_NOCACHE，避免预读大型 dyld cache 挤占文件缓存。原始 16 KiB 验证、派生 4 KiB 摘要及只读发布保持。Darwin24 使用已有的 `mach_vm_protect` ABI；采用单独 late patch，保持现有补丁的幂等应用，不修改用户脚本。

## main 与 Metal/GPU

实际 fetch 后 main 未前进：UniXNU `066c8d8`、NativeMetal `ce5e19e`、NativeAdreno `5c8c145`。没有把实验分支冒充 main。

当前源码/已部署匹配运行时已有 main 的参数化设备能力缓存、拒绝 encoder 对象、纹理行别名边界、handle index、按需对象块、独占映射快速路径及提交时避免重复 TLBIALL。Pong 插件源码补齐 main `461a842` 的每 pass GMEM/sysmem 自动选择，仅保留 nocb；现有 services111 插件二进制已经采用该策略，不需要为同一改动重复重写 OS。

保持 ABI1、Pong A730 固件/寄存器/SID、32 位 IOVA 和 22H374 的600字节 Metal device ABI；不引入 iPadOS27 的 arm64e/PAC thunk、A740 电气参数或实验 ABI2 前端。

NativeMobileDisplay main 也已拉取到 `33e941a`。其中直接 scanout 是默认关闭的 Sheng 双 VIG/DPU-bank 路径，需要板级 `native_ufs_dpu_map` 和 CTL latch；不能直接套用 Pong 的单 DSI 命令面板。此次保留 Pong 原来的最终显示拷贝，不把该专有寄存器路径当作已经适配。GPU 仍负责已接通的 Metal 渲染。

本轮不重写 OS/SystemOS/Data，自动重启关闭。编译、交接成功和实际进入桌面分别记录。

## 主机与交接结果

Darwin24 内核与镜像已构建完成，日志为 `kernel-services121c.log` 和 `boot-services121.log`。首次直接移入 main 的内部 `vm_map_protect` 不符合当前 BSD 编译接口，已改为本分支使用的 `mach_vm_protect`；后续编译通过。late patch 在主补丁之前自动逆向剥离，再按顺序重用，用户脚本未修改。

services121 Fastboot Sending 100112 KB / 2.333 秒、Booting / 2.920 秒均为 OKAY。仍使用已安装 services120 OS 及配套 ABI1 Metal 库，Data/SystemOS 不写入。实际是否越过签名分配、发布 GPU 和进入界面尚待设备运行日志。
