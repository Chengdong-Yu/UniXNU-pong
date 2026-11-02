# services156：Metal接口与界面线程卡顿

155设备中真实GPU scanout完成，backboardd 827次提交/827次完成，没有本轮CP故障，但用户仍报告明显卡顿。普通启动commit到完成均值约28.5ms；不是受控基准，也不说明已满足高刷新率。

## 实现

1. Metal异步render/compute pipeline和function specialization入口原先在调用线程同步编译并立即回调。本轮复制可变descriptor/constant values，在user-initiated后台执行，保留底层bounded compiler pool、真实错误和对象引用。提供缺失的compute descriptor短入口及其异步入口，不落入未初始化的Apple硬件父类实现。
2. 前端自身GPU完成等待/handler线程从默认QoS改为user-interactive，与官方运行时iOS策略一致；独立缓存回收保持utility。所有fence仍来自实际GPU，不提前报告完成。
3. 合并上游heap texture/buffer size-and-align查询，经C ABI调用真实runtime/Vulkan内存要求，完善heap放置接口，拒绝时不编造size/alignment。添加IOSurface只读linear texture alignment查询。旧MTLFeatureSet依据实际supportsFamily返回，不强制全NO，也不伪造Apple family能力。
4. 添加上游声明常量零值预热路径：枚举该函数实际声明的scalar/vector常量，为预热variant设置相应类型的零值，不改变应用显式指定常量的原生入口；不认识的类型明确拒绝。连接此前只有category声明、没有实现的newFunctionWithZeroConstants。
5. 设备持久prime日志证明184路径存在，但旧轮只处理一个TextInputUI库。可预热系统framework大多仍有UF_COMPRESSED，曾在render server解压时SIGBUS。本机按实际安装列表展开141个framework metallib，共240,059,151字节，保留原文件权限/时间和AIR内容；replay/debug库不展开。资源替换和Data缓存分开，保持空闲预热与逐函数journal。
6. 实际日志cameracaptured每约5秒超过27MiB high watermark被jetsam，反复加载真实Vulkan/Metal。只为该原生job配置192MiB的有界Jetsam MemoryLimit，同时更新其原生plist和launchd缓存；保持可执行文件、IPC和真实摄像头错误，不伪造摄像头服务，也不关闭其系统按钮。

保留155内核的初始物理缓存交接、系统WC描述符、按窗口索引、锁外回收，以及恢复的128IB原子提交。保留BGRA系统内存GPU渲染、真实scanout、亮度/USB配对/WLAN/音频/键盘及128GB Data。

## 限制与验证

运行时依然有真实feature gaps，例如linear texture全范围、部分tessellation/indirect和fast resource loading；部分Apple私有查询语义未知，未把这些字段强制改为支持。A740硬件抢占与Darwin26地址没有照搬到A730/22H374。

主机release Rust、前端和插件已编译；当前OS从REC准备增量更新，只写变化band。实际设备GPU完成、服务退出与呈现耗时单独记录，不运行测试/基准/镜像哈希或回读对比。
