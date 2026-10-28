# Pong：Darwin 22 → Darwin 24 差异

依据本机 iOS 16 可启动源码与 `xnu-11417.140.69` 的实际接口对照；不将旧私有地址直接用于新 restore。

| 路径 | iOS 16 基线 | iPadOS 18 所需处理 | 当前状态 |
| --- | --- | --- | --- |
| UEFI boot_args | 608 字节命令行，结构大小 0x2e0 | 1024 字节命令行，flags 0x470，实际内存 0x478，结构 0x480 | DarwinBoot24 已生成 |
| CPU 注册与启动 | 注册 CPU 后建立 GIC 控制器，再 initCPU / processor_start | ml_processor_register 会直接 processor_boot；会提前回调尚未准备好的 ArmCpu::initCPU | 首次 panic 已定位；Pong 延迟首次 boot 到所有向量就绪，并通过 processor_boot / processor_wait_for_start / ml_cpu_init_completed 完成新调度器流程 |
| CPU 驱动编译 | 固定 iOS 16 target | 跟随当前 XNU 的 deployment target 和 Darwin major | 构建器已改为从构建环境读取 |
| 调度器与 VM 配置 | 旧 timeshare / 内存策略 | 新 timeshare core、deferred reclaim、map ranges 等 | 已补入 Darwin 24 配置 |
| 用户地址与共享缓存 | 旧整数 VM 参数 | 新 VM unsafe 类型和共享缓存映射结构 | 保留新实现，vm_unix 单文件编译选项须保留 VM_UNSAFE_TYPES |
| 进程凭据 | 可直接访问的旧 cred 路径 | 新不可变凭据更新接口 | persona / exec 处理按新接口移植 |
| 存储闭源 ABI | 20G81 IOStorageFamily vtable / metaclass 地址 | 从 22H374 与公开 IOStorageFamily-323 生成绑定，适配 LTO 构造 / 析构 | 已生成新的 NativeUfs 库 |
| APFS 镜像 | GPT 包装的旧系统布局 | 22H374 OS 为裸 APFS；卷 UUID 与镜像尺寸全部改变 | 驱动兼容裸 APFS，新 SHNGROOT 窗口已安装 |
| launchd / dyld | 20G81 原版及拆分缓存 | 保留 22H374 原版，配套 61 个缓存文件与新 Data seed | 已生成并安装 |
| UI / Metal | 20G81 QuartzCore / Settings / GPU 私有 ABI | 必须按 22H374 重新适配，旧地址不能通用 | services12 接入 22H374 原厂 CPU 合成回退与真实进程容器路径，等待真机帧输出确认；GPU 绑定待移植 |

## 首次真机 panic

REC 保存的日志确认运行的是 Darwin 24.6.0、`Pong/22H374` 启动参数。故障 PC `0xfffffff007a83400` 对应 `ArmCpu::initCPU(bool)+0x3c`，FAR 为 `0`。回溯依次经过 `ml_processor_register → processor_boot → cpu_start → cpu_machine_init → PE_cpu_machine_init → ArmCpu::initCPU`。

故障发生在控制器创建前，尚未到 APFS 挂载或用户态。修复实际启动顺序，保留真正的 panic 与调度器启动状态检查，不把空指针调用改成无条件成功。

日志保存在本机忽略目录 `out/ipados18/logs/boot-first/`。

## 第二次真机结果与 APFS 修复

`cpu2` 日志确认 CPU 首次启动、GICv3 / CNTV、UFS 三个窗口和 APFS 根卷挂载均成功。后续的 `helper exec result=2` 来自挂载钩子仍使用 iOS 16 的根卷 UUID：根卷绑定失败导致 SystemOS 与 Data 均被跳过，Data 中的 init 因此不可见。

构建器现在从同一份实际卷元数据读取 **OS / SystemOS / Data 三个 UUID**，生成供 kern_exec 和 vm_unix 使用的同一头文件；启用的挂载路径中不再保留 iOS 16 UUID。配置缺失时终止构建，不回退到旧值。APFS 的 snapshot 警告出现在成功的根卷挂载之前，不是该次 init 缺失的直接原因。
