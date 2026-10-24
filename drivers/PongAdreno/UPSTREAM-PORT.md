# NativeAdreno / Pong 20G81

完整 NativeAdreno 源码导入 submit/。本次源码版本见构建目录 native-adreno-import.json；保留 MIT 许可。

实际运行链路使用 NativeAdreno 的 Ring、CPInit、Timer、IRQ、Pwrup、Cpu、Memory、SmmuMemory、SmmuContext、Vm、Submit、UserClient、Publish 和 Accelerator。Board/Client 同时链接。旧 PongAdrenoClient 包装器删除；静态库重建前删除旧归档，避免残留成员重复链接。

以下设备/运行时差异是必要适配：

- 采用 Pong 实际 A730 SQE、GMU、ZAP，SM8475 寄存器/IRQ/GMEM/时钟/RPMh。A740 的 HFI 队列结构、Cape PLL 配置、ACTLR、LPAC stream 和 DMS 配置无法只通过替换固件文件解决；这些部分由 PongAdrenoPlatform/Gmu/Power/Clocks/Smmu/Cp/Zap 提供。
- 保留 v108 已在 Pong 硬件完成的无 fence bootstrap、ZAP/PAS13、nonsecure 和真实 timestamp 顺序；LPAC 的1/0x401与主CP的0/0x400共享自有页表。
- IRQ 使用匹配 Nothing GEN7_INT_MASK 的启用位，删除 A740 的bit29。v108真实fence已完成，之后的IRQ mask回读失败是本次直接停点。
- NativeAdrenoPublish 原有多客户端窗口、BO视图缓存、映射清理、selector tunnel、mailbox、wait分段让锁逻辑整套接入；补入实际 NativeAdrenoStatus 和 Pong scanout 的所有权/协议适配。
- AGXAccelerator 使用实际20G81 IOAcceleratorES祖先，由原有晚初始化机制加载，工厂和属性读取使用上游实现。
- GMU通用服务/SM8550启动器等源码一并导入；Pong实际硬件启动入口由上述SM8475适配负责，避免两个owner操作电源和SMMU。未执行固件哈希路径。Sheng专用的shader-export持久化区域在Pong不存在，保留上游缺失可选导出后端时的失败语义，消除静态链接的未定义符号。

## GPU-only

native IOSurface/framebuffer通过pong_display=1启动，与是否允许CPU渲染无关。删除framebuffer的CPU drawLayer回退和CPU清屏回退。只有pong_scanout_frame返回1才计为已完成；2表示DMA不确定，会保留页面和上一帧。

20G81 UI适配在backboardd私有映射中禁用CA::WindowServer::Server::sw_renderer创建入口；保留原始EAGLServer::renderer中的真实Metal初始化。没有修改系统QuartzCore文件。删除旧的无操作renderer包装、仅限软件阶段的海报/壁纸限制和GPU预热跳过。依赖Metal创建失败的系统请求不会被伪装成成功。

保留BGRA scanout格式约束，因为它是实际面板输出布局。设置图标CPU缩放适配仍是CPU图像处理，并非CoreAnimation软件渲染回退。
