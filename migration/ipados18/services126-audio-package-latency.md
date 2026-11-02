# services126：原生音频身份、包管理网络与 GPU 等待

适用 Pong SM8475 / A730 / TFA9873，iPadOS 18.7.10 / 22H374。以已确认能进入桌面的 services124/125 分配与 scanout 为基础。

## 本轮日志

REC services125：真实 GPU scanout worker 与前三帧完成已保存，后续未保存 services124 那种连续三次资源拒绝并退出的记录。不能仅由此宣布唤醒闪屏完全解决。保留的 console 后半段统计大量可选服务退出：lskdd 约957次，spaceattributiond/wifip2pd 各约100次，fairplayd.A2 约97次。帧统计约8–18 fps，单帧驱动时间常达30–50 ms；这些不是面板刷新率测量，不做虚构性能提升百分比。

Data 中真实 rootful 日志确认根卷可写、Sileo 2.5.1 注册有效、包环境配置脚本已执行；APT 更新反复失败于 `Could not resolve apt.procurs.us`。音频旧日志显示 physical native endpoint 与 ASPL 都连接，但 VAD 路由 `-12986`、vcsp=0，没有实际 StartIO/PCM 播放成功记录。外部 audio-route helper 还在它自己创建的 HALS_System 查询，返回无输出 port，不能用于证明实际 audiomxd 的路由。

## 音频修改

- 真实本机扬声器改用原生 `Speaker` UID、`bltn` transport；撤回将内置器件声明为 Thunderbolt 的旧兼容策略。核对 data-a-live 的实际 IOAudio2 contract，并保留已核对的22H374方法/格式ABI。
- 音量/mute 控件明确 `read only=false`；支持原生 stream memory type1 和现有22H374标识0x10000001。仍仅输出，不伪造 Hawking/Codec 麦克风或输入PCM。
- route helper根据实际 transport查询 `pspk` 或 legacy `ptbo`，优先真实 Speaker UID。先走当前 HAL 的真实 property setter，再仅在 audiomxd 宿主尝试原生 MediaExperience 配置。没有初始化的 gCMSM thunk可能不 dispatch 就返回0，不能把它当已接通；最终仍要求真实返回描述或原生成功及可用音量能力。
- 已有外部 route job 保持禁用；实际 ASPL 的宿主线程继续承担路由请求。自制音量键直接改增益路径没有恢复。

参考公开实现：[data-a-live / PandoraAudio.cpp](https://github.com/ShueiTsuki/data-a-live/blob/main/wsl/PandoraAudio.cpp)，下载副本仅保存在 ignored reference-services126，不复制 SM8850、CS35L43、TDM0 或工厂校准值。

## Sileo / rootful

- 配置完成标记改为持久Data `/var/.pong-rootful-configured-v2`，避免每次OS组件替换丢掉完成状态。dpkg-query确认已经安装的Sileo不再重复执行 `--configure` 并因已配置状态中断。
- 新增 `/pong/package-network`：从实际 configd 的全局DNS复制合法IPv4/IPv6地址至标准 `/private/var/run/resolv.conf`；无关联/DNS时保留旧数据，不硬编码公网DNS，不伪造网络已连接。原生 getaddrinfo确认仓库域名，最长有进程alarm限制。
- 首启APT更新前有限等候网络；Sileo受限setuid helper执行apt/apt-get前同步实际DNS。保留根拥有的Sileo可执行文件身份检查及普通XNU提权语义。
- 显式设置guest `/etc/resolv.conf`到常规run文件的别名；旧文件/别名先保留在rootful-stock，仅接受已知标准链接。没有修改原生configd DNS策略。此兼容文件不保证修复mDNSResponder/原生resolver自身的其他问题，仍须设备网络结果。
- 现有官方Procursus状态已包含真实APT、dpkg、ca-certificates、libiosexec、uikittools、firmware-sbin，保留签名仓库与真实种子索引；没有注册伪硬件capability包、关闭仓库签名或声称bootstrap等于所有tweak注入环境。Sileo包安装仍需实际事务确认。

## 性能

- IRQ等待建立期间重查真实中断generation，已经发生的完成中断不会继续等满1ms fallback；仍检查真实fence。没有禁用超时/缓存一致性、强制虚假fence或重新引入VM-backed分配试验。
- 针对本轮确证高频退出的lskdd、fairplayd.A2、wifip2pd设60s最小启动节流，保留二进制、MachServices和按需启动；不把它们永久停用。
- 已正常工作的wifid/configd不再强制OS_ACTIVITY_DT_MODE调试洪泛，audiomxd定向诊断保留；启动参数applog仅选本轮音频/图形/输入/Sileo目标。
- 保留GPU scanout=1、三来源映射/texture复用、原生120Hz模式、8核、实际SM8475电源/时钟与热限制；不拿固定高频声明代替实测。

## USB边界

上一轮实际Pong eUSB2/NXP repeater、SID0、时钟/供电描述继续使用；本轮把supply phandle解析为实际节点和电压、repeater reset解析到实际GPIO controller，修正pinctrl-names解码。NativeDwc3的nabu源码没有22H374 Apple设备族后端，不能把其CDC实验或Apple S5L驱动当成Pong原生USB。Finder/原生Mux仍未实现；本轮没有启用错误SoC复位/PHY程序，也没有发布同名空USB服务。USB尚未完成。

## 构建与部署

用户维护的scripts/CI本轮不修改。针对所选组件更新OS，保留现有init/UI/Metal/触摸/壁纸/亮度和Data/SystemOS；不清空数据。内核、ASPL、route helper、Sileo helper与package-network已编译/链接。无镜像哈希/回读/性能基准。安装与Fastboot结果完成后补记，任何音频出声、Sileo安装成功或性能提升都不能由主机编译推断。自动重启保持关闭。

部署结果：services126 OS 安装 state=complete；Data/SystemOS 保留，原有 Metal plugin/运行库不替换。Fastboot 临时启动 Sending 2.319s、Booting 10.176s 均 OKAY（out/ipados18/fastboot-services126.log）。未写 boot 分区，自动重启关闭，手机保持运行。原生 USB 后端仍未接通；实际音频出声、APT/Sileo安装与交互性能尚待此轮设备反馈。
