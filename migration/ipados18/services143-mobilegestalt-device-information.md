# Pong 22H374 MobileGestalt 设备信息与供电能力

本轮保留 services142 内核、GPU 唤醒修正和已完成配对的原生 USB 路径，仅更新独立的 MobileGestalt 适配库和 init 的进程注入列表。

## 实际问题

- Mac MobileDevice 在 services142 中完成 Connect、ValidatePairing、StartSession；Finder 已由用户确认识别成功。
- 爱思所查询的 ModelNumber、RegionInfo 和播放/供电能力缺失，HardwareModel 仍为原始 iPad 的 J171AP。
- 原生 lockdownd 的 `com.apple.disk_usage` 通过 `MGCopyAnswer("DiskUsage")` 返回 Data 容量；旧答案含负可用容量，客户端再按无符号整数解释，显示异常 TB 数值。只覆盖单个容量问题无法接通此字典路径。
- 真实 IOPMPowerSource 已返回容量、设计容量、循环次数、外部供电和充满标志。满电时 `IsCharging=false` 与 `FullyCharged=true`、`ExternalConnected=true` 可以同时成立。

## 实现

- 识别明文问题名和其原生 MobileGestalt 混淆键。
- 覆盖 CopyAnswer、Bool、SInt32、SInt64 和批量答案入口，未适配的问题保留原生实现。
- USB 服务的型号展示为用户指定的 iPhone 16 Pro、MYMC3 + LL/A、D93AP/A3083；板级序列号继续使用实际 Pong，UID 和已有配对保持一致。此展示限定在 lockdownd/usb-mux，不将 Qualcomm SoC 全局伪装成 Apple 芯片。
- 供电答案每次读取真实 IOPMPowerSource；不强制声称充电或伪造电池数值。
- AudioPlaybackCapability 只在实际 IOAudio2Device 的 Speaker 输出流已注册时提供，不发布未实现的麦克风或校准值。
- 原生 DiskUsage 字典保留其他策略字段，只用真实 `/private/var` statfs 的 64 位数值修正 Data 总容量与可用容量。
- 将同一库接入 audiomxd、SpringBoard、backboardd、PowerUIAgent、Preferences 和原生 MobileGestaltHelper；保留 UI、编解码器及可选 ElleKit 注入规则。

## 验证范围

编译与实际设备结果分开记录。扬声器出声、熄屏唤醒和爱思界面结果不由构建成功推断。无镜像哈希、镜像回读比较或性能基准。
