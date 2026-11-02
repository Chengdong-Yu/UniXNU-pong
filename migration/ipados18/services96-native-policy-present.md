# services96：系统外设策略和显示搬运

## services95 实机反馈及日志

用户确认 Wi-Fi 开关和音量滑块仍不可用。进一步确认所谓动画快是滑动/画面跳帧，动画速度和系统时间正常。日志显示新增的精确 HID personality 已即时匹配内核 IOHIDEventDriver，DriverKit 的 120 秒等待消失；两个物理音量键报告均成功，但音频没有 StartIO。

QCA6490 继续注册 en0 并扫描 19 个热点，系统依然只有电源 GET，没有设置发起的 SET。不能把驱动扫描算作系统开关或关联已接通。本轮保留 Pong 的固件、电气、SMMU 和真实 station，不重新替换成 Sheng 的固件。

## 原生音频输出选择

选定 22H374 的 AudioObject API 操作调用进程内的 HALS_System。独立 audio-route 程序出现了尝试注册 audiomxd 所有端口的记录；它选择设备 35 并不能证明实际音频宿主的默认输出已改变。

把同一组实际 UID 翻译、存活输出校验和 dOut/sOut 选择移入 ASPL 的发现工作线程，位于 Initialize 和 PropertiesChanged 返回之后，且不持有驱动锁。避免从任何 ASPL 属性/IO 回调重入客户端 API。原独立 helper 的自动作业停用，文件保留。仍使用实际 PongAudioDevice 和标准音量/静音对象，不直接处理按键增益、不绘制自制音量条、不发布假输入端口或假 StartIO。

## Wi-Fi 管理器初始化

新增有限时长的一次性 MobileWiFi 初始化作业，使用选定系统的原生 manager/create/schedule/copyDevices/setPower ABI。只有 native manager 已发现真实无线设备时才请求 wifid 开启电源；保留飞行模式，记录真实返回的 AllowEnable。请求标记只表示已派发，不能表示联网。进程超时只终止自身，不阻塞启动，不修改 Preferences 控件、伪造网络或替代系统服务。调用者仅携带此路径需要的实际 Wi-Fi 管理 entitlement，没有关闭全局 CoreWiFi 权限校验。

## 跳帧

services95 记录最终显示调用约 28–57 ms/帧，许多交换晚于刷新。用户确认问题为跳帧，因此不改变时基、动画时长或伪造 FPS。保留 Metal/Vulkan 合成和渲染，只将最终 GOP 屏幕搬运交回现有 fb_threads=4 的 CPU 路径，减少每帧一个额外 blit 提交、源导入维护与等待。GPU scanout 源码和退休保护保持，可在适配后再次启用。120 Hz 面板模式与实际生成帧率仍分别记录。

## 构建与结果

用户维护的既有基础构建脚本未改；本轮只更新本项目新增的外设后置策略工具、音频源码与原生网络初始化源码。原生 Darwin24 内核主体复用已完成的构建，重新生成 services96 预链接镜像，配套 runtime 与 Wi-Fi 初始化程序编译通过。未运行测试、基准、镜像哈希或镜像回读比较；设备效果在临时启动后另记。

80 MiB OS 差量安装完成，Data/SystemOS 保留。临时启动 Sending 2.332s、Booting 7.525s 均 OKAY；自动 REC 抓取的 console/pmsg 包含本轮 ID、同步和 reset commit。真实 audiomxd 内 dOut/sOut 读回设备35，但没有 StartIO，用户确认系统音量滑块仍不可用。初始化程序未从 MobileWiFi 发现设备，未派发电源请求。不能将内核扫描到热点等同系统开关可用。

显示调用约 0.7–1.7 ms，较上一轮 28–57 ms 明显减少；用户确认卡顿减少，同时出现画面更新闪屏。该结果只支持最终拷贝耗时下降，不代表完整呈现正确或达到120帧。下一轮修复真实 GPU/显示所有权及 swap 退休，并纠正直接物理 dOut 对原生 VAD 路由的绕过。
