# services153：按GPU文档重构内存与BGRA路径

依据 https://unixnu.com/zh/porting/gpu 对照当前主线，而非把SM8550/iPadOS27常量照搬到SM8475/22H374。用户在152报告随区域变化的颜色错误，并要求同时处理慢帧和性能。152日志确认真实GPU scanout持续完成；首段未见CP/device-lost/分配失败，但呈现驱动平均20–47ms、GPU等待约5–10ms，不能把它视为120fps或已经流畅。

## 路径更改

1. BGRA遵循官方默认：所有Metal进程在设备创建前强制TU_DEBUG=sysmem,nocb，仍由真实A730进行GPU渲染。保留32位BGRA/opaque scanout；不改变SM8475固件、GMU/CP、SMMU stream ID或Darwin24 ABI。
2. 从主线turnip-gmem-mutable-swap.patch选择移入fd6_view、GMEM resolve、2D/3D store/load、input attachment读路径及其轻量计数。线性图像不再沿用tiled mutable标记，GMEM搬运采用恒等swap，原生普通图像复制保留原语义。完整配套补丁0006；明确不带入同一上游大补丁中与Pong method17冲突的优先级/ABI2部分。
3. 普通用户BO从内核wired VA/保留池转为无task的kIOMemoryPageable|kIOMapWriteCombineCache IOBufferMemoryDescriptor；prepare锁定零填充页并由VM批量设定cache属性。客户端映射该描述符自身，同一VM对象/cache模式贯穿allocation→CPUview→GPU。固件、ring、表和显式contiguous请求保持原有物理分配器。
4. 用户BO的zero-fill、prepare及cache模式准备在全局GPU锁外完成，锁内只接收该次请求的已准备页面并登记句柄/映射。连接关闭或请求不再接受时，未接收页面在锁外归还。物理页所有权索引覆盖该新路径，异常释放保留原quarantine。
5. 保留prepare/fence/真实叶子检查和TLBI。新map仅写无效叶子，不再先等全GPU队列清空；free仍等待该客户端整个队列范围的真实完成，以保护IB间接引用的纹理等资源，不再等待其他独立客户端的作业。已有提交不重复TLBI、unmap叶子/ASID策略保留。
6. 取消默认固定BO池（启动参数0），释放128MiB保留块及其kernel VA；真正可用的用户BO由系统内存支撑，仍受ABI1各客户端VA预算约束。保持152合成器1280MiB/SpringBoard512MiB及其不重叠窗口，未宣称扩大A730地址宽度或接通A740两环抢占。
7. 保留运行时背景pipeline编译/iOS QoS、压力回收、bounded spare/BO reuse、全局发现*、按需加载和32条小型UI shader idle预热。去除前端已实现属性的重复运行时探测，MPS gpuCoreCount实际返回A730值，不再产生误导的generic missing-answer日志。

## 保留及限制

152键盘独立InputUI适配/原生123事件、拼音迁移、当前Data和配对保留；未重置Data或用户词库。铃声资源和原生播放器保持151的ALAC目录，不把PCM输出或编译通过当作可听铃声证据。系统从本次REC的实际OS作为更新基线，只写变化band。

主机编译和设备观察分别记录；未运行测试、性能基准、镜像哈希或回读校验。颜色错误是否完全消失和交互帧延迟仍需本轮真实设备观察。

安装记录：仅11个变化band（88MiB）提交完成，Data/SystemOS未写入。构建结果及原生签名已完成，配套启动镜像正在临时启动。

设备结果：153首个用户IB触发真实BR/BV opcode错误（CP cause=0x2001，RPTR=0x42，WPTR=0x65），之后设备保护性拒绝新连接0xe00002d8。该轮没有实际GPU scanout完成，较短present数字来自回退，不能视为性能提升。GMU、HFI、ZAP及内核初始fence此前已通过。153新BO路径不能仅凭prepare成功认定缓存一致。
