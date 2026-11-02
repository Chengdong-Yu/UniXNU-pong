# Pong：最新调度与Metal分支接入

根据用户要求再次fetch两次。核心先更新到4317907，之后再拉到135b0dabedd9eeac1d43b386843a877f4c974972；两次均fast-forward，Pong components.toml本地覆盖使用保留的stash恢复。未重置用户设备脚本或OS profile。对应AdrenoMetal=b82fa2751d62d3b0a8b9330a58d1506d9cdf0963、Adreno740=4db2aa75415775667a8d1c03058053102183f7de、GopFramebuffer=d0b4eeefba1c7b4c0e5526497836bf96aad92cfa，XnuPrelink=b1888c12456c46a2226d63c794465dfcee08d465。

## 新增渲染策略

全部Metal客户端默认单采样，保留NATIVE_METAL_SINGLE_SAMPLE显式覆盖和已有实际resolve/copy语义。没有关闭GPU或以CPU代替Metal。

上游GopFramebuffer使用另一OS的Legacy framebuffer布局；Pong22H374仍使用MiniIOMobileFramebuffer的原生surface完成检查。它已经在真实surface尚未完成时保留上一帧，copy presenter只在真实命令完成后退休；本轮加入同一诊断ABI关联，不拷贝17/27的类偏移、DPU寄存器或把查询失败当完成。诊断不会在未prepare描述符上查询物理段。

## 跨层诊断

适配上游NativeGpuTrace.h/Store/inc到Pong：实际PID/提交/退休/导入/显示决定，以及Metal command、runtime/Vulkan batch、附件/swizzle/resolve/event/fence链接。Rust translator0026按Pong已有stage summary手工合并；Turnip frame diagnostics保留实际Pong结构，不引用上游不存在的submit profiler。保留atomic submit/fence publication修复，防止BO回收snapshot落后于实际接受的提交。

启用由pong_gpu_trace控制，默认构建仍不启用；本候选打开bounded trace以捕获用户普通操作。像素采样代码随分支合并，但不强制启用像素采样/哈希。没有图像哈希、基准或伪造fence。原始导出保持Data `/private/var/logs/pong/gpu-{60,180,300}.trace.bin` 和对应pixels.bin；collector只映射root只读快照，不创建GPU core/scanout owner、不执行GPU workload、不重启。每个快照描述符配对保留到unmap/close，避免错误内存回收。

165-final原生音频适配保持：audiomxd24MHz->实际19.2MHz频率已在pstore确认；两个固定24MHz转换接口改用真实timebase。可听的锁屏/解锁恢复仍待用户实际观察。

## 构建/设备记录

将构建结果与实机分开追加。保留A730实际固件、ABI1 shared32-bitVA、native4KiB、8核、128GBData、USB配对、Wi-Fi、音量/亮度。上游A740双环源码已在核心中完整合入，Pong板级未实现二环/抢占记录，仍使用实际单环优先级准入，不冒充已启用A740硬件抢占。

最终Darwin24内核、Rust运行时、Metal前端、Turnip、插件和root只读collector均已编译完成。已安装14个修改band、共112MiB；Data及SystemOS未写入。原生GPU/fence/显示关联和锁屏恢复结果在本轮临时启动后另行记录。
