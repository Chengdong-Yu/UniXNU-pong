# iPadOS18 外设接口与显示提交修补

依据 services83 的 REC console/pmsg 和本机 NothingOSS 驱动源码调整，保留 Pong 固件、板级地址和用户更新的构建脚本。

## 实机证据

- QCA6490 已通过 PCIe、SID1c01、MHI/QMI 和 WMI station 初始化，扫描得到 13 个热点；此前创建 station 的 RDDM 未再出现。系统只发送早期枚举请求，没有发起完整扫描，不能据此声称设置列表或联网已正常。
- DSI 回包为 `22200037`：DCS 两字节响应的值为 little-endian `20 00`。旧代码把 SET 使用的 inverted-DBV 字节交换也用于 GET，误判为 8192 并停止服务。
- 内核已捕获音量键按下、释放；SpringBoard 音量桥已写入实体 PCM 共享增益。未观察到 HAL StartIO 或实际发声。
- 电池为真实满电读数。REC 中原厂驱动报告 USB_CDP、外接电源在线、输入限值和最大电流均 1500mA。
- Framebuffer 日志只有 7–14fps，在驱动内平均约 40–52ms/帧；刷新模式配置为 120Hz。配置的刷新率不等于应用实测帧率。

## 修补

- 亮度：GET 的 little-endian 解码与 SET 的板级交换规则分开，继续保留写后真实回读、超时和失败处理。
- 无线：补齐物理站点 role/无虚拟 parent/WoW 未启用的枚举回答，保留 22H374 请求的 19 字节能力位图长度，扩展位按实际未支持填零；`SIOCGIFMEDIA` 在用户提供容量时复制真实支持的媒体项；接口节点同时发布实际 MAC。不宣告未实现的 P2P、AWDL 或唤醒能力。
- 按键：边沿后的去抖确认改为 8ms；无中断时使用 16ms 读取，避免旧 64ms 二次确认遗漏短按。保持真实电源和音量输入映射及失败释放。
- 音频：移除在 AudioServerPlugIn 内重入 CoreAudio 客户端 HAL 的路由线程；该调用违反所选 SDK 接口约定。保留真实 DSP 端点和初始化后的设备发布通知，补齐 bundle ID 和按 class qualifier 过滤 OwnedObjects。
- 充电：使用 Nothing 的 USB_ADAP_TYPE 属性 7 解释 USB 类型，适配器电流使用源的最大电流/输入限值，瞬时电流继续作为实时读数，不用于估计充电器规格。不修改固件充电控制，不把满电伪报为正在充电。
- 显示：缓存已固定缓冲上的线性纹理/视图，避免每帧创建销毁后端图像；scanout 用户侧只保留三帧导入，减少旧缓冲参与非一致性 DMA 同步的范围。保留所有权、缓存维护和真实 GPU fence 完成检查；未移除应用 GPU 渲染。

主机编译、设备交接与实际功能分别记录。未运行测试、基准、镜像哈希或镜像回读比较。新一轮滑块、Wi-Fi 列表、声音、充电图标和帧率效果仍需设备反馈。

## 主机构建

完整 Darwin24 内核与 Pong 原生库、运行时/HAL、Metal discovery bundle 和 8 核启动镜像均已编译完成。最新编译包含去抖与 19 字节无线能力修补。构建日志在本机 `out/ipados18/build-services84-final.log`、`build-runtime-services84.log`、`build-metal-services84-final.log` 和 `package-services84.log`。OS 更新计划为 18 个 band、合计 144MiB；Data/SystemOS 不重写。设备安装与启动结果随后追加。

## 设备交接

144MiB OS 写入范围均收到完成回执，安装记录为本机 `out/ipados18/install-services84/installation.json`，待更新状态已清除。临时启动 services84 完成：Sending 100112KB 为 OKAY（2.316s），Booting 为 OKAY（10.172s）。未写永久启动分区，设备保持运行。新外设与流畅度结果仍待用户观察，不将成功交接表述为功能成功。
