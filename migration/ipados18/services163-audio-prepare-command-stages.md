# Pong：休眠后播放重建与慢帧分段

依据[GPU官方文档](https://unixnu.com/zh/porting/gpu)定位真实慢帧，针对22H374、Darwin24及A730 ABI1。所有记录都是普通运行日志，没有基准测试或镜像哈希。

## 已确认问题

services162用户明确反馈熄屏解锁后仍无声。实际HAL继续接收非零PCM，DSP有WRITE_DONE，TFA9873已工作，不能把初始化成功当作可听恢复。海报命令scheduled延迟逐步增长至406ms；内核捕获的一条任务CP仅执行590us，CPU发布/完成侧同步各约10ms。

## 本轮修改

- 每次实际APM重启，在PREPARE之前重新配置Pong MI2S SD1 endpoint、PCM converter和shared-memory media format。保留真实已映射DMA句柄与图所有权；失败沿原错误回退。
- 参考Sheng的5秒短时保活，StopIO后仅继续真实静音DMA；native HAL已停止，不读取旧流。超时正常功放关停/图STOP/PRM clock release；长时间睡眠走完整媒体配置恢复。功放模拟增益、保护和校准不变。
- 新native播放从刚清零的ring起点消费，不再丢掉除末尾20ms之外的首批PCM；保留stream恢复时使用实际时钟/有效ring范围的处理。
- 对本阶段已成功物理同步、覆盖整个surface的实际导入省去重复user-VA flush。内核只有匹配完整导入才报告whole-range成功；部分命中、旧协议、copy backing和错误保留原有维护。
- 修正前端忽略newCommandQueueWithMaxCommandBufferCount的问题，把调用者指定容量传给运行时真正的slot/backpressure机制。默认队列仍是原生64；不丢任务，不伪造完成。
- 使用运行时原有commit/run/prepared/handed/taken/held/submit/poll/seen环，增加仅时间线开关，避免开启大型分配backtrace。每条慢命令读最近最多16384条记录，记录真实held原因和阶段耗时；缺失数据为-1。每进程最多4条/秒。

## 构建和设备结果

Darwin24内核、Rust运行时桥接及Objective-C前端已编译成功，原生页大小签名已更新。已写入56MiB/7个系统修改band，Data/SystemOS未写入；临时启动与设备捕获结果随后追加。听感与解锁恢复需要实际设备确认，不能从DSP回执推断。最高900MHz/DDR11请求、真实优先级、GPU呈现、USB配对、WLAN、亮度、HID和128GB Data保留，自动重启关闭。

## 实机捕获

Fastboot Sending2.331s/Booting10.174s均OKAY，原生USB自动配置。两次实际媒体恢复/APM启动、非零PCM、WRITE_DONE和TFA AMPS/SWS已捕获；解锁后可听恢复仍未确认，不能宣称解决。壁纸提交前prepare只有个位到数十微秒，inbox/submit-before阶段却有数百毫秒积压，scheduled最长434ms。源码进一步确认Turnip的在途fence列表未随真实等待完成清理，准入Busy可不断重试旧fence。另发现原生保留客户端resume绕过启停转换锁，锁屏StopIO可与其竞态，下一候选一并修复。
