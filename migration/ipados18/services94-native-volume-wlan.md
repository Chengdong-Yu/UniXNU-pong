# services94：系统音量接口与 WLAN 供电识别

## 实际阻塞点

services93 已建立原生 HID 报告设备，并接收物理音量键的按下/释放；系统音量仍无效。真实音频服务和 ASPL 已注册，但捕获中没有开始播放。Wi-Fi 则因 smpe3 首次型号读取为零而提前退出，尚未注册无线接口。

## 修补

- WLAN 型号/子类型识别与状态、电压、使能读取一起进入现有 100 ms 有界重试。始终要求真实 PMIC 型号和实际供电状态符合 Pong 配置，失败后保留具体原因，不伪造设备。
- 给物理音量键的 IOHIDUserDevice 设置 HIDDefaultBehavior，沿用 UniXNU 原生输入设备的默认事件处理策略。仍只发送真实 PMIC Consumer Volume Up/Down 报告，不直接调用调音量或绘制 HUD。
- 音频设备补齐旧设备级分贝范围、标量到分贝、分贝到标量属性，与现有原生音量控制对象以及 PCM 的实际软件增益使用相同量程。保留真实输出设备、原生控件和 mute 行为。
- 保留亮度修补、原生 wifid 管理端口、Pong 固件、八核策略及 GPU fence/地址窗口修补。

## 构建与设备结果

Darwin24 内核及 runtime-services94 编译通过；使用包含原生 IOHIDFamily/IOHIDEventDriver 的独立打包入口，用户维护的已有构建脚本未修改。72 MiB OS 差量安装完成，保留现有 Data/SystemOS；Fastboot Sending 2.328s、Booting 10.124s 均为 OKAY。未执行测试、基准、镜像哈希或回读比较。

自动 REC 抓取完成，console/pmsg 的本轮 ID、同步和 reset commit 均存在。日志确认 smpe3 及其他实际电源轨就绪；QCA6490 使用 Pong 活动槽固件，完成 MHI/QMI/WMI 初始化，发布 en0，扫描到 19 个热点。原生 IO80211 用户客户端被打开，传统 wifi.manager 查找失败未再次出现，但捕获仍只有初始 GET，没有设置端扫描/关联 SET；不能宣称系统设置或联网已可用。

audiomxd 连接真实立体声服务并发布 ASPL，物理键的原生报告设备仍存在。本轮未捕获物理音量按下或 StartIO，系统音量提示与出声效果待用户实际操作反馈。外部 CoreAudio 客户端的设备 35 选择结果仅作为该客户端的观测，不据此推定系统播放链路成功。保留原生媒体/音量控件，没有恢复直接调增益的自制按键路径。

亮度修补保持启用，没有本轮 DBV 拒绝。捕获中未出现内核 panic、GPU BV 故障或纹理 OutOfMemory；图形功能与延迟仍需分别评估。
