# services155：真实GPU接入后的慢帧修补

参考官方GPU、Adreno740/Metal、显示和4KiB文档，按SM8475/A730及22H374 ABI1适配。154已有真实scanout完成、908次commit/908次完成，首段未见CP错误；用户仍报告卡顿，不能把GPU接通当作性能完成。

## 本次实质改动

- 保留154的Darwin24零填充物理aperture一次性clean+invalidate。系统BO随后始终使用同一个WC VM对象；取消每次提交遍历这些系统BO每一页的属性重设，保留旧物理pager BO的修复、IB审计和真实故障保护。
- 从主线移入releaseSystemInto/collectFreedPages：真实fence、CPU视图和SMMU映射都退役后，在锁内移出页描述符和所有权索引，锁外complete/release，避免大BO回收挡住其他进程提交。
- 从主线移入已确认uncached导入页分类。使用Darwin24实际pmap属性及多DRAM银行管理状态，初次导入维护physical alias；只有全部页面确认uncached才省去逐帧全surface维护。普通cached IOSurface仍保留真实交接。
- publish/acquire/reassert按现有有序IOVA索引二分定位当前客户端窗口，停止遍历其他进程所有对象；保留alias、叶子、生命周期和TLBI检查。
- 查明当前实际编译Mesa缺失128IB批次补丁，只有16IB method5。恢复协商ParamsSubmitBatch及method17大小、隧道大小和128IB批次，避免每16个IB重复framing/提交/同步；老内核仍走原method5。保留当前主线100ms IRQ等待。补丁0007记录本次实际源码差异；CPU客户端的已知能力mask也接受本内核发布的SubmitBatch。
- 着色器预热按实际22H374 framework列表扩展，取消7库范围和32pipeline/30秒/5分钟退出限制。忙碌时保存函数进度并继续等待，空闲再恢复，不因一次动画永久终止整次启动的预热。后台QoS、90秒启动延迟、8MiB栈、8commit/250ms阈值和动画后2秒静默保留。已知会触发解压SIGBUS的压缩资源、replayer/debug资产仍不在render server中打开，实际使用进程按需加载不禁用。

## 文档各层对照

| 层 | Pong路径 |
| --- | --- |
| 固件、电源、时钟 | 真实A730 SQE/GMU/ZAP、板级OPP/HFI带宽票；不换SM8550固件 |
| 内存与映射 | 系统WC VM对象、锁外prepare/free、物理页所有权bitmap、exclusive batch、handle/IOVA索引 |
| TLB与提交 | 新无效叶映射、<=512页逐页解除失效/更多按ASID、提交无额外TLBI、真实fence/quarantine |
| BGRA | 153 GPU sysmem,nocb和GMEM load/store identity swap保留 |
| runtime/QoS | iOS完成/提交线程QoS、异步pipeline、空闲spare回收和有界BO重用保留 |
| shader cache | 原生Data持久缓存、实际build ID、嵌入22H374列表和逐函数续编译；不伪造预编译结果 |
| 显示 | IOSurface写入完成后NGP/PongSwap同序列完成，真实GPU opaque BGRA copy；不移植Sheng DPU寄存器 |
| ABI | Pong ABI1 method17=SubmitBatch；不把上游同编号priority/OwnedCoherent语义混入 |

官方A740双ring硬件抢占及Darwin26地址不直接用于A730/22H374。当前单ring尚无硬件优先级抢占，Vulkan高优先级请求不等于已经有抢占。仍不能保证120fps，设备观察必须分开记录。

## 部署

REC当前系统作为更新基线，仅10个变化band/80MiB，128GB Data、配对、铃声、输入法及外围驱动保留。未运行测试、基准、镜像哈希或回读校验；主机编译与真实GPU设备结果分开。
