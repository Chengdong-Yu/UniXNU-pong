# services141：原生 USB 启动与确定格式的 PCM 通路

## 实际停点

services140 的 Mac IORegistry 已显示 Apple Inc. / iPhone、05ac:12a8、配置1、480Mbps 和 SupportsIPhoneOS。CDC及显式REC命令可用。原生Mux已向62078发送真实请求，但 QueryType 超时；这不等于Finder配对已经可用。

读取手机 Data 中 `/logs/lockdownd.log`，多次启动均停在 `-UseRaptorCerts` 更新后。22H374 原生指令接下来调用可选的 FairPlayGetHWInfo/CESM 初始化与证书提取，返回空证书后原生代码仍能继续正常启动。Pong 没有这套 Apple DRM 硬件。

`userland/usb_services18.py` 新增有界、同构建限定的本机适配：解析 Mach-O 可执行段，把两个可选调用分别改为 NOP、NULL，沿原生缺失证书分支继续。拒绝未知指令或不完整修改；保留原始二进制于本机并保留 entitlements，以4KiB页重新签名。不修改原生配对、信任、TLS、服务权限或制造证书。root/wheel及真实监听socket配置保留。静态init只补齐不存在的root Lockdown/pair_records父目录，不替换现有配对记录。

## 音频

140 实际记录：原生Speaker已选中、控制值5、静音0、Q16增益414（约-44dB），非零PCM峰值42135，DSP WRITE_DONE，两只实际TFA9873均进入AMPS/SWS，hard_mute=0。用户确认仍无声音，上述状态不能作为出声证据。

141 把 native IOAudio2 唯一发布格式设为48kHz双声道S16 packed。映射环大小、ASBD、格式选择和清零长度统一为每帧4字节；读取后在内核显式转换到物理Q31环。DSP与MI2S仍保持32位、3.072MHz、Pong SD1，未猜测24位在32位里的对齐方式。原生0..16音量、对应Q16增益和dB映射同步调整为每步3dB（0仍静音，16仍为单位增益），不增加原厂功放增益，不修改MTP/温控/电池/过流保护。

## 构建与设备结果

内核、静态init和配套预链接镜像已构建。安装保持原有Data/SystemOS，仅更新配套OS。自动重启保持关闭；没有执行镜像哈希、回读比较或性能基准。实际原生USB回复和扬声器出声结果须在启动后独立记录。

实机：全量配套OS写入完成；临时Fastboot Sending 2.354s / Booting 10.180s 均OKAY。Mac自动配置iPhone/05ac:12a8/480Mbps并保留CDC。原生Mux首次收到62078实际回复；启动较早的一次连接超时，稍后的QueryType成功返回 `com.apple.mobile.lockdown`，GetValue实际返回iPhone17,1/iPhone/18.7.10/22H374。这证明原生服务已越过先前停点，不是daemon模拟的回答。未完成配对时ModelNumber/SerialNumber返回GetProhibited；UniqueDeviceID仍为全f占位，USB保持实际板级序列号，不能据此声称Finder配对完成。

音频：本轮真实CoreAudio以音量4启动，S16转换路径消费非零PCM，Q31峰值66496、Q16增益1039、静音0，随后两只实际TFA9873均报告AMPS/SWS和hard_mute=0。未在本机观察到kernel panic；没有可听输出的独立证据，需由实际播放确认。最终手机保持本轮运行，未启用自动重启。

Mac 的 `devicectl list devices` 本轮仍为 No devices found。原生Mux/lockdownd查询已工作，但Finder/Xcode完整配对与发现仍未证实，不能把前述协议进展写成完整原生USB功能已经完成。
