# services131：USB 与可启动路径的冲突隔离

## 日志与版本对照

- services128 可进入系统：USB 低32位连续分配失败，尚未完成新的 DMA/SMMU/控制器初始化；真实音频已消费非零 PCM、收到 DSP WRITE_DONE，用户仍未听到声音。
- services129 把 USB 放入原音频预留区后，日志在 UFS SMMU 配置附近终止。审计确实发现根存储尚未加入共享 SMMU 锁。
- services130 加锁并等根存储就绪后，REC 26172 字节 console/pmsg 明确包含 UFS transport ready、OS/SystemOS/Data 挂载成功、/pong/init 执行和 writable Data seed。最后非诊断行是 `PongUSB: root transport ready; starting independent USB DMA`。没有后续 USB running、APM、PRM、TFA 或 panic。独立 pmsg 中 GPU CP 已完成并发布真实 provider。

因此，130 的根存储顺序修改让系统越过了旧停点，但没有修复 USB initialize 本身。日志不能区分内部具体哪一次 map/MMIO/清零发生停顿；不能将早先的并发问题直接当成整个故障的已证实唯一原因。

## 代码审计和修复

1. **控制器所有权缺失。** 原 USB 仅检查 GDSC、master/UTMI 时钟和设备角色，就停止/重置固件控制器。没有实际 loader 对 eUSB2 PHY、repeater、NOC/时钟保留与旧 DMA 退休的交接记录。poll 有超时并不意味着单次阻塞 MMIO 也有软件超时。131 在首次硬件访问前要求真实动态 `/chosen/pong-usb-handoff`；不能从静态 DT 或 bootarg 编造记录。当前 loader 不生成该记录，所以 USB 会明确跳过硬件接管，避免继续阻断系统。
2. **复用音频 DMA 契约。** 129 的两半数学范围没有重叠，但它改变了既有 `pong-audio-dma` 的64KiB独占用途，没有独立 USB 固件分配/所有权记录。131 恢复音频全部65536字节，USB改为独立 `pong-usb-dma` 32768字节，并拒绝与音频重叠。当前 loader 不提供该分配；不回退到音频区或任意物理地址。
3. **RAM 与寄存器映射类型。** USB原 Map 对所有区域都使用 kIOMapInhibitCache，但事件/TRB/数据是普通RAM，并使用memset/memcpy和USB字节缓冲。131 给DMA单独使用 kIOMapWriteCombineCache；寄存器仍用设备属性。这是一处独立风险修正，日志并未证明它就是本次最后停住的指令。
4. **共享资源顺序保留。** UFS/SMMU 锁和 USB 线程有界等待根存储就绪保留，不再撤掉已让根存储完成的修改。USB没有持锁等待根存储。

保留 native Speaker/PCM、MI2S PRM 0x104/3.072MHz、首个错误保存及安全失败重试。没有发布虚假的USB/Mux服务或假装USB已接通；当前USB隔离是恢复启动所需的回归处理，独立UEFI资源、完整板级供电和Apple USB服务接入仍待实现。

## 构建与部署

用户维护的构建脚本不修改，OS/Data/SystemOS及型号MYMC3LL/A保持，自动重启关闭。只构建并临时启动内核，不运行镜像哈希、读回比较或性能基准。完成后补记实际编译/启动结果，桌面与音频效果必须单独观察。

Darwin24 内核编译及匹配 prelink 镜像构建成功。实际临时启动 /Users/desktop/Downloads/Pong-build-services131/pong-xnu-initial.img：Sending 2.321秒、Booting 10.169秒均OKAY。没有写系统镜像或boot分区，设备留在运行状态。USB交接前提在本机loader中不存在，预期明确跳过硬件接管；不能称为USB修好。桌面恢复和声音须本轮实机确认。
