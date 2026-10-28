# services70：HAL 路由、物理音量键与 PCIe 恢复

## services69 的已观察事实

REC pstore 929861 字节。Pong 六路 WLAN rail 均物理就绪，分别读到1012/966/1800/1900/1350/2200mV，enable和in-regulation均有效。PCIe出现DetectQuiet与DetectActive（states=3）但没有DLL，不能宣称Wi-Fi已接通。GPIO、GCC、82项PHY表继续使用Pong实际DT。

APM框架、两颗地址34/35的TFA9873（revision b73）、SID1801 DMA和立体声服务均就绪；HAL已加载并连接。没有PCM启动记录。audiomxd运行约51秒后自行SIGKILL并重新启动。物理音量键的down/up到达SpringBoard的increaseVolume/decreaseVolume通知桥，但用户观察到无效。

## 对照UniXNU文档及Sheng后的实际修补

- HAL新增iOS18枚举必需的custom、box、clock-device、stream taps空列表，以及model/serial/firmware/identify/configuration/accs/auto属性。未实现的列表返回正确的空列表，而不是unknown-property错误。
- 按Sheng的AudioServerPlugIn接入，采用iOS能自行创建driver-device端口的Thunderbolt路由类别；真实后端仍是Pong已就绪的TFA9873/MI2S输出。此前BuiltIn类别要求Apple私有port描述，Pong桥未提供。保留仅输出，不伪造输入流。
- 对已就绪物理服务在Initialize中执行非等待的即时连接，确保HAL首次枚举可见；硬件尚未发布时再后台重试。
- 恢复stream IsActive属性setter，并保留真实输出状态和失败传播。
- 通过选定运行时AudioGetHostClockFrequency的ADRP/LDR解析得到常量地址，新增受限user-client方法，检查当前task、共享缓存范围、对齐、物理映射和原始24MHz值后匹配Pong19.2MHz计时器。Darwin24的get_task_pmap存在于Unsupported.exports，但不出现在生成kext头，增加匹配声明；未访问硬编码task结构偏移。
- 时间戳从本次StartIO的帧起点递增，使用Raw clock和真实DSP安全提前量；首次启动等待有界，失败不报告运行。
- 默认关闭直接SpringBoard通知桥，使实体音量键保留原始Consumer HID down/up，上游路径接管长按和路由。直接桥仅显式pong_volume_direct=1时启用，避免默认吞掉HID事件。
- Wi-Fi首次保留Sheng QMP交接流程；第二次失败恢复改为Pong pci-msm完整PHY reset/power/table/PIPE序列，再重新上电QCA6490 PMU。曾经的重试只重置WLAN端点，未重建PHY。两次训练都有界，失败仍返回正常启动。

本轮编译Darwin24、UI/HAL和预链接启动镜像。只更新OS窗口，保留8GiB UFS Data及现有SystemOS。按用户要求跳过镜像哈希、镜像审计、设备镜像回读、基准与额外测试。运行结果须由本轮设备日志及实际播放/按键/扫描确认。

构建结果：format2 Darwin24 内核与PongAudio.driver、libpong-ui18编译完成；实际预链接镜像位于本地boot-services70。内核链接已包含Darwin24 get_task_pmap、pmap_find_pa和phystokv。OS写入进行中，尚未进行本轮运行确认。

交付完成：OS写入6845104128字节并sync，安装记录complete；Data/SystemOS未更新。工作盘恢复挂载后按写入收据从6442450944字节处续传。当前macOS的ADB原生USB后端无法正常启动，ADB_LIBUSB=1连接REC成功。services70通过Fastboot临时启动，Sending 2.318s、Booting 10.138s均OKAY。运行中的音频、音量键和Wi-Fi效果仍待确认；未宣称硬件已可用。
