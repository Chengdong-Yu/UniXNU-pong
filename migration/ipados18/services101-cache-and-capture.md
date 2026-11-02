# services101：WLAN缓存参数与自动REC抓取

## services100真实结果

REC/pstore已读取。本轮在发布原生IOAudio2扬声器后、WLAN分配内存时发生真实内核panic：
`IOMemoryDescriptor.cpp:379 cacheMode < sizeof(iomd_mem_types)`。
还没有启动audiomxd，不能以扬声器发布日志证明输出路由或出声成功。

源码与异常参数说明，问题是IOBuffer分配选项中的位重叠：
`kIOMemoryMapperNone=0x800`，映射缓存掩码为`0xf00`，WC为`0x400`。
传入两者后，pageable内存对象的映射收到了缓存类型12，超出Darwin24的类型表。
这是services100引入panic的直接原因；不能把services99同时出现GPU512MiB池和
WLAN分配失败，直接等同于证明物理内存耗尽。

WLAN现在先要求实际IOPlatformExpert已确定没有全局IOMapper；分配参数不再携带
与缓存位重叠的MapperNone。物理段查询仍明确使用MapperNone，SID1c01仍由本板级SMMU管理。
保留128MiB VM-backed固定页、WC与屏障、真实范围检查和总线主控隔离。
同一路径的页表/MSI辅助分配也移除这种参数混用。没有放宽XNU缓存断言或伪造分配成功。

## 用户授权恢复自动返回

用户最新要求下一版自动重启并自行抓日志，覆盖services98关闭自动重启的要求。
依照[官方抓取流程](https://unixnu.com/porting/log-capture)，新增
`drivers/PongBootCapture/PongBootCapture.h`，由已有ArmCpu启动点编入，不修改用户构建脚本。

- 仅`pong_capture_seconds=30..3600`、有效boot ID和目标参数存在时启用。
- 重新读取Pong REC DT的SPMI寄存器窗口、SDAM restart@48 reg与bits，确认沿用本板级位置；
  SPMI版本、实际APID与EE0所有权也在内核中检查。
- 本轮180秒后请求原生`reboot_kernel(RB_QUICK)`，保留文件系统同步。
- boot ID和阶段写入既有console/pmsg区域；目标REC或bootloader使用Pong的SDAM与IMEM编码。
- 如果正常shutdown超过30秒，独立线程尝试返回，并明确记录同步未确认；不会将这种结果
  标记为正常同步完成。所有CPU完全停止调度时，软件定时器仍无法保证返回。
- `capture-pong-boot.py`按原路径拉取`/sys/fs/pstore`，每轮独立目录，并检查文本boot ID与完成标记。
  不哈希镜像、不写永久boot、不以新旧镜像回读比较代替日志。

现有OS/Data、Pong活动槽无线固件、原生音频接入、亮度、八核与Metal功能保留。
功能可用性继续以本轮实际设备日志为准。
