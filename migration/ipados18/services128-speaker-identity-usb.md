# services128：单一 PCM 所有者、About 标识、USB 控制器和 GPU 维护范围

## services127 实机

用户仍未听到声音。REC console 证明前一轮 CoreAudio 只读状态页 SIGBUS 已消失，同一 audiomxd[99] 持续运行，真实 DSP 图和原生 IOAudio2 都 start 成功。旧 ASPL 同时发布了自己的 PCM 设备并尝试另行选择 VirtualAudio 路由，持续返回 -12986；不能以 OutputRunning 或滑块可用代替声音确认。

## 音频修复

- 成功发布真实 native Speaker 后，在已有共享控制状态明确标记原生输出所有权。旧 ASPL 仍做已验证的19.2MHz时钟适配，但不再发布第二个PCM设备或另行配置其路由。
- 保留 ASPL 的真实硬件回退：仅 native 端点发布失败时才暴露旧设备。此回退不与原生设备同时拥有同一DSP图。
- 旧兼容客户端关闭或调用start/stop不再能停止原生引擎；原生启停仍走真实 APM、功放与DMA流程。音量控件与HID路径保留。
- 从22H374 `AMCP::IOAudio2::Engine::write_data_to_stream` 确认状态页 +0x20 是提交到环形缓冲的独占结束样本位置，不是host时间。消费者只读取其已提交范围，避免读取未写位置或重放旧wrap。保留控制页RW修复及kernel无指针共享布局。
- 新增一次性的实际非零PCM消费和首个DSP WRITE_DONE记录，用于区分系统未送样本、DMA未完成和功放模拟通路；没有合成测试音或假完成事件。

主机编译成功不证明喇叭出声。本轮尚需设备观察非零提交、真实DMA完成及听感。

## 关于本机

用户最终指定型号代码 `MYMC3LL/A`，替代最初讨论的A3294展示值。名称保持iPhone 16 Pro，型号代码通过22H374 PSGAboutDataSource `_modelRegionString`，点击切换的监管型号通过 `_regulatoryModelNumberString`（A3083）提供。

SerialNumber是该数据源 `_setValue:forSpecifierWithKey:` 的真实键；仅替换此行值，保留原生其它信息。使用本机随机生成并固定保存的10字符合成序列号，存于guest `/pong/device-serial`，所有者root、0644。读取要求普通文件、正确长度、非组/全局可写。它是该移植系统的展示标识，不代表Apple原厂序列号/设备认证。原始Apple私有组件和生成值均不上传。

## GPU

保持已能进入界面的ABI1内核、bridge-services123和Turnip版本，不重引入曾导致UI/CP回退的ABI2全套替换。只对一次提交的同一缓存维护阶段，将重复或重叠的活跃虚拟地址范围合并后维护：不同VA的别名和后续提交仍保留操作。避免同一物理画面多视图产生重复的大范围CPU cache walk，不跳过一致性、不缩短fence判定、不返回虚假完成。性能收益未做基准。

## USB：真实 DWC3 firmware-handoff CDC 路径

新源 `drivers/PongUsb/PongUsbDevice.h` 使用Pong live DT的core 0xa600000、Apps SMMU SID0；GCC只接受SM8450/SM8475的0x49004/0x49018/0x49024状态，绝不使用nabu的0x0f000时钟组。参考：
https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/master/drivers/clk/qcom/gcc-sm8450.c

通用DWC3寄存器字段来自BSD-2-Clause-Patent NativeDwc3，许可证随源保留；板级初始化由Pong独立实现。通过现有Power构建单元包含，不修改用户维护的构建脚本。

- 异步启动，不阻塞OS启动。不满足真实GDSC/分支时钟、设备角色、Core ID、控制器停机、DMA和SID0隔离前提即返回。
- 仅复用已经由固件供电/初始化的Pong eUSB2 PHY与NXP repeater。没有把SM8150 FEMTO/QUSB2偏移写进Pong PHY，也没有猜测PMIC电气配置。
- 先停旧DMA，再分配独立低32位连续DMA页、使用共享SMMU lease锁独占SID0，并保留其它流。具备真实事件环、EP0、USB地址/描述符/配置、CDC控制请求与bulk传输。
- 收到真实host配置状态完成后才发布PongDwc3Console；没有注册空IOUSBDeviceFamily、Mux或HID端点。
- CDC只输出现有ramoops console，主机输入丢弃，不提供root命令执行。传输完成后才前进日志游标，保留故障/未知DMA寿命的页。
- 控制器/endpoint等待有界；无自动重启。连接阶段使用高/全速匹配packet size和实际短包。

**边界：该通路不等于Apple原生USB/Mux/Finder/文件传输后端。冷启动eUSB2/repeater供电配方及Apple设备族接口尚未完成。若固件未保留PHY/角色状态，本轮不会枚举，必须从新日志继续适配，不能称为USB已接通。**

## 配套部署

更新OS内音频插件、UI适配库及相同ABI的Metal bundle。保留设备实际dpkg状态（ElleKit已配置），没有发现除已安装ElleKit之外新增的真实包载荷。Data/SystemOS、init和Sileo helper保持，自动重启关闭，镜像哈希/读回及基准均未执行。

### 本轮部署结果

OS 的 9663676416 字节已全部写入，`install-services128/installation.json` 状态为 complete。最终临时启动使用 `boot-services128-final/pong-xnu-initial.img`，包含轮询 USB 事件环的中断屏蔽修正；Sending 2.320 秒、Booting 10.177 秒均为 OKAY。未写入 boot 分区，未重启回 REC。

启动后的主机 USB 枚举快照没有发现 Pong CDC 设备或 `/dev/cu.usbmodem*`。因此本轮不能认定 USB 已接通，也尚无该通路提供的 XNU 新日志。桌面、真实喇叭出声及性能变化待实机观察；Fastboot 成功只证明镜像已交给设备执行。
