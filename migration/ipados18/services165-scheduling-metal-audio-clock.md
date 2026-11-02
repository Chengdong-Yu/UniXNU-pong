# Pong：上游调度、Metal与原生音频时钟

完整合入UniXNU `fix/gpu-scheduling-and-metal` 到本地核心（d6bd3dd），保留Pong组件覆盖和22H374配置。AdrenoMetal、Adreno740、XnuPrelink切换到分支对应提交；未重置已有本地改动。所有组件的主机检查源码随上游保留，本轮不运行测试或基准。

## 22H374 Metal/编译器接入

接入上游同UID共享缓存，保留沙箱私有目录回退；并发导入使用PID/序号临时文件，扫描跳过未发布项。Mesa实际构建开启shader-cache，IR3使用可写缓存目录。用户已有构建脚本保持原样，本轮覆盖命令为 `meson configure <Turnip-build> -Dshader-cache=enabled`，随后ninja。

逻辑队列汇总当前请求，降级或销毁时撤回提升。修复Pong Turnip只接受历史最强请求的问题；保留ABI1 priority method18及batch method17。优先级真实传入Darwin24任务角色与已有单环准入，不假称抢占。工作集建议受真实进程VA lease与RAM共同限制，避免将2GiB建议暴露给128MiB连接。

Poster纹理/管线/通道一致使用单采样，resolve处理保留Load/StoreAndResolve源内容；其他客户端保留MSAA请求。接入swizzle、真实allocatedSize、函数常量元数据/特化链接名、viewport/scissor数组及完整blit范围。translator0025保留大动态索引常量的共享存储、指针/GEP偏移和实际分配统计；已加入版本化补丁目录。保留Pong活跃导入所有权、阶段记录、真实fence、异步编译/完成QoS和4KiB ABI。

## 缓存维护

164中大量碎片IOSurface使每个物理span各做一组DSB/抢占开关。改为验证完整logical span后跨碎片以最多64页批处理；每批仍完成同样的clean/invalidate与DSB，保留device-memory例外和所有权。新增 `[gpu cache batches]` 与已有CP/调度/维护耗时共同判断结果。不关闭缓存一致性。

## 音频

源码中的旧HAL目录策略使原生音频不能依赖旧插件构造时机。增加独立audiomxd适配，在实际mach_timebase与原生AudioGetHostClockFrequency不一致且符合22H374已知24MHz常量时，通过现有受限PongAudio selector4修改真实映射常量。getter指令、调用者、地址、原值均校验，provider延迟时有界重试；未知实现保持原值并记录原因。保留164实际HAL尾部排空、启停锁、格式重建和原生锁屏音效；不合成PCM、不提高硬件增益。

## A730板级边界

上游双环源代码已完整在核心内，Pong不直接链接A740抢占平台。Pong当前CP_ME_INIT明确关闭抢占，无第二ring、抢占记录与GMU keepalive状态链。直接拷贝A740启动/上下文常量会破坏实际A730路径；本轮将通用Metal、编译器、缓存和当前优先级完整适配到现有ABI1，双环硬件接通仍待A730板级实现，不能宣称已启用。

## 构建与设备

Rust运行时release、22H374前端、原生codec、Turnip和Darwin24内核已编译；shader-cache启用后的最终Turnip及配套插件随后记录。自动重启关闭。更新前从REC取得当前未挂载OS作为基底，再仅写本轮改变band，Data128GB/SystemOS不变。实机结果在实际临时启动与日志捕获后追加。

最终启用shader-cache的Turnip及插件编译成功，96MiB/12个OS band安装完成。Fastboot Sending2.333s、Booting10.172s均OKAY。镜像：`/Volumes/PongDev/build/services165/boot/pong-xnu-initial.img`。日志在`/Volumes/PongDev/logs/pong/services165/`；此刻设备观察尚未完成，不将Fastboot结果当作音频或帧率成功。

第一次实机捕获：真正GPU scanout完成；4096次提交中4095次已完成，没有捕获CP/SMMU故障或`[gpu slow]`告警。第1024次统计维护63038MiB耗时1.887s；164对应累计81740MiB耗时6.866s，工作量不同，不能当作同场景帧率基准。壁纸commit10的inbox约1.074ms、held38us，CP/提交阶段17.660ms；这些仅是本次普通启动样本。

USB后开始的串口捕获遗漏了更早的音频clock记录。回REC读取pstore确认独立适配实际将24MHz改为19.2MHz，result=0，观察值19.2MHz；随后旧HAL确实以compatibility-clock-only模式加载，未接管原生PCM。初次关于旧HAL完全未执行的推断不成立，以实机记录为准。旧HAL在较晚初始化不能替代本轮较早的频率适配。

继续审计确认相邻的`AudioConvertHostTimeToNanos`与`AudioConvertNanosToHostTime`仍分别固定125/3和3/125（22H374反汇编0x1b756b38c与0x1b756b3b8）；仅修改频率getter不足以使换算一致。补充audiomxd专用实际mach_timebase换算、框架尚未加载时的有界重试、持久化clock诊断。新构建入口`scripts/build-pong-native-audio-runtime.py`显式链接CoreAudio导出，保留用户原有codec构建脚本。其他进程的转换调用保持原生。真实锁屏后可听仍需观察，不以WRITE_DONE代替听感结论。

REC只读检查确认`/mobile/Library/Caches/org.unixnu.metal`已有多个真实进程的pipeline日志和IR3缓存子目录；共享目录并非仅设置环境变量。上游family waiver保留，不因补接口而虚假宣称全部Apple GPU族能力。

补充适配库显式链接CoreAudio，保持已有codec构建脚本不变；使用新增`build-pong-native-audio-runtime.py`构建。时钟日志经既有kern.pong_diagnostic保留，构造阶段框架未就绪会有界等待。两个转换只在audiomxd改用真实timebase，其他调用者转原生实现。

补充音频运行库仅写入32MiB/4个改变OS band，GPU内核/插件保持首轮已确认配置，Data128GB未改。当前安装回执为`/Volumes/PongDev/logs/pong/services165/os-install-final/installation.json`。最终临时启动与日志随后追加。


最终补充版第二次启动出现真实CP/SMMU故障（FSR80000402、FAR0xe0d4100，IB1=0x2ea60000，IB2=0xe0d4000），随后DeviceLost。第一轮成功不能证明稳定，165不列为稳定候选。USB/CPU仍响应，显式回REC保留故障记录。音频pstore同时确认最终构造器加载、频率修正为19.2MHz和StartIO成功，但未完成锁屏后可听观察。
