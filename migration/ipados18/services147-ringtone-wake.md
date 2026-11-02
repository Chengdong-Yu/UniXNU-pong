# services147：电话铃声与熄屏唤醒

## 设备依据

services146 当前日志中可以看到多次实际 MI2S、DSP WRITE_DONE、TFA9873 AMPS/SWS 与应用非零 PCM。用户反馈电话铃声仍无声，熄屏唤醒后音频丢失并闪屏。当前 Data 中原生铃声类别音量已为 0.6763，媒体已通过原生接口回读 0.75，不应再靠反复调大类别音量修复。`buttons-can-change-ringer-volume` 原为 false。

显示日志伴随 `scanout copy not submitted status=3` 和 IOSurface 等待超时。GPU scanout 会把 alpha 置为 0xff，但 CPU 回退路径以前复制源 alpha；Pong 的固件显示会混合 alpha，两条路径切换可能闪屏。

## 修改

- IOAudio2 在独立的 IO 转换锁下序列化 start/stop，并有界等待旧时钟线程退出。快速锁屏/唤醒、铃声预览的连续启停不再因为上一轮 10ms 时钟退出窗口直接返回 Busy。PCM 和音量控制不持有该转换锁，保留原有物理 DSP 启动结果与尾部 drain。
- 保留已工作的数字音量曲线、功放原厂增益/保护。通过原生 SpringBoard ringer controller 一次性启用铃声、开启音量键调铃声，仍允许用户后续切换静音。
- 通过当前系统的 TLToneManager 检查来电铃声：先校验来电类型的原生配置键、文件与 ABI，缺失或无声配置时选当前系统的原生默认铃声。已有可读铃声保留，不通过伪造路径创建成功。
- 软件解码适配扩展到 Preferences 和 tonelibraryd。只有这两个铃声客户端明确请求 Apple 硬件解码器时，先选系统实际安装的软件解码器，避免硬件资源延迟分配后的失败；其他显式编解码成功行为保留。
- CPU 显示复制与 GPU 相同地输出不透明 BGRA，黑帧也采用不透明黑，避免安全回退时的透明度跳变。GPU 正在执行或完成不确定时仍禁止 CPU 写屏。
- 显示 idle 时仅释放可选的 compositor import cache 持有，保持实际纹理、命令缓冲及 presenter 的三项 pin 所有权；覆盖 Vulkan OutOfDeviceMemory / OutOfHostMemory 的真实错误拼写并只重试一次。没有扩大未知 DMA 生命周期或伪造 GPU fence。
- 内核 exec_inject 配置及 init/tweak-refresh 的匹配缓冲扩至 1024 字节。新增铃声客户端后基本规则为 468 字节，旧上限可能无法再附加 ElleKit 规则。

## 更新方式与限制

以 REC 中当前未挂载 OS 为本地基准，只写本轮修改的 sparsebundle bands。保留 128GB UFS Data、合成组件标识、原生 USB UID/配对、现有 Vulkan backend。自动重启关闭。编译和临时启动不作为电话来电、实际扬声器或熄屏唤醒已正常的证据；以设备日志和用户观察继续确认。

## 安装和启动

本轮局部 OS 更新 100,663,296 字节已完整写入（12 个 8MiB bands），未重写 OS 整卷。Fastboot 临时启动 Sending 2.404s / Booting 10.177s 均为 OKAY。手机留在运行状态，自动重启关闭；后续设备原生铃声和唤醒表现另记。
