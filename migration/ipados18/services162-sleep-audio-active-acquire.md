# Pong：显示休眠后的原生音频与活动资源完成同步

依据 [GPU适配文档](https://unixnu.com/zh/porting/gpu)，使用真实CP时间戳和普通系统刷新日志继续定位。此轮不修改全局IOSurface缓存模式，不回退GPU路径，不更改A730寄存器或固件。

## services161设备证据

实际GPU仍完成扫描输出，捕获未见CP/SMMU故障或显式缓存同步拒绝。5秒连续窗口实际记录22–51帧/秒；这些窗口工作负载不同，不能当作受控性能对比。剩余慢帧的完成侧同步仍可达10–23ms。音频初始化有真实非零PCM、DSP WRITE_DONE及TFA9873 AMPS/SWS，但用户明确反馈熄屏解锁后无声；初始化成功不等于恢复成功。

## 音频修复

1. HAL保留客户端后再次StartIO不再仅依赖客户端`active`缓存。检查真实引擎/stream状态，恢复先前关闭的输出流；实际引擎停了则走原有硬件启动握手。
2. 关停先让TFA功放关闭并有界等候MANSTATE/SWS/AMPS退出，再停APM图和释放MI2S时钟。原先先停APM图可能撤走功放完成状态切换所需的音频时钟。超时保留时钟并记录实际错误，不假称关闭成功。
3. 每次真实图启动前，在功放掉电状态恢复易失的32bit TDM/输入/sink配置；MANSCONF先于PWDN释放，参考实际Nothing TFA状态顺序。模拟增益、保护、MTP及校准不改。
4. 保留161的请求代次确认及已提交PCM位置恢复；没有测试音、未实现输入端点或强制取消系统静音策略。

## GPU修复

能力位16独立协商显式完成侧同步；方法19增加phase2（同步本次使用范围并选择显式acquire）。Metal真实完成回调后、释放CPU/显示写者所有权前，对当前命令仍持有的buffer/texture/父view执行phase1。未参与协议的资源继续内核原有保守同步。实际错误反映到命令状态和显示handoff，不伪造fence或完成。phase0原协议保持兼容。

## 构建与实机

配套Darwin24内核、Turnip及Metal前端已编译成功；最终音频对象晚于HAL恢复源文件修改，已链接该修补。仅写系统分区修改band，Data128GB、USB/Finder、WLAN、亮度、HID、最大GPU档及真实优先级保持。自动重启关闭。设备结果另行追加。

已安装80MiB/10个修改band；128GB Data/SystemOS未改。Fastboot临时启动Sending/Booting均OKAY。未启用自动重启。设备捕获结果随后追加。

## 首轮设备结果

Fastboot Sending2.332秒、Booting2.879秒均OKAY；原生USB自动配置。首轮598345字节USB日志有真实GPU帧完成，未捕获CP/SMMU故障、主机所有权同步拒绝或kernel panic。实际2048次提交的累计缓存维护约6.686秒/78693MiB；160同计数约31.511秒/383376MiB，两者工作负载不同，只作为普通运行的开销记录。

连续窗口最高56帧/秒，驱动约6.8ms、等待GPU约3.1ms；另外窗口约26–42帧/秒。不能据此宣称稳定120Hz或所有慢帧已解决。没有超过33ms的内核慢任务记录；仍有一条海报前端54ms命令。

音频实际两次StartIO成功、两次非零S16→Q31 PCM、DSP WRITE_DONE及TFA AMPS/SWS。没有捕获关停超时或保留时钟错误。用户明确反馈这版熄屏解锁后仍无声，恢复修复未完成，不能用初始化成功代替听感确认。最后保留手机运行，无自动重启。

## 后续失败现场

实际晚期日志仍有新StartIO、非零PCM、DSP WRITE_DONE及功放AMPS/SWS，未出现静音控制切换。用户仍反馈无声。MercuryPosterExtension提交到scheduled等待由约20ms逐步增加到406ms，而捕获的一条内核慢任务CP执行仅590us；需要继续拆分提交前准备、调度及CPU缓存开销。
