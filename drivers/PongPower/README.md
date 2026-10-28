# Pong DSP、音频与电源（Darwin 24 / 22H374）

## 已接入的代码

- `PongAdsp.cpp`：手机自身 signed ADSP 固件、PAS1、40 MiB 预留内存、SMP2P、GLINK、ready/fatal 通知和有界等待。
- 实际 SMEM `80900000/200000`、TCSR mutex3 `1f43000`、IPCC `ed18000` / INTID261、AOSS `c300000`。不使用 Sheng 的 SM8550 IPCC、固件或 PAS36 DTB。
- 通过 `gPhysBase/gPhysSize` 检查 DarwinBoot 实际 RAM 所有权；若覆盖 ADSP carveout，停止加载，不覆盖 XNU RAM。
- 电源请求与 WLAN 共用 APSS active TCS4 的 IOLock；GPU 的 active TCS3 保持独立。只保留启动 minima，不在 handover 时降掉共享电源投票。
- `PongBattery.cpp` / `ShengBattmgr.c`：沿用 UniXNU 协议 API，以 NothingOSS `qti_battery_charger.c` 的单电池属性编号解码；不访问 Xiaomi BOARD_GET 扩展、不写充电限值。
- `PongAudio.cpp`：`adsp_apps`、APM speaker/playback graph、TFA9873 配置、独立 SID1801 DMA 和输出环；只有真实初始化完成才发布 `PongAudio`。尚无真机播放成功记录。

## 板级音频约束

Pong 是 TFA98xx 双功放（I2C `a94000`，地址 `34/35`）及 WCD938x，不能加载 Sheng CS35L43 功放配置。实际 `pinctrl-0` 是 qupv3_se13_i2c_active，GPIO48/49、qup13 mux1、pull-up / 2mA；GPIO52/53 不属于这条总线。复位为 GPIO1/89；本机 DT 未设置 `reset-polarity`，Nothing 驱动默认高电平有效，必须 HIGH 10ms 后 LOW 10ms 释放。此前反向序列将功放一直保持在复位状态。PCM 为 tertiary MI2S。只应用本机容器中的易失性 speaker 配置，不运行校准或写工厂 MTP。仍需真机确认 LPASS 时钟、DMA 周期完成及匹配 Darwin24 的 HAL。CPU 调频/系统睡眠回调尚未接入；保留当前固件策略。

## 构建与输入

`scripts/build-pong-power-native.py` 产生 `libPongPower.a`，由主构建脚本 force-load，`ArmDTPlatform` 在 CPU nubs 已发布后调用。电池 metaclass 延后注册，避免 prelink 静态初始化顺序依赖。

参数：`pong_adsp=2`；PCM 调试显式传入 `pong_audio=2`，默认不启用。HAL 默认保留在 `/pong/staged-audio18`，`refresh_runtime --enable-audio-hal` 才将其放入可枚举目录；即使启用，工厂也仅在真实内核设备就绪时创建实例。但 services44 的真实记录说明该检查仍未避免 audiomxd 的 SIGSEGV 循环，当前必须继续默认隔离，尚不能声称 HAL 已与 22H374 正常协作。内核仅提供 control/output 两个内存类型和输出 start/stop，不创建占位 capture 环。本机 `adsp.mbn` 通过 `refresh_runtime --power-firmware DIR` 放入 `/pong/firmware/adsp.mbn`；固件不放入 Git。

参考：UniXNU 官方 `docs/zh/porting/power.md`；UniXNU-sheng `0e1f6e5` 的 Qualcomm 协议/服务结构（保留原 BSD-2-Clause-Patent 声明）；NothingOSS `android_kernel_msm-5.10_nothing_sm8475` `sm8475/t` 的 PAS 与电池驱动。

构建成功只说明接口完成链接；需下一轮 REC 日志确认 PAS、通道响应及真实电池服务发布，不能据此声称已经出声或硬件已可用。

services45 已实际读出 0x34/0x35 两个功放 revision=0xb73，并完成
SID1801 的 DMA 映射，尚未完成 graph。services46 合并官方 power.md
所引用的 UniXNU-sheng 552b674 的 GLINK 大 intent 保留修复；已占满的
小空闲 intent 不再挤掉 graph 命令申请的大 intent。同时内存映射命令
等待真实 MEM_MAP_DONE/handle，不能把普通 ACK 当成映射完成。HAL 继续
在 staged 目录，未据此宣称系统播放已接通。
