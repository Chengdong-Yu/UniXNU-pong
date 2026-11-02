# services158：Safari 间接资源与 GPU 内存交接

## 设备证据

services157 的本轮 live CDC 与 REC pmsg 同时保留真实故障：FSR=0x80000402，FAR=0x10950b400，CP cause=1；随后共享设备拒绝连接0xe00002d8。故障前1024次提交/1023次完成，cache distrust=0，但累计维护141699MiB、12266450us，不能将低帧率简单归因于GPU频率或shader编译。FAR高于当前ABI1原生32位IOVA，落在运行时Metal逻辑地址空间，必须检查间接地址翻译/驻留；仅凭FAR不能断言具体是哪条shader指令。

## 接通实际 Metal 接口

Pong前端的compute/render `useResource` 与compute `memoryBarrierWithScope` 原为空实现。上游运行时已有实际residency、地址翻译表、资源引用和barrier逻辑，却没有被这些Objective-C接口调用。

本轮从前端、C ABI到Rust运行时完整接入buffer/texture的单个与批量useResources、heap单个与批量驻留、compute/render scope和resource barriers。旧render无stages重载明确覆盖vertex+fragment。资源使用属性与stage按真实枚举检查，外来/缺失对象拒绝命令；保留wrapper、IOSurface和heap至真正完成。runtime据此建立间接访问的guest→native地址表和生命周期，不把Metal逻辑地址直接当作SMMU地址，不伪造fence或忽略真实故障。

## GPU 路径复用已有性能方法

按官方GPU文档的系统内存/同描述符策略，将MiniIOSurface自己拥有的像素backing改为pageable、prepare锁定、write-combined内存entry；内核/客户端视图保持同一cache策略。在暴露及清零之前，完成Darwin24特有物理aperture缓存交接。外部wrapped pixels仍由调用方拥有，保留必要的缓存维护，绝不全局开启未经验证的硬件I/O coherence。

既有有限surface尺寸缓存、后台清零、Metal import缓存、retired BO缓存、异步pipeline/完成QoS、128IB提交、索引查找和锁外VM释放继续工作；新backing使GPU合成使用这些复用路径时无需反复维护整个缓存型surface。保留A730/SM8475硬件参数、32位ABI1窗口、原生4KiB SMMU、BGRA sysmem/nocb策略以及真实DMA/fence隔离。

## 文档覆盖与边界

已逐项检查官方GPU页的电源固件、上下文/stream、地址所有权、BO创建/导入/解除映射、TLB、提交、完成、Metal发布、编译链、BGRA和预热机制。已有实现保留；本轮改动针对上述实际缺口。A740/SM8550、Darwin26 ABI2及寄存器不套到Pong。跨进程MTLSharedEvent handle仍是已知缺项，不伪造支持或将所有Metal family标为可用。

编译、临时启动与实际设备观察分别记录。没有运行合成性能测试或镜像校验；修改OS仅写入刷新基线后实际变动的bands，Data128GB与配对保留。

## services158 实际回退与159修正

用户报告158启动冻结，未进入桌面。REC显示GMU/CP初始化、原生fence和cameracaptured Metal设备创建成功，随后在backboardd早期初始化阶段输出停止，没有可用的SMMU故障或panic backtrace。该轮全局GPU-owned IOSurface WC backing尚未达到可用状态，不能视为性能成功。159撤回这项MiniIOSurface backing/属性更改，恢复157已进入桌面的surface分配与缓存策略；保留独立的Metal资源驻留、heap与内存屏障修复和故障IB快照。明确保留外部缓存同步，不以跳过缓存维护换取帧数。
