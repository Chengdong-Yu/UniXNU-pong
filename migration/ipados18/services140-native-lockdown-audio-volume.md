# services140：原生USB服务身份与扬声器格式

services139 实机结果：Mac 自动完成 Apple Inc. / iPhone / 05ac:12a8 配置，480Mbps，保留 CDC；不再需要主机 SetConfiguration 工具。主机扩展功能3不受支持时正常退回实际配置1。USB读取日志可用；CDC reboot fastboot 实际返回 Fastboot，reboot rec 已于136/138验证。自动重启保持关闭。

原生 Mux 接受查询并将339/68/348字节等真实请求送入62078，但没有收到原生回复。完整139 REC表明 lockdownd 以缺少显式UserName的plist导入user/501域。新 usb_services18.py 为供给的22H374原生daemon明确root/wheel，保留原有监听socket、配对/TLS与事件处理，移除过时ServiceIPC键，同时更新canonical和launchd cache。未伪造AppleUSBDeviceMux/设备控制器类。

音频139日志证实真实Speaker被选中，原生IO开始，非零PCM提交、DSP WRITE_DONE与两只TFA9873 AMPS/SWS出现。用户仍报告无声音，不能把上述状态当作出声。

140修正共享内存端点 media_format.payload_size：只计PCM格式体，不混入16字节外层参数头（立体声20字节）；外层参数仍独立按8字节补齐。功放播放时明确解除B0字段0x5810 PWM hard_mute，不修改原厂增益、OCP/温度/电池保护或MTP。增加原生控制值、应用增益、静音、PCM峰值及实际功放增益/静音读数。48kHz双声道Q31、Pong SD1、3.072MHz、专属DMA和SID1801保持不变。

核对来源：NothingOSS android_kernel_msm-5.10_nothing_sm8475，c402fcdb3a1523d10b15ef1123bced45456f7592 中 TFA9873 B0 字段、tfa_init.c 与 tfa_dsp.c；AudioReach media_format 12字节头及PCM体结构。未套用TFA9874/CS35L43的寄存器。

host：140内核和配套预链接镜像已构建；设备本轮结果待启动后追加。没有运行镜像哈希、回读比较或性能基准。Data/SystemOS及现有功能保留。

实机补充：140保留自动iPhone枚举与CDC，但QueryType仍超时；仅修正root身份没有解除原生启动停点。Data原生日志最后为-UseRaptorCerts，后续可选FairPlay/CESM硬件初始化待适配。实际音量5、Q16增益414、PCM峰值42135；两只功放hard_mute均为0，格式命令已被DSP接受。仍未证实可听声音。
