# Pong 板级接口与运行时修补

## 基线与上游

设备仓库已快进到用户更新的 `77edc26b47afd3ee8776ead5941fea1125dc05cd`。
合并保留此前板级修补和用户的 CI、固件归档、存储与启动打包逻辑。
本轮新增运行时服务策略放在独立的 `userland/board_services18.py`，不通过继续
修改用户刚更新的镜像构建脚本来实现。合并前补丁、未跟踪文件及 stash 保留在本机。

## services81 设备证据

- QCA6490 的 PCIe、SID1c01 SMMU、MHI/QMI 已完成；固件登记 102 个通道，随后
  在 station 创建阶段进入 RDDM。最后命令包含 `VDEV_CREATE` 和 `VDEV_SET_PARAM`。
  固件来自连接的 Pong 活动 B 槽 `modem_b/image/qca6490`，不是 Sheng 的无线固件。
- 背光服务创建成功，但 DCS 初始亮度读取失败，尚无实际写入记录。
- 两颗 TFA9873、ADSP/APM 和 PCM DMA 服务已创建，HAL 客户端已连接；没有 StartIO
  或实际出声证据。实体音量事件已到达 SpringBoard，原生音量方法调用后未观察到效果。
- Sileo 已注册在 `/Applications/Sileo.app`，打开后因 Evander 缓存目录创建失败，
  在 `DatabaseManager.swift:39` 触发 SIGTRAP，不能以应用已注册推断可用。
- 8 个 CPU 均已启动并请求最高有效非 boost LUT；GPU 请求 900MHz。一个帧统计窗口
  为 49 帧/7873ms，驱动约 55ms/帧。源码仍对反复使用的约 10MB IOSurface 每帧
  prepare/map 和 Metal 导入。蓝牙服务在没有传输层时反复退出和拉起。

上述日志及 Data 诊断记录位于本机 `out/ipados18/logs-services81`，不进入 Git。

## 修补

### 无线

实际 station 以 Ethernet MSDU/HW_80211_ENCAP 发送，驱动原来却采用默认
NativeWiFi frame mode；在开始注册前明确选择 Ethernet 模式。对 station 创建参数
去掉 generic MBSSID AP 的回退标志，保留真实收发链配置和固件返回值。
继续保留崩溃后的请求终结、停止 dispatcher、隔离未完成 DMA 的处理。
这不是扫描成功或已联网的结论。

### 音频和按键

将默认输出选择放到 audiomxd 内的 PongAudio.driver，通过全局 CoreAudio UID 查找
真实 PongAudioDevice，避免外部 root 程序重复初始化 audio registrar。保留已有有效输出。
共享音频 ABI 升至 2，附加真实 PCM 输出增益和静音值。SpringBoard 音量键新增只允许
调节增益的用户客户端，PCM 转换和 HAL 音量控件读取同一个增益；关闭此控制客户端
不会停止播放。HAL 类型 0 的缓冲区和关闭行为不变。

### 背光

参照 Nothing SM8475 DSI 控制器读路径，单独发送 GET_DISPLAY_BRIGHTNESS，并同时等待
DMA 和 BTA 完成后读取 FIFO；不再将设置最大回包长度的命令与 GET 混发。保留 16 字节
回读上限、Pong NT37705 的 12 位亮度范围和失败返回。没有伪造亮度读取成功。

### Sileo

启动任务创建并交给 mobile 的真实可写目录，包括 `/var/jb/var/mobile/Library` 下的
缓存和数据库。仅在 Sileo 进程内纠正 Foundation 用户目录，并去掉这些路径上不受支持的
文件保护属性，其余属性、文件操作及错误继续由系统处理。沿用调用者受限的 setuid root
helper 执行 APT 命令，不为整个 SpringBoard 或所有应用赋予 root。

### 性能

内核保留最多 8 个已准备的源描述符和用户映射，Metal 侧保留最多 8 个对应导入；仅在
真实 GPU 完成后允许回收。wire ABI 升至 2，使用独立 source_id，不能仅凭可复用虚拟地址
命中缓存。内核与配套 Metal 前端必须一起安装。DMA 状态不明时仍保留页面并隔离通道。
蓝牙任务要求真实 DT 传输节点后再启动；保留程序。移除 launchd 的额外活动诊断选择，
保留内核屏幕日志及所需无线、Sileo、音频日志。

## 构建和实机界限

完整 Darwin24 内核、用户运行时、音频 HAL、Metal 前端和预链接启动镜像均已编译完成。
Metal 使用现有 services64 的桥接与 Turnip，避免用旧组件覆盖已接通的 A730 路径。
本机 OS 通过稀疏卷写入记录准备更新，保留现有 UFS Data/SystemOS。
本轮没有运行测试、性能基准、镜像哈希或镜像回读比较。

本机 OS 已完成 160MiB 更新并写入手机，6 个写入范围均收到完成回执。Data 和
SystemOS 没有写入。安装回执为本机 `out/ipados18/install-services82/installation.json`。
实际扫描、Sileo 打开、声音、亮度和流畅度仍需设备结果。

临时启动已执行：Sending 100112KB 为 OKAY（2.322秒），Booting 为 OKAY（10.172秒）。
未刷写永久启动分区，未在启动后主动重启设备；这仅证明镜像已交接，不证明上述功能可用。
启动命令输出保存在本机 `out/ipados18/boot-services82/fastboot.log`。
