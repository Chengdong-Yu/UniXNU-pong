# 22H374 桌面启动与根卷匹配修复

## 设备证据

本机 services84 REC 记录已保存 console/pmsg，并从实际 Data 的 CrashReporter 读取 backboardd IPS。
屏幕末尾等待 `IOPropertyMatch = { EmbeddedDeviceTypeRoot = 1; }`。相同等待在 services83 也存在，
不能单凭末行把它判定为此次无界面的直接原因。

此次 backboardd 多次 SIGSEGV，崩溃线程均为 `com.apple.CoreBrightness.CBDisplayContaineriOS`：

- `_DisplaySliderToLogicalBrightnessInternal + 272` 读取零地址。
- 调用路径为 `DisplayCreateWithCapabilitiesAndID` → `DisplayCreateWithID` →
  `CBDisplayModuleiOS initWithBacklight` → `CBDisplayContaineriOS findBacklight`。
- 面板已真实回读 `level=32 max=4095` 并发布背光；此后才进入此前没有执行的背光初始化路径。
- GPU 设备和 scanout worker 都已创建；没有呈现前 backboardd 就退出，不把该退出推断为 GPU fence 卡死。

## 修补

### 真实根卷匹配

在已有 UFS OS UUID 解析路径中，确认唯一 IOMedia、BSD 设备号以及 Pong OS 窗口祖先后，
给该真实根卷发布标准 `EmbeddedDeviceTypeRoot` 属性。失败或歧义不发布，不创建占位磁盘，
不把通用平台节点当成根存储。消费者仍取得真实根卷的 BSD 名称和属性。

### CoreBrightness 初始化顺序

只对匹配的 22H374 CoreBrightness 和 backboardd、且真实 NT37705 背光服务已发布的情况适配。
该构建在缺少 Apple 面板校准时，先调用 slider→logical 来计算默认极值，之后才分配自己的默认曲线。
换算函数虽检查了空曲线，随后仍无条件读取首项，造成所有已捕获的零地址崩溃。

适配器在该初始化窗口读取并使用同一运行时内置的软件默认曲线进行插值；曲线一旦存在，
调用原始函数。没有伪造 Pong 面板校准、电流或光度测量，也没有关闭物理亮度控制。
保留所选映像身份、入口指令和可执行范围限定，不套用 iOS16 地址，不捕获或吞掉其他异常。
此改动通过现有本进程代码页适配机制安装，不修改 Apple 恢复二进制。

## 主机与设备结果

完整 Darwin24 内核、运行时/HAL 和启动镜像分别构建，保留 services84 的无线、按键、
充电与 GPU 更改。用户维护的构建脚本未修改。Data 不重置，OS 运行时按写入计划更新。
没有运行测试、性能基准、镜像哈希或镜像回读比较。

安装和临时启动结果在完成后追加；构建通过不等于已恢复界面。

完整内核与配套运行时编译通过，8 核启动镜像已打包。日志为本机 `out/ipados18/build-services85.log`、`build-runtime-services85.log` 和 `package-services85.log`。OS 更新计划为 96MiB，保留 Data/SystemOS 与现有 Metal 运行时。

## 安装和临时启动

96MiB OS 更新收到设备写入完成回执，待更新记录已清除，Data/SystemOS 保留。services85 临时启动 Sending 100112KB 为 OKAY（2.322s），Booting 为 OKAY（10.127s）；未写永久启动分区，启动后保留运行状态。设备是否进入桌面、标准根卷匹配是否完成和亮度调节效果仍需实机观察，不能以成功交接代替这些结论。
