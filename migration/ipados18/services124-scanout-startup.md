# services124：恢复首帧路径，重新启用 GPU scanout

目标：Pong SM8475 / A730，iPadOS 18.7.10 / 22H374。

## 本轮设备日志

REC 中 console/pmsg 共约 180 KiB。GPU ARC/时钟、GMU/HFI、ZAP、真实 CP fence、IRQ332 与 IOAcceleratorES 发布均成功。backboardd 创建真实 Vulkan/Metal 设备并返回给系统，22H374 8-bit compositor options 实际安装。启动参数确为 `pong_gpu_scanout=0`，运行库明确记录 GPU scanout disabled。

本轮没有 MiniIOMobileFramebuffer swap，也没有新的 panic，5/20/60 秒 GPU 状态没有保存；Data 中未发现本轮 35 秒 backboardd/SpringBoard 栈快照。不能把扫描输出关闭当作所有冻结的唯一原因。services123 新增 VM-backed BO 及共享客户端 pager 是本轮首帧前回归的主要可疑改动，尚无冻结时的线程栈证明具体锁点。

其他日志：cameracaptured 的部分 GPU capability 查询暂无答案，SEP/可选硬件服务/部分首启文件缺失。它们后面仍有服务继续启动，未观察到这些消息对应的致命崩溃。不伪造 GPU 核数、SEP 或 Apple 专用 scaler 来消除消息。音频实际 service/HAL 连接成功不等同于已出声，沿用现有接口。

## 修复

1. 将 NativeAdrenoMemory.cpp/.h 和 clientMemoryForType 恢复为已被用户确认能进入桌面的 services122 分配/映射模型，撤回 services123 的 VM-backed/WC memory-entry 试验。保留 services122 的共享 ring 全局退休、有界等候及锁外休眠修复。备份本轮前文件在 ignored out/ipados18/services124-before。
2. 本轮启动参数显式使用 `pong_gpu_scanout=1`，保留 128 MiB BO 池。构建脚本默认也是 scanout=1；本轮不修改用户维护的脚本，不再传入旧的禁用覆盖值。
3. 核对真实链路：backboardd -> 0x4148 独立用户连接 -> Pong framebuffer 的物理描述符 -> offered source pages -> Metal blit -> 实际 command completion -> PongScanoutComplete。模式、页/stride、timeout、DMA 退休检查保留。Mini framebuffer 已在尝试 GPU 帧后再次检查 quarantine；未知 DMA 完成不会转而并行 CPU 写入。
4. 保留已安装 services123 的兼容 Metal 组件：IOSurface 有界复用、内存压力回收/一次 OOM 重试、Metal 对象接口和 UUID 限定的 22H374 BGRA/EDR 策略。不重新启用 ABI2，也不重置 Data/SystemOS/OS。

## 状态

本轮只更新内核和启动参数，OS 运行库已安装，无须再次传输 9 GiB OS。构建与 Fastboot 结果完成后补记。用户要求保持自动重启关闭，临时启动后手机留在系统。没有运行测试、基准、镜像哈希、镜像回读或比较。

Darwin24 内核与 prelinked boot image 已构建；Fastboot 临时启动 Sending 2.337 s、Booting 10.176 s 均 OKAY（out/ipados18/fastboot-services124.log）。手机留在运行中，未开启自动重启，未刷 boot 分区。实际首帧/GPU scanout 完成及输入性能尚待运行结果。
