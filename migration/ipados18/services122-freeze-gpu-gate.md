# services122：无 panic 冻结与共享 GPU 页表

## services121 记录

REC console/pmsg 显示 SystemOS、Data、launchd 已越过 services120 的分配失败；A730 SQE、GMU、ZAP、真实 CP fence、IRQ332 和 IOAccelerator 均成功。backboardd 随后记录真实 Vulkan device 创建成功，最后显示 GPU scanout 关闭/CPU 最终显示拷贝开启，没有 panic 或新的 CP 错误。用户报告系统冻结。Data 中本轮两份 PongBoot 图形启动快照为空，没有可用于确认内核栈的新记录。

## 实际修补

- 撤销 services121 新引入的 pageable BO 路径，恢复与原物理视图映射/逐页 WC 属性维护配套的 wired BO。保留已解决启动分配失败的 128 MiB 池上限及 main 的 native4K 分块/非缓存 I/O。
- 当前 ABI1 使用共享 GPU 页表。在实际编译的 `NativeAdrenoPublish` 包装层补回 Pong 原包装层已有的 map/free 前 GPU quiescence；此前修改未覆盖实际链接的包装层。轮询时释放全局 gate，睡眠期间不阻塞其他客户查询和退休 fence，重获锁后重新检查连接及硬件状态。
- 关闭客户连接前同样等待整个共享 ring，而不只等本客户的队列；在 gate 外等待，完成后持 gate 撤销映射。真实失败时保留资源，不提前释放仍在 DMA 中的 BO。
- 对无限期 fence 请求提供最长 500 ms 的一次内核等待，返回原生可重试 timeout；每次轮询核对真实硬件健康状态。没有伪造完成的 fence 或吞掉硬件错误。

日志仅能定位到图形初始化之后，尚不能证明上述某一项是唯一冻结原因。主机构建、临时启动、设备 UI 结果分别记录。不改 OS、SystemOS、Data 或用户脚本；自动重启保持关闭。

## 主机结果

最终内核构建日志 `out/ipados18/kernel-services122b.log`，对应驱动对象重新编译于最后一处关闭路径修补之后。预链接/镜像记录 `out/ipados18/boot-services122.log`。八核、128 MiB 可选 BO 池、CPU 最终 scanout 及真实 Metal 渲染配置保留。未运行测试、基准、镜像哈希或镜像回读。

Fastboot 临时启动 Sending 100112 KB / 2.351 秒、Booting / 2.026 秒均 OKAY。启动参数没有自动回收设置，手机留在运行状态。本轮没有 OS/Data 写入，是否恢复界面及消除冻结尚待实机结果。
