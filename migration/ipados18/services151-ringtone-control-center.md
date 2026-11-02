# services151：铃声、控制中心与全局 Metal

## 设备证据

services150 保持实际 GPU scanout，Mac 的原生配对/会话/diagnostics 均成功。但后续 USB 日志记录 backboardd 的 SIGBUS：后台预热打开 `/Applications/MTLReplayer.app/Frameworks/MTLReplayController.framework/default.metallib` 时，AppleFSCompression 工作缓冲区申请失败，压缩分页返回28/12。此后还出现 BoAlloc NoMemory。未见这一轮 CP opcode fault 再现；不能把后续 UI 退出归为同一 CP 故障。

另一处 backboardd SIGABRT 是 `__NSCFNumber objectForKey:`。用当前缓存实际 `_objc_exception_throw` 导出反推出该进程 slide=0x1e750000，栈地址对应 CoreBrightness +0xf17f4/+0xf2094。读取实际方法元数据确认出错类为 `CBSliderCommitTelemetry`，它把 DisplayBrightness 当字典读取 NitsPhysical。不是 Vision/libfaceCore；未减 slide 直接查镜像会误判。

## 修复

- 只对精确22H374 CoreBrightness UUID、实际 PongPanel 和已确认方法 IMP/ABI，跳过可选 CBSliderCommitTelemetry 上报入口。实际滑块和NT37705 DCS调光保留；不发布虚构面板光学校准值。
- 预热只访问小于8MiB且未压缩的少量 UI framework shader 库，最多32条 pipeline；超过每250ms1个实际 commit 就让路。不扫描/打开应用、调试工具和AGXMetalA10私有执行库，不影响所有客户端按需使用Metal。
- 保留当前 core6790e7c、Adreno740a9a2cbb 的兼容适配、A730固件、ABI1映射索引与ring尾部CP_NOP。配套运行时增加可选 idle memory budget：Pong对backboardd设置128MiB、其他客户端32MiB dedicated image备用上限，每池最多1个空block；只有已无资源引用的空闲内存参与回收，真实GPU/DMA生命周期不变。上游默认行为在未设置环境变量时不变；源补丁0025已保存。
- 保留NativeRinger状态同步和78个原生铃声ID/路径/播放器；把匹配固件中的实际音轨用系统afconvert转成 M4R容器中的Apple Lossless。内部TonePlayer不一定经过可插入的AudioConverterNew；该格式使用已安装的软件解码实现，绕开无Apple硬件AAC的板级差异。未生成测试音、静音替代或虚构播放成功。原音轨仅备份在PongDev本地。

全局发布种子保留 `*`，真实A730/22H374设备可用才发布；不把未支持的Metal操作或未完成fence伪装成功。自动重启关闭；Data128GB、USB配对、Wi-Fi和实际音量键处理保留。系统从REC当前文件系统同步后只提交改动bands。

## 结果

实际构建、安装、临时启动和设备日志另记。编译和非零PCM不等于用户已听到铃声，UI首帧也不等于控制中心已稳定。

主机结果：78条原铃声音轨已完成ALAC转换，保留路径与原播放器；精确CBSliderCommitTelemetry入口修补和Metal插件编译通过。带空闲缓存上限的Rust配套运行库已完成优化链接。未运行测试、性能基准或镜像校验。

安装：从REC当前OS作为编辑源，完成48个改动band（384MiB）增量提交。配套启动镜像复用已完成的services150 A730内核/loader，并保留其原始构建记录；本轮更新的是151用户态和Metal运行库。自动重启关闭。

后续实机记录：Fastboot发送2.333s/启动10.131s成功，原生USB配对、会话与diagnostics均成功，实际GPU scanout与非零PCM继续完成。REC日志确认CBSliderCommitTelemetry修补在backboardd后续实例安装。另捕获backboardd-2026-11-01-015339.ips，SIGSEGV为Turnip常量子命令流分配失败后的未初始化cs，见services152；不应据151短期首帧判定控制中心长期稳定。铃声是否可听仍未得到用户确认。
