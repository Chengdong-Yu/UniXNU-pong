# services149：iPadOS18 原生铃声与最新 GPU/Metal 适配

## 铃声修复

services148 已恢复 XNU USB、实际 PCM 和 GPU scanout。日志确认默认来电铃声解析到实际文件，但 `ringer controller restore did not complete`。从实际 22H374 SpringBoard 方法元数据和实现确认：`SBRingerControl` 的简短 getter/setter 是 objc_direct，没有普通 Objective-C 方法入口；对它们做 method ABI 查找会一直失败。

改用本版实际公开的 `setRingerMuted:withFeedback:reason:clientType:`，ABI 为 `v36@0:8B16B20@24I32`。通过运行时 `_ringerMuted` 的名字、类型和 offset 读取原生缓存，不引用旧版固定偏移。即使本地初始缓存是 false，也向原生音频服务发起一次启用铃声请求。保存迁移标志后，用户仍可正常控制静音。保留本轮已经完成的铃声解码、78 首资源、音频启停竞争和唤醒显示修补。

## 最新上游

本机 UniXNU core 从 f0721e9 快进到 6790e7c，保留本地 PongPlatform24 component 和 iPadOS18 profile。同步其 main 固定的 AdrenoMetal a0f0287、Adreno740 a9a2cbb、GopFramebuffer 19ef3a7、ArmSmmu4K 83969cd。AdrenoMetal 的 main 合并头 831a9dc 与该固定点无源码差异。

移植到 Pong 的实际运行路径：

- Adreno740 01ed692：可选连续 BO 搜索限制在 128KiB，失败后五秒退避，大 BO 回普通 wired 分配。保留 A730 的 native4K、PA 宽度和必需固件/队列连续分配。
- Adreno740 dd43abd：等待未完成 CP fence 时只在锁外作提示性进度读取；真正完成、错误、超时和资源退休仍走原有锁内 dispatch。保留 ABI1 的共享页表修改串行化。
- AdrenoMetal 0021：空闲 dedicated image block 重用，内存池预留移到有合并请求的后台 room keeper，减少纹理创建线程阻塞；实际内存压力与显示 idle 释放可选缓存。
- AdrenoMetal 0022/0024：运行时阶段追踪和 allocation-site 记录统一受诊断开关控制；默认关闭，不对桌面持续采样。
- AdrenoMetal 0023：iOS 上编译/完成线程使用实际 Darwin QoS。
- 当前主线桥接 API：以 HostPageTexturePolicy::InPlaceWrites 保留可直接写入的 host-page 图像；以 PipelineWaitPolicy::Exact 等待真实管线，避免慢编译时丢绘制；refuse_rebinds 改为 Device 实例方法，SharedEventListener 适配新的构造返回类型。OS-facing 方法与对象布局仍由 22H374 前端选择，不保留已经从运行时删除的 MetalLevel 字段。

最新原始补丁保存于 `patches/metal2vulkan/upstream149`，应用到本机已适配 22H374 的 624bab39 translator/runtime，Rust/C ABI 新增的缓存回收接口同步实现。A740 的双 ring 抢占寄存器、SM8550 固件、ABI2 独立地址空间和 Darwin26 指令偏移不替换 Pong 的已工作实现；这些依赖不同硬件或 OS ABI。已有 opaque scanout、shader priming 和缓存语义保留。

## 构建与设备结果

只构建实际 kernel/runtime，不运行性能基准、镜像哈希或回读比较。配套 OS 从 REC 当前未挂载系统同步，之后只写此次修改的 sparsebundle bands；Data 和 USB 配对保留。编译、安装及设备观察分别记录于 PongDev services149。

services149 配套内核、UI、Rust Metal bridge 与插件已完成编译；只提交 8 个改动 band（64MiB），保持 128GB Data、实际 Vulkan backend 和原生 USB 配对。Fastboot Sending 2.335s、Booting 10.179s 均为 OKAY。设备运行结果另行记录，不能由构建成功推断铃声已可听。
