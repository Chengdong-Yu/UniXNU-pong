# Pong 亮度控制

## 板级输入

- 面板：NT37705 Visionox OLED，1080 × 2412，单 DSI0 命令模式。
- REC 启动参数实际选择 `qcom,mdss_dsi_nt37705_visionox_amoled_120hz_cmd`，phandle `0x5e6`。
- 控制器：SM8475 DSI v2.6，寄存器 `0x0ae94000`、长度 `0x400`。
- 调光：DCS `0x51` 写入、`0x52` 回读；0 表示关闭，非零下限 10，上限 4095。
- `bl-inverted-dbv` 在 Nothing 驱动里交换字节，并不反转亮度。其 MIPI helper 原始顺序为低字节、高字节，交换后 Pong 在线路上使用高字节、低字节。
- 输入记录在 `configs/pong-display-sm8475.json`，ADT `/chosen/pong-dsi-backlight` 为 `<Q6I`，依次为基址、长度、最大值、非零最小值、字节交换、通道、版本。

## 实现

`PongBacklight.cpp` 在独立 IOWorkLoop 上使用控制器的命令 FIFO。每次最多两个 DWORD，完全不需要分配 DMA 内存；保留固件的像素流水线、PLL、显示供电及扫描缓冲。

首次读取真实亮度后才发布 `IODisplayParameters`、`backlight-control`。写入完成后再读面板确认；16.16 秒的 `FadePeriod` 转成最多 5 秒渐变，新请求覆盖待处理目标，不阻塞调用方。硬件完成、数据类型和超时均有界处理。DMA 忙时不重用或复位 FIFO。

CoreBrightness 查找 AppleARMBacklight conformance。板级类只注册唯一的 `PongBacklight` 名称，在编译期实现 `getMetaClass()` 适配所选 Darwin24 接口。真实 AppleARMPlatform 的类初始化可能晚于 framebuffer；亮度工作循环每 100 ms 异步查询，最多等待 30 秒，类就绪后才开始 DCS 事务及服务发布。没有原生类时停用亮度服务并记录原因，不阻塞系统启动。

services61 实际 panic 证明 AppleARMPlatform 随后会注册 `AppleARMBacklight`。已删除之前的同名兼容类，不能用“当前没查到”推断它永远不会注册。没有复制固定长度 vtable 或修改猜测的槽位。服务只允许 `brightness`、`FadePeriod`，backboardd 使用现有的服务级属性授权。

服务启动允许固件保留的 DSI 视频引擎，只要求 DSI_EN 已开启且测试图案未激活。命令事务临时启用 CMD_MODE_EN，完成后恢复原控制位；不复位固件显示流水线。

## 构建接入

- `CONFIG_XNU_PONG_BACKLIGHT=y` 单独控制链接，与 Wi‑Fi 是否启用无关。
- `build-pong-power-native.py` 构建类，并将其静态初始化放到 IOKit 后期注册路径。
- `MiniIOMobileFramebufferStart` 仅在显示启动成功后调用亮度初始化。
- 打包器显式加入 `pong_backlight=1`；设为 0 可暂时保留固件亮度。

## 来源与当前状态

- [UniXNU 官方显示与亮度文档](https://unixnu.com/zh/porting/display)：OS 属性、工作循环、渐变及完成/失败处理。
- [Sheng 亮度服务](https://github.com/UniXNU/UniXNU-sheng/blob/08cde20/package/AppleARMBacklight.cpp)：CoreBrightness 服务发现与 sandbox 接入方式。没有采用其 KTZ8866、双 I²C、SM8550 时钟和引脚参数。
- NothingOSS 版本 `c402fcdb3a1523d10b15ef1123bced45456f7592` 的 [dsi_panel.c](https://github.com/NothingOSS/android_kernel_msm-5.10_nothing_sm8475/blob/c402fcdb3a1523d10b15ef1123bced45456f7592/vendor/qcom/opensource/display-drivers/msm/dsi/dsi_panel.c)、[dsi_ctrl_hw_cmn.c](https://github.com/NothingOSS/android_kernel_msm-5.10_nothing_sm8475/blob/c402fcdb3a1523d10b15ef1123bced45456f7592/vendor/qcom/opensource/display-drivers/msm/dsi/dsi_ctrl_hw_cmn.c)、`dsi_ctrl_reg.h` 和 `drivers/gpu/drm/drm_mipi_dsi.c`：Pong 编码、FIFO、BTA、状态及寄存器事实。本实现为本项目的工作循环传输，没有引入 Linux 运行时依赖。

已完成编译并按用户后续指令临时启动，Fastboot Sending/Booting 均为 OKAY；尚待确认滑块、最低/中间/最高亮度及面板回读。
