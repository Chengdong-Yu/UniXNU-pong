# services68：22H374 HAL 加载格式修复

## 日志与原生代码证据

REC console 保存于 `audit-services67/console-ramoops-0.txt`，913147 字节，无新的 XNU panic。选择性读取当前 Data 的 CrashReporter 日志，audiomxd 仍每约5秒因空指针读取退出，主线程为 `HALS_AHPManager::Activate()+48`。22H374 CoreAudio 的反汇编显示该位置读取 manager+0x178 保存的驱动指针后解引用，指针为空。services67 的工厂改动没有消除它。

22H374 `HALS_PlugInManager::LoadPlugIns` 在未滑移地址 `0x1b7578358` 比较目录项扩展名；引用的字符串 `0x1b78811e6` 为 `.driver`。现代 AudioServerPlugIn 工厂路径使用 `443ABAB8-E7B3-491A-B985-BEB9187030DB` 类型。原生 VirtualAudio.plugin 使用另一套 legacy AHP 类型，不能混用。Pong 之前把现代 ASPL 编译成 dylib、安装为 `.plugin`，与现代枚举路径不一致。Sheng 的官方构建脚本使用 `.driver` 和 `-bundle`。

## 修改

- `compile_audio` 生成 `PongAudio.driver`、MH_BUNDLE、隐藏默认符号并显式导出工厂，保留 iOS18 SDK、原生 ASPL 类型和 IOKit user-client 许可。
- 两个更新入口均移除 HAL 视图里的旧 `PongAudio.plugin`，再安装现代 driver；保留原生 VirtualAudio 和音频服务定义。
- 工厂与 Initialize 只记录各自加载入口，连接仍在后台进行，真实硬件就绪之前不发布音频设备。
- 修正 services67 对 SpringBoard getter 的推断：22H374 的 `volumeControl` 是单纯 ivar load，没有证据证明 getter 本身等待音频。继续读取已有控制器，不主动创建对象。

## Wi-Fi 审计结论

按 UniXNU 中文 network/power 文档，重新核对 live Pong DT、NothingOSS pci-msm/CNSS、GCC 和 pinctrl。GPIO80/94/95/96/204、82项PHY表、19.2MHz aux、100MHz refgen、Gen2x1目标和有界失败返回保持当前 Pong 配置。REC 原生 Linux 能在约15ms完成训练；XNU 两次训练仍停 Detect Quiet，PHY状态0x86但DLL未激活。因此不能把设备发布或PHY就绪写成Wi-Fi已可用；本轮未凭猜测改动额外的PMIC、PHY或SoC寄存器。

## 构建和交付

重编译用户态 helper/UI/HAL，使用 services67 已编译的同一 Darwin24 内核重新预链接并打包，启动参数标记 services68。只更新 OS 窗口，保留 8 GiB UFS Data、8核、屏幕日志和现有硬件配置。不运行镜像哈希、回读比较、测试或基准。临时启动成功仅表示 fastboot 接受镜像；桌面、音频和Wi-Fi结果仍需手机反馈。
