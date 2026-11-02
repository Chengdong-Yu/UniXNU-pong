# services89：原生网络请求的 OOL 内存生命周期修补

services88 REC console 保存在本机 `out/ipados18/logs-services88/pstore`。Wi-Fi 已完成真实扫描（11 个热点），Backlight 原生服务和音频 HAL 均已发布；19:01:38 左右后日志停止，尚未见本轮 panic 正文、原生 Wi-Fi 请求完成记录或 HID 服务打开。Data 的 CrashReporter 中没有该轮新的显示/SpringBoard IPS，不能把旧 services86 崩溃报告当成本轮证据。

源码查到上一轮新增 IO80211 原生通道的确定性错误：22H374 bind 首个 selector0 envelope 为 0x3c30 字节，超过 MIG in-band 上限，XNU `is_io_connect_method` 使用未设置 AutoPrepare 的虚拟 IOMemoryDescriptor 传入。旧代码直接 `readBytes`，该方法经 `getPhysicalSegment` 走 DMA segment walker，未 wired 时会触发 XNU 的 `IOGMD: not wired for the IODMACommand` 不变量；输出 OOL `writeBytes` 同样需要对应生命周期。实际记录未留下 panic 正文，因此这里区分已确认的源码错误和本次停机根因的实机确认。

本轮只在这些 CPU 请求拷贝前配对 prepare/complete：输入 kIODirectionOut，输出 kIODirectionIn，失败返回错误。内存仅归该次请求，完成复制后立即 complete，不映射到无线 DMA arena；输入复制在 station 状态锁外完成，保持后续固件请求异步执行。没有全局取消 IOGMD 检查、假报成功、延长等待、退回/关闭 Wi-Fi，也没有改页大小和 SMMU。

保留 services88 的 IO80211 selector0/1/7、原生 Wi-Fi 设置界面、HID Consumer 报告、删除自制音量/HUD、亮度字节序与可重试策略、标准 HAL 静音对象。配套 OS/runtime-services88 和 Metal plugin-services87 保持不变，Data/SystemOS 不修改。用户维护的构建脚本不改动；按钮、开关和滑块保留。没有运行测试、基准、镜像哈希、镜像审计或镜像回读比较。

## 主机构建

完整 Darwin24 内核及静态驱动编译通过（build-services89.log），沿用 runtime-services88 和 Metal plugin-services87，不改已安装的系统窗口。配套 8 核临时启动镜像随后生成；设备结果另行记录。

## 临时启动

services89 镜像已生成并通过 Fastboot 临时启动：Sending 2.339s、Booting 2.724s 均为 OKAY，实际输出保存在本机 `out/ipados18/fastboot-services89.log`。未重写任何 OS/SystemOS/Data 窗口或永久 boot 分区，设备留在运行状态。是否恢复桌面、原生 Wi-Fi 请求是否完成，需要本轮实机反馈/日志，不能以成功交接代替。
