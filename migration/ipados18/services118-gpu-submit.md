# services118：首次 GPU 提交与启动停滞

## services117 设备证据

读取 `out/ipados18/logs-services117-retry-auto/pstore/`。Pong A730 的 SQE、GMU/HFI、ZAP、初始化 fence 和 IRQ332 均成功；随后第一次用户态提交触发 BR/BV opcode error：CP cause `0x2001`，RPTR `4`、WPTR `0x5f`。FSR `0x400` 是格式字段，本轮没有对应的 SMMU 翻译故障证据。随后 Metal 连接被拒绝，用户确认未进入界面。

本轮回 REC 没有匹配的同步/重启提交记录，不能作为自动回收成功的证据。

## 修补

- CPU 写 TTBR0 前，除等待已提交 fence 外，还要求实际 ring RPTR/WPTR 与 GPU idle。切回默认页表及销毁进程页表同样遵循此条件。CPU 切表不能借用 CP 包内置的同步语义。
- BO 的首次映射和复用映射都显式使用其实际 write-combine 策略；保留默认缓存 BO 与物理 framebuffer 的不同属性。
- 真实 CP 故障发生后，一次性保存已有 ring/IB 快照，不额外提交测试命令、不清除或伪装故障。
- 按用户最新要求移除 ArmCpu 的自动回收入口，镜像不附加 capture 时间/目标参数。屏幕日志、pstore 和正常 panic 处理保留。

复用已安装 services116 用户态与现有 Data/SystemOS；本轮为内核更新，不重写 OS。编译与 Fastboot 结果另行记录，不能据此宣称界面、GPU 渲染或音频已经恢复。

## 主机结果

Darwin24 内核与预链接镜像构建成功，记录分别为 `out/ipados18/kernel-services118.log`、`out/ipados18/boot-services118.log`。镜像位于 `out/ipados18/boot-services118/pong-xnu-initial.img`，使用实际八核 ADT；按用户要求跳过镜像校验。未改变已安装的 OS、SystemOS 和 Data。

Fastboot 临时启动成功：Sending 100112 KB / 2.288 秒、Booting / 10.168 秒均为 OKAY，记录 `out/ipados18/boot-services118/fastboot.log`。设备留在运行状态，没有安排自动回 REC。是否恢复界面尚待实际运行结果。
