# services102：壁纸、电池上下文与网络启动阻塞

## services101 实际证据

自动180秒返回 REC 已完成，console/pmsg 均有本轮 boot ID、同步请求及提交后重启标记。
没有再发生 services100 的缓存类型断言，但 WLAN 128 MiB pageable IOBuffer 仍分配失败。
原生 IOAudio2 客户端打开，不等于 StartIO 或实际出声；系统音频输出仍未确认。

- PosterBoard IPS：`PBFHomeScreenPreviewImageBuilder _imageForView:` 构造 CARenderer 选项时包含 nil；
  同轮多次约10 MiB IOSurface 分配失败、CAMetalLayer 无 drawable，仅约33 MiB surface 在用。
  不以此认定整机物理 RAM 耗尽。
- Preferences Battery 页 watchdog：设置同步等待 PowerUIAgent；后者等待 contextstored；
  contextstored 主线程在 `_DKMotionMonitor` 初始化时同步等待 CoreMotion activity capability。
  该板尚无运动活动硬件 provider，不应因 iPhone 外观标识而进入不存在的运动服务。
- configd 主线程卡在 IPConfiguration 的 gethostuuid。原生 ARM UUID 路径缺少 Apple ECID/chip-id，
  不发布 IOPlatformUUID 会让 PreferencesMonitor 和网络配置启动等待。

## 具体修补

1. ARM64 IOKit pageable allocator 单对象上限96 MiB，不能容纳原128 MiB WLAN arena。
   改为普通 wired IOBuffer，逐4 KiB核对物理页，清理缓存后仅对自己持有的页设置并确认 WC。
   保持128 MiB IOVA、SID1c01和实际 Pong modem_b 固件，不缩小或伪造映射。
2. MiniIOSurface 普通 shared 分配失败时使用 VM-backed shared descriptor，成功 prepare 后才交出真实内存。
   保留尺寸、清零、映射、所有权及 GPU fence；未取消缓存断言或返回假 drawable。
3. contextstored 专用22H374适配仅将 `CMMotionActivityManager isActivityAvailable` 返回原生 NO，
   让无硬件的 motion monitor 保持 inactive。保留其余 context、PowerUI 和电池页服务；不停用控件。
4. ArmDTPlatform 接受显式配置的稳定软件平台 UUID 并发布原生资源。每机 UUID 来自 ignored local.toml，
   由本机 SoC serial 派生；不在公共源码放入设备唯一值，也不伪造 Apple ECID。
   板级 peripheral boot wrapper 传参，用户基础镜像和 userdata CI 脚本未修改。

内核和配套运行时编译完成，OS 差量更新保留 Data/SystemOS。继续使用180秒自动 REC 抓取。
设备效果以本轮新日志为准，编译和 Fastboot 成功均不作为 Wi-Fi、音频或界面可用证明。
