# services142：唤醒、原生 USB 连接状态与音量

141 REC显示熄屏/唤醒后后台服务、按键和WLAN扫描仍运行，无kernel panic。显示在scanout未提交（kNoResource）及后续IOSurface等待处停止更新，不能描述为所有CPU停止。

显示侧保留有界的三组有效scanout导入：Pong熄屏只清空现有framebuffer，没有GPU复位或撤销映射，不再每次空帧销毁再导入。真正未提交失败仍按原有路径清退导入；提交后未确认完成的DMA仍隔离并保留页。Metal完成回调先释放真实IOSurface写者状态，再把可选缓存淘汰交给单个后台任务，防止资源析构挡住显示正在等待的完成回调。无拷贝内存耗尽时仅淘汰可选缓存后重试一次。

USB原生日志证实：Mac已发起Pair，但lockdownd因为其AppleUSBDeviceMux监听未收到实际Pong事件而返回“USB or proxy host no longer connected”；UniqueChipID缺失、UniqueDeviceID为全f占位也阻止电脑完整发现。

新独立 `libpong-usb18.dylib` 仅为lockdownd把原生Mux/接口匹配转换成真实PongUsbMuxTransport；原生PTP属性过滤仍生效。内核原服务发布真实USBDeviceFunction/IsActive及配置、断开interest事件，不伪造Apple驱动类。独立身份数据来自REC实际Qualcomm serial2380287147和USB serial dd72e429，搭配本机已有platform UUID形成稳定软件UID；不向AppleImage4注入Apple ECID/chip-id。usb-mux与lockdownd读同一身份，避免描述符和查询不一致；配对、用户信任和TLS仍为原生处理，不自动接受信任或生成假证书。

音频保留原生S16到Q31物理通路。根据141的控制值4/2与极低非零峰值，把重复的指数衰减改为0..16线性幅度（0静音，16单位增益）；以16段真实dB元数据对应实际增益。原厂TFA9873增益、MTP、温控与过流保护保持不变。仅软件配置进展不能证实可听输出。

本轮产物、日志、临时打包脚本全部移至PongDev。清理此前生成的boot、runtime、logs、install、delta及Metal构建目录，保留原始源码、固件输入、当前构建和必需的编译缓存。141安装记录被清理后，依据此前实际成功的全量安装和os-storage记录保留了一份明确注明来源的最小世代记录；没有重新读取或比较设备镜像。

内核/静态init/独立USB适配/Metal组件和预链接镜像已编译。只更新配套OS，保留Data、已安装Vulkan后端、着色器缓存和软件包；自动重启保持关闭。实机结果待启动后追加。

实机追加：Mac 原生 MobileDevice 已完成 `AMDeviceConnect=0`、`AMDeviceValidatePairing=0`、`AMDeviceStartSession=0`，用户确认 Finder 识别成功。USB 仍为 05ac:12a8、480Mbps、配置1，实际 CDC 日志和原生锁定服务均可读。满电快照为外部供电已连接、FullyCharged=true、IsCharging=false；并非无供电。音频已见非零 PCM、真实 DSP WRITE_DONE 和功放状态，仍不能据此确认可听输出。熄屏唤醒效果尚无用户确认。
