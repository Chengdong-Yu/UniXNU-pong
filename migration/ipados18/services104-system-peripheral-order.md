# services104：真实外设接入顺序

## services103 实机结果

180秒同步后自动回 REC，console/pmsg 的 boot ID 和提交标记匹配。
ADSP 从 stage9/10 到11/12/13，APM ready；TFA9873 revision b73 和实际 SID1801 PCM 服务发布。
物理电池重新发布100%、4428 mV；容量来源恢复，不再把未发布电源视作0%。
充电状态仍随真实固件状态1/2/3变化，尚不能以 source log 认定电池设置页全部恢复。

WLAN64 MiB固定页/SID1c01映射、PCIe MSI、Pong amss20与bdwlang固件成功。
en0注册，102个硬件channel，首轮扫描到10个热点；原生IO80211 client有真实请求。
bootstrap仍停在native manager deadline，无Power set请求、关联或DHCP证据。
IOAudio2 client打开，但VAD没有确认输出端口，无StartIO/出声证据。
本轮未见新的PosterBoard/SBRendererService退出，用户实际壁纸选择与电池页仍待确认。

## 接续修补

- Wi-Fi helper 原顺序在同步 WiFiManagerClientCreate/CopyProperty/SetPower 之后才发布en0与启动SC配置。
  services103中manager超时，该分支实际永远未运行。现于真实en0到达后，先启动独立15秒SC工作进程，
  发布真实接口并配置，然后主进程请求原生管理器；保留飞行模式及真实接口核对。
- 音频host路由原只查PongAudioDevice旧ASPL。实际PongSpeakerNative已经被CoreAudio打开，现优先使用
  原生UID且要求alive/输出stream真实存在，再回退原旧接口。未伪造端口或麦克风。
- PowerUIAgent对外部电源的同步CoreDuet查询可阻塞电池页。已核对22H374实际BOOL方法ABI，
  改为从真实IOPMPowerSource的ExternalConnected读取；不存在有效实测属性则保留原实现。
  不返回固定充电状态，不停用电池页。dyld回调只解析已加载IOKit导出，不递归dlopen。

本轮复用services103内核；只更新匹配运行时和启动参数，保留Data/原亮度/八核/触控/Metal。
用户最新授权构建后临时启动，继续自动REC抓取；成功判定仍需实际系统接口结果。

## 实机回归

用户报告104未进入界面。REC仍有本轮boot ID，但没有自动同步/提交重启标记，不能称本轮自动抓取正常完成。
实际日志：ADSP/channel、物理电池、CP fence仍成功；真实en0到达发布与SC配置均返回0，
MobileWiFi管理器仍deadline。音频路由选出的physical从41变为33，与current33相同；
渲染器c0/d0/p0，无画面提交。没有证据证明这就是因果，但不应保留未验证默认路由变化作为稳定配置。
105撤回默认原生UID优先和PowerUIAgent注入（实现源码保留），保留103硬件分配修复及104实际执行成功的SC顺序。
同时使用已有两次有界SpringBoard/backboardd快照，若仍无画面可取到具体等待栈；未增加永久轮询或替代服务。
