# Pong：帧同步批次和条件 PAC 接入

先阅读当前核心的 `docs/zh/porting/gpu.md`、`gpu-diagnostics.md` 和 `arm64e.md`。再次获取的 main 为04573c510c5fd4892ca6948739389141cc9e05f5，已经是当前135b0dabedd9eeac1d43b386843a877f4c974972调度分支的祖先，未再制造重复合并。核心完整保留arm64e profile、原生PAC密钥、threaded-starts和Metal Rust回调边界；Pong的RustPacBoundary.c与所选上游源一致。

## 实际帧证据

166从Data取回60、180、300秒的真实trace，每份包含约1.66万条记录。没有开启像素哈希或图像比较。可关联的显示fence查询均是reason1，未将失败当完成。该轮到9216次提交，未捕获CP/SMMU故障，不能据此承诺以后不会发生。

180秒快照中，backboardd命令4766从前端commit到完成35.890ms：调度通知12.062ms，真实CP执行4.617ms，内核退休22.646ms，前端完成35.890ms。提交前资源维护和完成后的CPU工作占用了相当时间，不能将35.890ms全部记为GPU执行时间。原始记录和逐层关联保存在PongDev的services166/native目录。

## 缓存所有权批次

Pong ABI1新增method20/flag32，每次最多64个范围。Metal同一命令的保留导入在一次调用内交接，Turnip协商能力，内核在一次设备锁和BO表遍历内处理每个匹配条目一次。与method19相同，发布执行真实clean/invalidate，完成获取只发生在真实GPU完成后；完整范围覆盖的reply bit才允许省略重复CPU VA维护。没有匹配的范围继续保守路径；旧内核使用原method19。没有跨命令复用"干净"结论，也没有关闭SMMU/cache/fence处理。

帧event61新增实际同步/剩余VA维护耗时：`command phase sync_us va_flush_us surfaces owners covered_ranges`，phase1为完成获取，phase2为提交前发布。内核`[gpu host batches]`记录调用、范围、实际条目和耗时。前端本轮使用O3，保留Rust release/fat LTO及已有优先级、shader-cache、单采样和真实resolve策略。用户已有构建脚本保持原样，实际编译命令记录在本轮build-report。

## arm64e/PAC 边界

把上游xnu-12377的native-pac-keys移到当前Darwin24源位置，作为最后一层补丁。`HAS_NATIVE_PAC`只在`__arm64e__ && XNU_PONG_NATIVE_PAC`时定义；当前22H374配置不设置此flag，继续arm64。不能把另一代内核补丁、arm64e kext和当前arm64用户态直接混合。

八个CPU在各自initCPU读取ISAR1/ISAR2/SCTLR，记录地址/通用PAC特性，不写密钥、不执行PAC探测指令、不改变用户态能力宣告。每核EL1密钥访问和EL0/EL1指令往返仍待资格确认。上游方案为每次启动共享密钥，不提供进程之间的PAC密钥隔离。这里记录的是源码合并和适配，不是已运行arm64e系统的证明。

166已确认audiomxd原生时钟从24MHz对齐19.2MHz，重启后有真实PCM/DSP完成和TFA活动。锁屏后可听输出仍待观察；本轮不靠删除锁屏音效、重置音量或虚构播放成功来掩盖它。

## 构建和安装

Darwin24内核、当前Turnip、O3前端和匹配插件已编译。系统仅安装12个修改band、共96MiB，Data/SystemOS未改。当前候选位于PongDev的`build/services167/boot/pong-xnu-initial.img`。最终实机运行结果单独记录，不以构建或Fastboot成功替代GPU/音频结果。


## 本轮实机结果

167 已临时启动并从 REC 取回真实 trace。前 5120 次提交未捕获 CP/SMMU 错误，scanout 有真实完成；1024 次提交时，30216 个范围合并为 1793 次请求，实际维护 30221 个匹配条目、累计473105微秒。60秒快照中多数完整交接约0.3–0.4ms，但 MercuryPosterExtension 仍有41–267ms等待，主要集中于调度/inbox/提交阶段。横跨熄屏暂停的长轮询不能记作纯 GPU 执行时间。慢帧尚未全部解决。

八核均读到 ISAR1=0x10011101211052、ISAR2=0，address=1、generic=1，密钥访问未探测。audiomxd 实际记录 native=24000000、actual=19200000、result=0、observed=19200000；这说明时钟改写生效，不证明锁屏后的可听输出恢复。
