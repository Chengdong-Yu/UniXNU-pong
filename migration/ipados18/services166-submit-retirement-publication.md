# Pong：原生提交与BO回收的fence发布

165第二次真实启动中壁纸进程退出，CP嵌套IB访问未映射地址0xe0d4100，整机GPU进入DeviceLost。保留真实故障和资源隔离，不清除SMMU故障伪装成功。

审计Darwin Turnip发现：`darwin_submit_chunk`在未持有dw.lock时进入内核，硬件接受并返回后，外层`darwin_queue_submit`才拿dw.lock更新dw.last_fence。与此同时`darwin_bo_finish`持retire.lock并在dw.lock下取last_fence，可能取到已完成旧值，随后把仍在执行任务可达的BO放入可复用缓存。内核保留根IB的引用不能保护用户态提前覆盖的嵌套流/数据。

把一次实际Submit ioctl与成功fence发布放进同一dw.lock区间，回收snapshot使用同一锁；外层不再重复记fence。只覆盖一次内核调用，不把Busy重试/等待包在锁里。无新fence伪造、无同步删减、无关闭BO重用；保留共享shader缓存、live优先级、64页缓存维护批处理、真实FIFO/完成和全部165 Metal适配。

修改有明确生命周期竞态依据，但具体导致本轮硬件故障仍需实机结果；不把可能性写成已证明唯一根因。补丁：`patches/mesa/0013-pong-atomic-submit-retirement-publication.patch`。

最初候选计划复用165内核；用户随后要求再次合并最新分支，最终166已重新编译A730/Darwin24内核、Metal前端、Rust运行时和Turnip，包含最新跨层帧诊断。最终启动文件为PongDev的`build/services166/boot-final/pong-xnu-initial.img`，旧`boot`目录不作为本轮候选。165-final原生音频时钟/时间换算保持安装。REC刷新当前OS后仅写112MiB修改band，保留Data128GB/SystemOS。自动重启关闭。
