# services134：原生扬声器接口与 Pong USB QUP5

## services133 实机依据

REC console/pmsg 共 1,062,793 字节。实际 TFA9873 地址 34/35、revision b73；APM 就绪、原生 IOAudio2 Speaker 打开、MI2S PRM 3.072 MHz 请求成功。CoreAudio 原生环中的非零 committed PCM 已被消费，DSP 返回 WRITE_DONE。此证据确认系统识别和数字传输，不证明扬声器已有可听输出。没有发现 audiomxd 退出或 kernel panic。蓝牙服务缺失的重复请求不能代替内置扬声器故障定位。

USB 停在 `repeater QUP clock did not run`，尚未进入 NXP/eUSB2/DWC3 初始化。REC 的实际 GCC QUP0 slave AHB CBCR 为 `0x80000002`，属于 HWCG 自动门控的合法空闲状态；旧代码要求 CLK_OFF 清零而错误拒绝。REC QUP5 时钟源为 XO/19.2 MHz。Pong DT 的 QUP5 引脚是 GPIO206/207、mux1、pull-up/2mA；NXP@4f 的实际 revision a2，ID 22:32:11，PM8350c GPIO7 已拉高释放 reset。

## 本轮修补

- IOAudio2 selector6 实际保存输出流 active 状态；inactive 时清零该流，不消费或回退到其他所有者。重新启动清空上一会话的客户端输入/输出提交位置，保留独立 PCM/control 锁和锁外 DSP 启动等待。
- 依据 Nothing 的 tfa9873_specific / tfa98xx_key2，为易失的 optimum/profile 配置补齐 KEY2 解锁和重锁。TDM sink、格式、PWDN、MANSCONF、AMPE 请求读回实际位值；没有 MTP copy、OTP 或校准写入。
- 功放状态增加控制、输入、TDM、sink 寄存器读数。除启动初始状态外，在首次真实应用非零 PCM 提交至少100ms后读一次，避免把空缓冲阶段当作可听输出结论。
- 配套 ASPL 在 audiomxd 自己的 HAL 上查询实际 Speaker UID、alive output、流格式和默认路由；只有 HAL 明确返回空默认输出或 Null_Device 时尝试现有原生 VirtualAudio 路由。不启动曾引起独立 HALS_System 冲突的外部默认设备 helper，不发布额外 PCM 设备或虚构麦克风。
- QUP5 有界时钟检查允许真实 HWCG idle 状态；检查 APSS vote，恢复实际 Pong 引脚，依据硬件 DFS 表选择 XO/19.2MHz 后配置 GENI I2C。继续要求真实 NXP 响应和 DT 参数读回成功，再做 eUSB2/DWC3 工作。

USB 仍是正在接通的实际 CDC console 后端。IOUSBDeviceFamily、AppleUSBDeviceMux 和 Finder 配对尚未实现，不能通过空 IOKit 服务声称已支持。

## 构建与部署

Darwin24 内核和匹配的 PongAudio.driver 已编译。保留 services133 的 ABI1 Metal、scanout、原生 HID 和亮度路径；用户构建脚本本轮未修改。Guest dpkg 的203项包版本/架构/配置状态与上一保留代际相同，保留最新 status 和现有包 payload。只替换 OS 的音频 HAL，Data/SystemOS 不重置。OS 使用完整一致代际传输；没有镜像哈希、镜像读回比较或性能基准。自动重启关闭。安装和本轮硬件结果另行记录，编译成功不代表可听输出或 USB 枚举成功。

实际安装回执 `/Users/desktop/Downloads/Pong-install-services134/installation.json` 为 complete，OS 的9,663,676,416字节写入完成，os-storage pending已清空。镜像 `/Users/desktop/Downloads/Pong-build-services134-final/pong-xnu-initial.img` 已通过 Fastboot 临时启动：Sending 2.320s、Booting 5.023s均OKAY，总计7.563s。没有刷写永久boot分区。设备保持运行，尚无本轮可听输出或GUI确认；启动命令成功不等于音频/USB功能成功。

启动后的两次主机 IOUSB 快照未发现 Pong CDC，`/dev/cu.usbmodem*` 为空。尚不能读取新的实时 USB console，不能将本轮 QUP 修补描述为已成功枚举；设备未被主动重启回REC。
