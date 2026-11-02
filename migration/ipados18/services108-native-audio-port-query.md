# services108：保留已恢复首帧路径，对齐音频端口查询

107自动抓取确认：InterfaceNamer原生超时完成通知解除wifid/SpringBoard等待，出现首个可见帧与持续swap。
用户要求的原生音频UID优先和PowerUI注入均保持，Data不重置。

## 实际ABI差异

107物理输出device33与VAD62仍没有确认端口路由。现有适配器以outp scope查询prts，
而22H374原生vaemThunderboltOutputIsConnected→vaemGetVADPortIDFromVADPortType明确使用glob scope，
由ptbo qualifier筛选输出端口。现将同一真实查询对齐原生scope，不制造端口或改系统音量UI。
检查实际返回长度与CFArray类型；无输出端口时保留失败，不发起空路由或声称已出声。
保留真实错误码并限制错误日志数量，便于区分ABI查询错误与未发现物理端口。

本轮仅运行时重编译、OS差量安装与预链接打包；使用services103已完成的内核硬件修复。
底层Pong固件、DMA、ADSP、TFA9873、亮度、八核、Metal与PowerUI均不撤回。
Wi-Fi的CoreLocation同步等待、Shortcuts编码崩溃和BlastDoor sandbox依赖仍为后续问题，
不以首帧或29个扫描热点冒充系统Wi-Fi开关/关联及音频已通过。

## 本轮实机结果

108自动同步回REC完成。用户确认系统Wi-Fi已修复；开关/菜单可用性以用户结果为准，
关联、DHCP和吞吐量不扩展声称。显示继续出现首帧。
用户确认仍无音频、缺系统内置输入法，并要求下一版不再自动重启。
实际PMIC音量按下/松开报告已传入HID，report result=0；这仍不等于音量路由有效。
全局scope修正后prts返回2003332927（who?/未知属性），继续反查确认此前选错对象：
vaemGetVirtualAudioPlugin以pibi/32字节AudioValueTranslation取得插件ID写gCMSM+0x80，
prts/rtcf用该插件ID；vcsp才在defaultVADID上读取。109修正插件查询ABI和请求目标。
