# services67：桌面启动、HAL 激活与 QCA6490 建链

## REC 日志证据

- services66 的 `console-ramoops-0` 为 976713 字节，系统运行数分钟，没有新的 XNU panic。
- PCIe PHY 状态 `0x86`，PHYSTATUS bit6 已清，PIPE/rchng 时钟未 halted；ELBI=1，DLL 未激活，仍在 Detect。
- SpringBoard PID7 保持运行；GPU设备和scanout worker创建，但无实际首帧，延后的唤屏和音量准备完成日志均未出现。
- audiomxd 每约5秒 SIGSEGV。选择性读取 Data 的 CrashReporter 文件（不复制或比对整盘、禁用元数据校验）取得当前 .ips；崩溃为 `HALS_AHPManager::Activate()+48`，空地址读。
- REC 原生 `pcie0-long` IPC 日志记录 CNSS 请求 `target_link_speed=2`，Linux在约15毫秒后进入L0，以Gen2x1完成链路。相关时钟频率为aux19.2MHz、refgen100MHz，与Pong配置相符。

## 实际修改

### HAL和SpringBoard

已接受的 HAL 插件工厂返回稳定的有效 IUnknown 接口；服务连接移到 Initialize 启动的后台线程。只有真实PongAudio控制页通过检查后，设备列表才出现输出设备并通知HAL。删除工厂把硬件尚未连接当成空插件返回的处理；保留硬件错误传播，没有伪造声音输出。

音量按钮桥接通过运行时类型、offset和类大小检查读取SpringBoard已有 `_volumeControl` ivar，避免定时调用可能同步初始化音频路由的getter。`_OBJC_IVAR_$_SpringBoard._volumeControl` 已存在于22H374符号表。硬件控制器尚未构造时重试，不主动构造它。

### PCIe

训练目标对齐实际CNSS的Gen2，不改芯片支持的最高链路能力。链路超时允许一次有限的端点重启：关LTSSM、保持PERST低、WLAN_EN低150ms，再按实际BT/XO/WLAN时序上电。重试保留已稳定的PHY/REFCLK和共享电源，之后等待100ms再释放PERST。两次失败返回正常系统启动，不伪造链路成功、不访问endpoint BAR。services66的主机MMIO电源前置条件继续保留。

## 交付边界

使用format2构建Darwin24、更新OS的UI/HAL，8 GiB Data不变。跳过镜像哈希、镜像校验、设备镜像回读和性能测试。临时启动只证明Fastboot接受镜像；恢复桌面、音频可用和QCA6490建链仍需本轮设备结果确认。
