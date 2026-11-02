# services150：界面冻结和原生铃声

REC 日志确认原生铃声完整接口已经执行，缓存静音 before=0、after=0，accepted=1；Ringtone 原生音量0.6763。DSP 完成真实 WRITE_DONE，TFA 接收非零 PCM，未改动工厂增益或保护。铃声迁移标志本来会让后续 SpringBoard 实例完全跳过同步；现在每个实例同步一次实际缓存状态，首次迁移解除默认静音，此后尊重用户静音状态。

显示冻结伴随真实 CP opcode error（cause=1）、scanout500ms超时、VK_ERROR_DEVICE_LOST 和 backboardd SIGSEGV；后台音频与按键继续，未发现这段记录有内核 panic。恢复上游默认后台有界管线编译，移除会在 UI 调用线程同步编译的 Exact 覆盖。

继续移植 UniXNU Adreno740 main a9a2cbb 中36c4e44的对象映射索引：不再在全局设备锁下搬动所有4KiB span；每次只构造目标映射的页表事务。保留 Pong ABI1固定对象预算、实际SM8475表绑定和全类物理别名检查；未引入需要 upstream 全局页所有权索引的 imports-only 快捷路径。原生页表逐叶检查、发布、TLBI和故障资源保留继续生效。也修正 metadata 分配失败时的悬空指针清理。

对照高通 KGSL adreno_ringbuffer_allocspace，把 ring 尾部不足以容纳完整 batch 的空间填成 CP_NOP，再从零开始。旧实现把每个 DWORD 取模，可能把包头和 payload 分拆到物理 ring 两端。padding 与 batch 一同计入未消费空间，先发布实际缓存再推进 WPTR。只有真实 opcode fault 时记录 RPTR 附近8个已存在 ring word，不在健康路径采样，不伪造 fence或自动复用不确定 DMA 页。参考源码保存在 PongDev/reference/pong，公开来源 https://github.com/qualcomm-linux/kgsl/blob/gfx-kernel.le.0.0/adreno_ringbuffer.c 。

保留149已编译的四项上游 Metal运行时更改：spare image复用、异步room keeper、真实内存压力回收和iOS线程QoS。设备仍使用A730固件、原有Vulkan backend、4KiB和22H374对象ABI。

本机内核/UI/Metal编译及设备运行结果分开记录。OS从REC当前未挂载文件系统同步后只写改动bands，128GB Data和USB配对保留。

铃声解码补齐：独立 codec 适配库现在覆盖 SpringBoard 原生铃声客户端；对已限定的系统音频进程，显式请求 Apple 硬件压缩解码器时，在创建阶段选择实际安装的软件实现，避免硬件句柄直到首次 fill 才失败。保留第三方 codec、PCM 和无可用软件实现时的原生错误。新的 init 与codec库配套更新，注入容量仍为1024。
