# iPadOS 18 首帧与服务启动修复

目标版本为 22H374 / Darwin 24，保留 services97 已有的亮度、八核、原生 HID、QCA6490、APM/TFA9873 和 Metal 接入。

## 源码和本次日志结论

- services97 的 native wifid 已可调用驱动，底层扫描得到 21 个 BSS；这不代表设置开关已正常。bootstrap 的 native MobileWiFi 设备枚举仍为空。
- IOMFBDisplay::is_ready 使用 SwapWait(0, 0x80000003)。旧版直接 BadArgument，阻止首帧进入。现在 0 查询锁下实际待完成队列，空队列真实返回成功；高位 readiness 查询在未完成时返回 Busy，避免阻塞合成线程。非零已提交帧仍在 CPU/GPU 交接和实际显示复制完成后退休，保留错误传播和表面持有。
- audiomxd 以 mobile 运行，StandardErrorPath 指向 root 所有目录时启动返回 EACCES。恢复 /dev/console，沿用 pstore；wifid/configd 同样恢复该路径，避免日志重定向改变启动行为。
- Wi-Fi bootstrap 增加按真实 IO80211Interface 与 BSD Name=en0 查询的入口，适配 DLIL 自命名接口尚未进入 BSD 匹配索引的阶段，不构造设备字典或宣称已连接。
- 删除 PongBootCapture 的内核重启/回 REC 实现及 ArmCpu 调用；启动镜像不再传递 capture 参数。控制台和 pmsg 的存储位置不变。

## plist 核对

576 项缓存 LaunchDaemons 均存在相同的单独 plist；检查 SpringBoard、backboardd、RunningBoard、lsd、configd、wifid、audiomxd 的真实执行路径和原生 MachServices。未发现需补造的关键 job。修复后无非 root 服务向 root 所有 /var/logs 创建 stderr 的配置。MediaConversionService 中两个 XPCService=false 来自原始恢复镜像，保留原值；不以补造服务消除告警。

本轮只改 OS 32 MiB 的脏块，保留现有 Data / SystemOS。主机构建结果和实际设备可用性分别记录；临时启动成功不等于上述功能已在真机确认。

## 主机完成记录

Darwin24 内核及 Wi-Fi bootstrap 已编译，启动镜像已打包；安装回执显示 32 MiB OS 差量写入完成，Data/SystemOS 保持原样。未运行测试、性能基准、镜像哈希或回读比较。打包中曾遇到磁盘不足，清理了可重建的旧 payload 中间文件，保留最终启动镜像、内核、源码和日志后重新打包成功。用户维护的基础构建脚本未改。

Fastboot 临时启动完成：Sending 2.328s / Booting 10.169s，均为 OKAY；未写永久 boot 分区。未启用自动重启，也未由主机带回 REC。界面、Wi-Fi 开关、系统音量及呈现效果等待本轮实际观察。
