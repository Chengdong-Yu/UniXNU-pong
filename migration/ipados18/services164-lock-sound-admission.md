# Pong：锁屏音效启停与GPU准入等待

## 锁屏后音频

继续保留系统原生锁屏音效，不通过删音效或强制取消静音绕过问题。services163捕获两次实际APM重建、非零PCM、WRITE_DONE及功放AMPS/SWS；这不能证明解锁后已经可听。

源码确认保留HAL客户端的resumeOutput原先绕过设备ioGate，存在“读running=true、并发StopIO关闭最后一个用户、resume仍返回成功”的竞态。现在所有原生Start/Stop/resume共用设备转换锁；每个userclient另有请求锁，将重复启动、停止、关闭和active所有权串行。retained resume不增加users；关闭只释放一次。DoIO以原子active状态读取。真实StartIO结果、客户端ID、StopIO剩余users进入日志，以判断锁屏音效结束是否关闭了后续播放。保留163的媒体格式重建、5秒真实静音DMA保活、功放正确关停及首批PCM消费。

另修正原生HAL尾部被截断：最后StopIO原先立刻把running置false，尽管clientOutputEnd仍领先readAt；底层DSP的排空流程不能再拿到这段HAL PCM。现在冻结实际committed end，释放PCM gate让物理worker消费该尾部，在最多400ms（一个341ms ring加调度余量）内完成后才停止native reader。映射/设备所有权保持到StopIO返回；不读取新提交、不合成数据、不无限等待。日志记录committed_tail与unconsumed，区分真正消费完和超时。后续新StartIO由ioGate排序。

## 慢帧

依据[GPU官方文档](https://unixnu.com/zh/porting/gpu)的逐阶段捕获，services163壁纸prepare为个位到数十微秒，inbox/提交前积压却到数百毫秒。Turnip准入Busy逻辑等待inflight[0]，但该列表仅入队/淘汰、从未在完成后移除；因此可不断等待已经完成的旧fence并立即重试，增加IOKit调用和CPU竞争。

在真实WaitFence成功后，按单ring序列号清理该连接已观测完成的前缀；保留last_fence、真实未完成任务、原有挂起等待/错误处理和优先级。列表最多8项，使用32位序列号差值，不能据此跳过未完成fence。不是假fence，不重排或丢弃接受的命令。补丁为patches/mesa/0011-pong-retire-observed-admission-fences.patch。

## 结果

最终Darwin24内核（含HAL尾部排空和客户端锁）、Turnip已编译并完成配套签名。仅48MiB/6个系统修改band更新，保留128GB Data/SystemOS；临时启动/实机观察随后追加。继续保留163分段记录、完整导入的缓存一致性、原生GPU路径与已实现功能。自动重启关闭，系统只写修改band，Data128GB保留。

Fastboot Sending2.332s/Booting10.177s均OKAY，最终镜像为build/services164/boot-final/pong-xnu-initial.img。自动重启关闭。USB与音频/慢帧捕获随后追加。

实机结果：用户确认锁屏/解锁后仍静音，慢帧仍严重。捕获到非零PCM、DSP WRITE_DONE和两颗功放AMPS/SWS，StopIO实际尾部unconsumed=0；这些状态不足以证明可听。壁纸运行时提交前等待最高约2.3秒，CP执行常见8.7–14.5ms，缓存维护仍有6–12ms。继续在165修复原生CoreAudio时钟适配缺失以及优先级和缓存接入。
