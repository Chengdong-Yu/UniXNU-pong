# services103：容量回归与外设、壁纸综合修补（未实机验证）

用户最初要求综合修补后再启动，随后明确要求“构建完就启动”。本轮按最新指示安装匹配运行时并临时启动。
services102 已在这项指示前临时启动，自动180秒返回和同步抓取完成。

services102 实际结果：平台 UUID 发布、contextstored 活动监测不可用分支生效；
WLAN128 MiB ordinary wired 分配仍失败；ADSP stage9返回-12，导致原生电池和音频来源未建立。
不能把该失败说成电池容量测量为零，也不能仅通过用户态上下文修补恢复真实电池数据。

## 本轮源码修补

- 电池：撤销“所有USB/状态/电压字段本周期全部成功才发布容量”的新增门槛。
  使用真实 capacity/gauge 的有效测量；充电状态去抖只影响 source，不阻塞容量和电压更新。
  保留掉线时最后真实测量与 stale 标志，不伪造百分比或充电状态。
- ADSP：PAS 元数据使用显式 host physically contiguous 内存，核对实际连续物理范围和36位上限；
  固件加载 bounce 改用显式 prepare 的 VM-backed descriptor，完成后配对 complete/release。
  仍使用 Pong 自己的 PAS1 固件及 carveout，不替换为 Sheng DTB。
- WLAN：64 MiB pageable descriptor 在Darwin24单对象96 MiB上限内，显式固定并逐页建立原生4KiB映射；
  IOVA长度、Linux分配边界与日志使用真实arena大小。WC属性和设备DMA隔离保留。
- 音频：22H374 VAD的 prts 端口查询改为 output scope，与原生端口查询调用对应；
  保留真实端口校验，不添加假的Hawking输入或直接拦截音量键改增益。
- 壁纸：services102 的 SBRendererService 在 NUDevice_iOS 创建 CI renderer 时 SIGABRT。
  将图片服务加入范围受限的22H374适配；Metal CIContext创建失败时，使用实际导出的
  kCIContextUseSoftwareRenderer 请求真实CPU CoreImage上下文。
  不关闭全局Metal、篡改fence或吞掉断言；CPU上下文同样失败时仍返回nil。
  MiniIOSurface真实VM-backed分配修补保留。

内核、运行时及候选boot已构建。候选包需配套runtime-services103 OS更新；
安装和启动结果以回执及本轮抓取为准，以上源码/构建结果不是功能成功证据。
亮度、原有八核/触控和Data保持。用户维护的userdata构建/CI脚本未改。
