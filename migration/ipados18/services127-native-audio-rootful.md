# services127：原生音频共享页、充电状态与 rootful 注入库

## 实机依据

services126 已由用户确认音量滑块和两个音量键可用，但仍无声音、充电状态异常。REC console 显示真实 `PongAudio: stereo PCM output started`、`PongAudio2: native output start result=0`，随后 audiomxd[90] SIGBUS 退出。Data 的原始 `audiomxd-2026-10-30-013934.ips` 指向 CoreAudio `HALS_IOA2Engine2::_FirstIOThreadStarted`，写入状态映射地址 +0x18 触发 KERN_PROTECTION_FAILURE。

检查供给的 22H374 CoreAudio 对应函数可见它写 status+0x18 和 +0x20。原来驱动将 type0 状态页映射为只读，造成第一次真正开始 IO 的服务器崩溃。type0 改为共享可写，保留专用4KiB分配、时间戳和客户端字段，不映射内核指针。Speaker/builtin 身份、已可用的 gain/mute 和 HID 路径保留，没有增加假麦克风或测试音。

## 电源上报

原始日志显示 external=1、status 从 discharging 到 charging 最后 FULL，充满时 VBUS 仍4711mV。不能把充满视为继续实际充电，也不能把 ONLINE=0（充电策略停止取电）直接视为插头拔出。

- 以完成周期的 fresh mask 区分当前充电读数与历史值。
- ONLINE=1 或新测量 VBUS 能确定外接电源；ONLINE=0 且 VBUS 读取失败时保留上次真实状态。
- 失败读数和两次周期去抖期间，保留此前真实 charging/full 标志，避免凭空清除充电图标状态。
- 外接电源且充满时 TimeRemaining=0，不再报告未知的放电时间。
- 容量刷新不依赖可选USB字段。固件仍独占充电控制，不写充电电流、温控或限流。

## Sileo / jailbreak 环境

用户要求本轮不处理域名解析。保留既有 DNS、APT 仓库签名、实际 Procursus 状态和 Sileo setuid 路径。

补入官方 https://ellekit.space/pool/ellekit_1.2_iphoneos-arm.deb 的真实 rootful runtime：libellekit、libinjector、MobileSafety、Substrate/libhooker API 别名、真实包文件清单和 dpkg record。包本地版本为 `1.2+pong1`，表示Pong修改了签名页和加载集成，未冒称为未改动官方包。Mach-O重新签名到4KiB。保留源许可、原始上游postinst；Pong postinst改用本板已有kern.exec_inject，避免远程暂停/注入PID1以及覆盖既有UI适配。

`userland/rootful_tweaks18.py` 只安装到已挂载的本机 rootful OS 副本，并拒绝覆盖已安装的其他注入框架。没有更改用户维护的 scripts/CI。`PongTweakConfig.h` 由static init和tweak-refresh共同使用，拼接规则，保留UI库。

默认显式应用目标：SpringBoard、Preferences、MobilePhone、Sileo。ElleKit继续按tweak自己的plist过滤；其他应用可在root拥有的 `/private/etc/pong-tweak-processes` 追加其可执行文件名，再以root调用 `/pong/tweak-refresh`，下一次应用启动生效。拒绝PID1/xpcproxy注入目标，边界错误保留已有UI规则。该集成不宣称已覆盖任意守护进程、任意新应用或所有旧tweak的Darwin24兼容性。

恢复开关：REC中在iOS Data根目录建立 `.pong-disable-tweaks`（iOS路径 `/var/.pong-disable-tweaks`）后重启；原生UI适配继续加载。ElleKit自身 `.eksafemode` 也被保留。首启任务只配置实际staged包，没有假capability包、假jailbreakd或关闭APT认证。

可复用的准备入口（路径由本机传入）：

```sh
python3 userland/rootful_tweaks18.py --package /path/to/ellekit_1.2_iphoneos-arm.deb \
  --out /path/to/new-stage --runtime /path/to/runtime --developer /path/to/Xcode.app/Contents/Developer
```

安装入口为该模块的 `install(mounted_guest, stage, runtime)`；static init仍由现有 `compile_init` 构建。所有二进制和输出留在忽略目录。

## 构建与部署边界

内核和static init/tweak-refresh已编译；音量HID、已安装audio ASPL/UI/Metal、壁纸、键盘、亮度、Data/SystemOS保持。USB后端仍未完成，本轮没有声称实现USB。

不进行镜像哈希/回读、测试或性能基准；自动重启关闭。编译/安装/Fastboot成功分别记录，不能视为已确认出声、充电图标或tweak安装运行成功。

部署完成：`install-services127/installation.json` 为 complete，仅更新OS。实际guest包状态中新增记录均为firmware工具生成的gsc/cy+等能力元数据，没有发现另装的注入框架；这些真实记录以及Sileo已配置状态保留到了候选OS。

最终启动镜像 `out/ipados18/boot-services127-final/pong-xnu-initial.img` 使用最后一次成功的 services127b 内核。Fastboot Sending 2.319s、Booting 10.181s 均 OKAY，总15.835s，见 `out/ipados18/fastboot-services127.log`。未刷写boot分区，没有自动重启，手机保持运行。实际声音、充电图标及ElleKit tweak加载仍需本轮设备观察，不能从Fastboot成功推断。
