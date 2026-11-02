# services145：原生媒体与铃声音量

## 设备依据

services144 已有用户确认：锁屏音实际出声，但非常小，其他声音仍听不到。该版发布的真实 IOAudio2 Speaker 控制值为 1，静音值为 0。S16 输入转为 DSP Q31 后采用 1/16 的线性数字增益，尚不能据此判断媒体类别的音量是否同样过低。

通过 REC 读取选定 22H374 的 CoreAudio / MediaExperience 实现：`HALS_IOA2UCDevice::SetControlValue` 的 selector 2 传入顺序确实是 `[controlID, integerValue]`，与本机驱动一致。旧版 HAL 的曲线读取调用 `CACFDictionary::GetFixed64`，新版 `make_level_map` 也按有符号 32.32 解码；曲线单位保持一致。保留已经适配的 0..16 曲线与原厂 TFA9873 B0 配置，不调整模拟增益和保护参数。

## 修改

SpringBoard 的既有运行库在加载完成后，通过原生 `AVSystemController` 查询 `Audio/Video` 和 `Ringtone` 类别。每个类别仅做一次迁移：若原生音量不超过 0.125，则通过 `setVolumeTo:forCategory:retainFullMute:` 请求 0.5。保留原生 full-mute 状态。高于该值的现有设置保留，成功回读后持久记录，后续按键、滑块和开机均不再覆盖用户选择。

调用前检查 Objective-C 参数和返回值 ABI。操作放在独立串行队列，启动时延后执行；服务尚未准备好时最多重试十次，不在 SpringBoard 主线程、dyld 回调或外部 HAL 实例中操作。实际请求与回读记录在 `/private/var/mobile/Library/Logs/Pong-audio-volume18.log`。

## 构建与限制

运行库已通过 iPhoneOS SDK 编译并按 4KiB 签名。复用 services144 的内核、Metal、USB 配对和 128GB UFS Data，仅替换 `/pong/libpong-ui18.dylib`。音量回读、驱动最终控制值和实际听感须以临时启动后的设备结果为准。

## services145 设备结果

OS 运行库安装完成，Fastboot Sending 2.319s / Booting 10.174s 均为 OKAY。原生配对、会话和 diagnostics_relay 查询均成功。媒体类别回读 0.6473、铃声类别 0.2974，均高于该轮低值迁移阈值，因此未覆盖这些设置；物理音量仍为 1/16。用户随后确认整体声音已正常但偏小。services146 按新的明确要求抬升默认类别音量和低档数字曲线。
