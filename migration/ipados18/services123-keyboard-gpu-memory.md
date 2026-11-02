# services123：输入交互期间的 GPU 内存与 Metal 复用

目标系统：iPadOS 18.7.10 / 22H374，Pong SM8475 / Adreno 730。

## 设备证据

services122 用户确认可进入桌面，但输入法慢，连续输入可能冻结。REC console 中 backboardd 和 SpringBoard 的真实 Vulkan 设备创建成功，CP 初始化无失败，随后 Metal 遥测反复记录 `texture: OutOfMemory(Device)`。已有日志没有证明 kbd 自身崩溃；35 秒保存的进程栈也不是冻结时刻的栈。因此本轮先处理可见的资源分配和帧间导入问题，不能据此宣布输入法问题已解决。

## 本轮改动

- 保留 services122 的共享 GPU ring 退休规则、锁外有界等待、ABI1 和匹配的旧 Turnip/translator 组合。
- 普通非连续 BO 在 128 MiB 物理池耗尽后，使用 IOKit 的 VM-backed memory entry。内核和客户端通过同一个 descriptor、同一种 write-combine 属性访问；GPU 持有期间 prepare/wire，真实退休后 complete。避免大 BO 继续耗尽 KMA_DATA，也避免 services121 中 VM backing 与物理客户端 pager 混用及后置缓存属性修改。
- 从 NativeMetal `0f11e64` / `547312c` 移植帧间 IOSurface 导入复用。缓存持有实际拥有 IOSurface 的 buffer，最多 64 项/64 MiB/1 秒；内存压力或分配失败时回收（pressure dispatch source 由进程内 static ARC 引用持有），可选缓存释放放在缓存锁外，真实在途 GPU 所有权保留。
- 上游 Metal protocol conformances、command-buffer userDictionary、texture-buffer alignment 接入当前真实对象/运行库，不伪造 GPU 核数或执行结果。
- 普通纹理/缓冲区在真实 OutOfMemory 后仅回收可选导入并重试一次，不无限重试或隐去最终失败。
- 从 `11420f4` / `805659b` 移植 8 位合成输出策略，使用实际 22H374 QuartzCore UUID `FAF7D7EA-18F0-3C2D-816C-AC56D16C40A3` 和其自身选项表。只写匹配映像的 RW 数据，不修改 VM 权限：force-FP16=121，EDR-mirror=146，display-headroom float=20。

## 分支适配界限

已拉取核心 `feat/ipados27-gpu-parity`，当前为 `87bf1f920b5ab40afb70012cb7ce080d55a2cceb`；NativeMetal 对应 `9edbe61cf2bd2273730d64eec11c23f40db8f57d`。

22H374 的 bool 选项表只有 199 项，没有 iPadOS27 的 `CA_DISABLE_BACKDROP_COMPUTE`（276）；其 `CA_FP16_MIN_HEADROOM` 选项也不存在。不写这些新系统地址。已有 Pong BGRA 输出修补保留。需要新 translator / ABI2 的改动保留在先前候选快照，避免重新引入 services114–119 的启动回归。此次使用匹配的 ABI1 Turnip 二进制，不将 ABI2 库混入系统。

用户维护的构建/CI 脚本本轮未修改；Data/SystemOS 不重置。由于 OS 在设备上可写，安装新 Metal bundle 使用完整逻辑 OS 传输，不能把本机局部元数据增量覆盖到设备已改动的 APFS。保留原有 helper/UI/audio/Sileo，只替换 Metal bundle 和本轮内核。自动重启仍关闭。

## 验证状态

源代码/22H374 ABI 与选项表审阅完成。Darwin24 内核、最终 Objective-C++ frontend、匹配 ABI1 Rust 运行库和 22H374 plugin 编译/链接完成；最终 frontend 重新链接至本轮 archive。最终 plugin 直接链接并打包本轮 bridge，保留匹配 ABI1 的 Turnip。段匹配复查确认 QuartzCore 选项在可写 __DATA_DIRTY，已修正后重建。首次 OS 传输在未提交 manifest 前中止，最终完整安装以 install-services123-final 为准。安装和 Fastboot 结果另补记，设备输入法流畅度、连续输入和内存压力行为仍需运行观察。按用户要求不进行镜像哈希、回读校验或基准测试。

完整 OS 安装已完成。随后在任何 guest 启动前，以 install-services123-final 完整安装为基线更新 pressure source 的 ARC 生命周期，只按 band 元数据生成局部传输计划；设备尚未修改 APFS，不做镜像内容比较或回读。最终安装收据为 install-services123-pressure。

最终部署：9 GiB OS 完整写入后，在首次 guest boot 前追加 72 MiB pressure-source 生命周期更新；两份安装收据 complete，Data/SystemOS 保留。`out/ipados18/fastboot-services123.log` 记录临时启动 Sending 2.332 s、Booting 10.176 s 均 OKAY。未刷 boot 分区，自动重启关闭，手机保持运行；进入桌面与连续输入是否修复仍待实机结果。
