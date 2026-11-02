# services109：VirtualAudio对象、系统输入法及停止定时重启

## 保留已确认的功能

用户在108确认Wi-Fi修复；保留原生接口完成握手、Pong固件、现有Data及内核。
音频原生UID优先与PowerUI注入继续保留，未重新撤回。

## 音频实际错误修复

108对端口查询返回who?。对照所选22H374的vaemGetVirtualAudioPlugin，
gCMSM+0x80是以com.apple.audio.CoreAudio.VirtualAudio bundle ID查询的插件AudioObject，
并不是VirtualAudioDevice_Default设备AudioObject。现有适配错误地把prts/rtcf发给了设备。

现在使用原生pibi/AudioValueTranslation的32字节输入/输出描述，核对返回对象的aplg类型；
prts全局scope与rtcf发送到真实插件ID，vcsp仍读取真实defaultVAD。
同一错误的ASPL插件fallback查询也一并修正。没有硬编码对象号、假端口或虚构输出设备。
真实端口不存在时仍返回失败，音频出声需后续实机确认。

## 输入法

22H374 OS仍包含TextInput_zh/en/emoji和键盘布局，kbd服务实际启动。
REC的mobile全局偏好只有AppleLanguages/AppleLocale，没有AppleKeyboards；
跳过Setup后没有建立输入法列表。SpringBoard只在列表缺失/为空时通过原生CFPreferences补入
简体拼音、英文QWERTY及emoji，不替换非空用户配置，不清Data。

音量HID此前声明完整PC键盘的字母和修饰键，却从不产生这些键；会向系统暴露不真实的
实体文本键盘，可能压制软键盘。现改为Consumer Control应用集合，仅真实音量加减，
精确SPMI匹配的DeviceUsagePairs同步改为12/1。原生IOHIDEventDriver支持Consumer元素，
依然由系统派发音量事件，不恢复自制音量HUD/直接写增益。

## 启动与边界

启动参数pong_capture_seconds=0，不再建立自动重启线程或设置REC目标。
保持现有console/pmsg路径与有界进程快照，临时启动后留在系统。
本轮运行时编译和预链接HID元数据更新；复用已完成的内核硬件修补，不称重新编译内核。
未执行测试、基准、镜像哈希或回读。用户维护的CI/构建脚本不变。

配套ABI中的实际key值已核对：allowed port types、category、mode。未替换为猜测的键名。
