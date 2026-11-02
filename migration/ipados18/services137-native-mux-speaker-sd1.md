# services137：原生 Mux 帧方向与 Pong 扬声器 SD1

## services136 实机结果

临时启动 Sending 2.334s / Booting 10.130s OKAY。macOS 枚举 Pong iOS USB + console、CDC `/dev/cu.usbmodemPONG_SM84752`，真实 usbmuxd 独占 ff/fe/02 interface。取得 454884 字节实时 console。Mux 已完成 v2 VERSION 协商，但随后 parser 重置，原生 host 的 lockdownd preflight 超时；因此不能声称原生连接或 Finder 已成功。

CDC `help` 已返回命令列表。随后实际发送 `reboot rec`，日志记录 accepted、usb-recovery-sync；手机实际以同一 serial 回到 ADB REC，取得 1254275 字节 pstore。没有启用自动重启。Fastboot 命令使用相同同步关闭流程和不同真实 PMIC/IMEM 目标，结果另记。

## 原生 USB

device→host 的 v2 magic 改为 FACEFACE，host→device 保留 FEEDFACE，并兼容已知双方向 framing；不再把 host 实现直接当成 device 实现。参考公开 [demuxusb 定义](https://github.com/hack-different/demuxusb/blob/master/ext/usbmux.h) 和 [逐帧解析](https://github.com/hack-different/demuxusb/blob/master/src/protocol/USBMUXInterfaceExpert.cpp)。VERSION 始终采用 8 字节 v1 header，包括 host 重开会话；逐帧识别实际 v1/v2 framing，兼容过渡 SETUP/TCP 和本机固定 20 字节 TCP header；保留所有长度/sequence/window 检查。保持有界帧长、真实 socket、sequence/window 检查和 iOS 自身配对。不会忽略不认识的数据或返回假成功。

原生 ACK|PSH 数据保持正常 TCP 字节语义。对不认识的 magic/protocol/SETUP 仅记录头部元数据，不转储 TLS/配对内容。既有 CDC 和实际 DWC3 DMA 隔离保持不变。

## 音频实际板级错误

本轮重新读取 Pong REC 的 live DT：

```dts
qcom,msm-dai-q6-mi2s-tert {
    qcom,msm-dai-q6-mi2s-dev-id = <2>;
    qcom,msm-mi2s-rx-lines = <2>;
    qcom,msm-mi2s-tx-lines = <1>;
};
```

该属性是数据线位图，Qualcomm `msm_dai_q6_mi2s_get_lineconfig` 按置位选择 SD 线。Pong 播放是 SD1（GPIO124），SD0（GPIO122）为反馈。之前的 AudioReach 配置却将 stereo playback 放到 I2S_SD0=1，时钟/WRITE_DONE 成功也不能把数据送到功放的输入线上。

本轮改为真实 I2S_SD1=2，保留 tertiary index2、48k stereo S32_LE、PRM 0x104/3.072MHz、本机 TFA9873 CNT、保护和 IOAudio2 音量接口。没有发布采集端点，没有提高功率、写 MTP 或关闭保护。来源是本机 DT 和 [Qualcomm 驱动](https://android.googlesource.com/kernel/msm-extra/+/48696dd1a1e9bf42b32045253819137717020aa2/asoc/msm-dai-q6-v2.c)，不是 Sheng 的板级取值。

## 安装

保留 Data/SystemOS、现有 codecs/HAL/GPU/WLAN/亮度/HID 及本轮捕获的实际 rootful 包状态。只替换 USB bridge 并同步 canonical/cache job。经历过 guest 启动后使用完整一致 OS 写入，不用旧本机 APFS 元数据做局部覆盖。没有镜像哈希或设备镜像读回校验。声音和原生服务结果以本轮设备响应为准。
