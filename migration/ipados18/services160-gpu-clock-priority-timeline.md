# Pong A730：最高档、进程优先级与慢帧时间线

依据官方 GPU 适配文档及所固定上游 Adreno740/AdrenoMetal 源码，针对 22H374 / Darwin24 / Pong ABI1 实现；保留 services159 已恢复 GPU 呈现的 IOSurface 策略。

## 实现

- `pong_gpu_lock_max=1` 默认开启，强制选择 Pong 设备树第0档900MHz及配套DDR level11。沿用真实GMU HFI性能投票与ACK；固件保护不变。该记录是已请求档位，不冒充实测时钟。
- Metal队列优先级经实际Turnip导出函数、能力位4和新增方法18传到内核。ABI仍为1，方法17仍为128IB批量提交。首次显式请求可低于默认档，之后采用上游进程内最紧急请求策略。
- Darwin24使用真实`proc_get_effective_task_policy`及`IMP_TASK_POLICY_ROLE/DARWIN_BG`，不调用新OS的`task_get_gpu_role`。
- 单ring对低/普通/高优先级进程分别限制1/2/8个在途任务。返回真实Busy，由Turnip等待真实fence或短暂睡眠；不丢弃或重排已提交工作，不宣称硬件抢占。
- CP_REG_TO_MEM写入自有fence页中的64个真实起止时间戳槽；最多8个在途任务。`[gpu slow]`记录pid、角色、优先级、前序pid、CPU发布同步、完成侧同步、队列等待、CP执行和retire耗时。每秒最多8条。
- `Pong Metal slow`以真实commit/scheduled/completed时点记录前端及完成侧耗时，每进程每秒最多4条。保留实际未完成和错误状态。

## 编译与安装

Metal前端、Turnip及Darwin24内核均已编译并链接；80MiB/10个修改band已写入，Data/SystemOS未改动。Fastboot临时启动成功；真实慢帧及最高档ACK结果待设备日志。系统分区先从静止REC保存当前更新基底，再仅写修改band；不校验镜像哈希，不清空Data。

## 已知边界

当前是单ring准入优先级；A730上下文抢占仍需独立板级适配。缓存一致性仍按实际内存类别维护；不会为了降低耗时而对缓存导入全局关闭同步。Safari之前的高地址故障须继续通过真实设备观察确认，不能由编译成功推断已解决。

生成树修复：0002 CPU偏好已包含在0001中，不重复撤销；0003 rootful补丁改用真实位置的精确上下文，修复模糊应用导致重复函数定义的问题。未修改用户构建脚本。

## 设备实测

实际pstore确认最高档请求900000000Hz/DDR11，GMU返回ACK；这仍是请求/确认，不是物理时钟读数。USB实测能力与优先级方法18接通，backboardd实际priority0，海报实际priority3。71+166条有界慢任务记录中，backboardd大量作业CP只执行约0.4–0.9ms而CPU发布同步9–25ms、完成侧同步7–17ms；排队通常数到几十微秒。153/158的全局GPU丢失未在该捕获中出现。普通工作负载提交3072次时已维护575129MiB，累计46.6秒。不能把调高频率当作此CPU瓶颈的解决方案。

services161针对该结果增加活动导入资源的显式提交前维护。完成侧仍使用真实fence及保守同步，不回退至软件渲染掩盖GPU问题。
