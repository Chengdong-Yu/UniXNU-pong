# services152：上游 GPU 集成、内存与键盘

本轮同步 UniXNU main6790e7c、Adreno740 a9a2cbb、GopFramebuffer19ef3a7，并把 AdrenoMetal 更新至831a9dc、UniXNU-sheng 更新至28b3028。Metal 新 HEAD 合并了其他分支历史，但相对已取用的a0f0287没有树内容差异；实际工作是把 Pong 尚未接入的兼容实现补齐。没有把 SM8550/A740 的固件、两环抢占寄存器或24A446指令地址移入 SM8475/22H374。

## 实际记录

上一版 REC 保存了 backboardd-2026-11-01-015339.ips：主显示线程在 tu_bo_init_new_explicit_iova 内读取0x1b20，x0/dev=0，调用方是 tu_cs_reserve_space → tu_emit_consts → draw。源代码没有检查 tu_cs_begin_sub_stream 的结果；分配失败时局部 cs 未初始化，后续写命令才崩溃。日志另有多个海报进程纹理/缓冲区 OutOfMemory。亮度 CBSliderCommitTelemetry 修补已实际安装；不再把这一轮不同的 SIGSEGV 当作亮度统计异常。

键盘资源检查确认目标有 KBLayouts_iPhone.dat、KBLayouts_iPad.dat 和 Dynamic，InputUI 的 UIDeviceFamily=[1,2]。偏好 AppleKeyboards 仍引用旧 Pinyin，而原生 KeyboardsCurrentAndNext 已使用 Pinyin-Simplified。用户说明字母可输入，仅123切换无效，整体键盘偏移/裁切；没有证据支持改动全屏触摸坐标。

## 改动

- 移入 Adreno740 的物理页所有权索引。Pong 的保留池全生命周期标记、散页 BO 持有到真实释放；所有权索引不可用或页超出覆盖范围时仍走完整冲突检查。只有不属于驱动 BO 且导入缓存类别相同的页才进入现有批量快速路径。保留4KiB叶子核验、TLBI、fence及隔离。
- ABI1共享32位GPU空间仍不重叠：backboardd由1024→1280MiB，SpringBoard固定512MiB，其余14个连接128MiB。前两槽按实际进程名保留。这是可用地址空间，非额外预占的物理内存；避免重现Darwin24 kernel data VA耗尽，wired池保持128MiB。
- Turnip常量子命令流分配失败时记录真实Vulkan错误；图形调用方和计算路径停止继续编码，vkEndCommandBuffer正常返回失败。只限制已retire且可释放的BO缓存（桌面96MiB、SpringBoard64MiB、其余8MiB，且不超过连接VA的1/4）；活动DMA和真实fence不变。补丁0005记录在patches/mesa。
- 新版 Mesa 的 Params标志2表示OwnedCoherent，而Pong ABI1的同一标志表示SubmitBatch，不能混用。Pong用户BO继续使用已接通的write-combined CPU/GPU映射，不引入伪造的coherence能力。
- 补齐上游CoreImage/MPS读取的gpuCoreCount，按Pong A730配置为4；保留现有全局*发现和真实GPU提交。沿用高Vulkan队列请求；不宣称ABI1支持A740硬件抢占。恢复预热的上游8commit/250ms空闲阈值，仍限制小型未压缩UI库、32pipeline及忙时让路。
- 独立InputUI进程接入本机phone idiom，只在hw.product=iPhone17,1且UIDevice方法ABI匹配时安装。不进入SpringBoard/容器/状态栏修补，不替换123事件或触摸坐标。迁移精确旧拼音模式，保留用户新增模式；一次性清理可重建键盘images缓存，保留词库/数据库。

## 构建与设备结果

内核、匹配22H374的前端/插件、Turnip、UI和静态init均按原生4KiB签名构建；启动参数967字节。未运行测试、性能基准或镜像校验。系统以REC当前OS为编辑基础，仅写变化bands，Data128GB、配对、Wi-Fi、铃声ALAC目录保留。设备实际启动及后续日志另记，不以编译或PCM证明交互/铃声已修好。

安装结果：以当前REC捕获的OS为基础，仅12个变化band、96MiB已提交；Data/SystemOS未写入。Fastboot临时启动Sending2.350s、Booting10.130s均OKAY。后续设备日志继续观察。
